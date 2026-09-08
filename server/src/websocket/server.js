/**
 * websocket/server.js — OmniDeck WebSocket 通信引擎
 *
 * 设计要点：
 *  - 与 HTTP Express 共享同一个端口（默认 8080），路径 /ws
 *  - 客户端分两类：
 *      * device — ESP32-S3 终端固件（URL: ws://nas:8080/ws?role=device）
 *      * web    — Vue 3 管理界面（用于实时刷新状态 / 屏幕镜像）
 *  - 连接建立时立即向该客户端推送 FULL_SYNC 全量数据包
 *  - 任何 REST 增删改成功后调用 broadcastSync() 向所有客户端广播 SYNC_UPDATE
 *  - 收到 device 上报的事件（alarm_triggered / heartbeat / env）后：
 *      * alarm_triggered → 调用 AI 播报引擎生成"日期+温湿度+今日日程"语音流并下发
 *      * env             → 记录最新温湿度，供 /api/status 与 Web 端展示
 *  - 屏幕镜像：固件周期性上报 mirror 状态快照，Web 端实时渲染
 */
const { WebSocketServer } = require('ws');
const path = require('path');
const fs = require('fs');
const db = require('../db');
const { generateBroadcastSpeech, ttsSpeak } = require('../ai/broadcaster');

// 最新一次硬件环境上报（内存态，重启后等固件下次心跳即可恢复）
const deviceState = {
  online: false,
  lastSeen: null,        // ISO 时间
  temp: null,            // °C（SHTC3）
  humi: null,            // %RH
  battery: null,         // 电池百分比（ADC 采样）
  mirror: null,          // { status, schedule, countdown, ai } 屏幕镜像快照
};

/* 每日励志名言库（按日期轮换，设备端显示在"每日一言"卡片） */
const QUOTES = [
  '千里之行，始于足下',
  '不积跬步，无以至千里',
  '自律给我自由',
  '今天的努力是明天的底气',
  '天道酬勤，力耕不欺',
  '心之所向，素履以往',
  '你有多努力，就有多幸运',
  '把优秀变成一种习惯',
  '路虽远，行则将至',
  '事虽难，做则必成',
  '青春须早为，岂能长少年',
  '不畏将来，不念过往',
  '越努力，越幸运',
  '星光不问赶路人',
  '滴水穿石，非一日之功',
];

/** 按自然日轮换选择一句名言 */
function todayQuote() {
  const days = Math.floor(Date.now() / 86400000);
  return QUOTES[days % QUOTES.length];
}

/** 组装全量同步包：周日程 + 置顶倒计时 + 激活闹钟 + 待办 + 服务器时间 + 每日一言 */
function buildFullSyncPayload() {
  const schedules = db.prepare('SELECT * FROM weekly_schedules ORDER BY day_of_week, start_time').all();
  const topCountdown = db.prepare('SELECT * FROM countdowns WHERE is_top = 1 LIMIT 1').get() || null;
  const alarms = db.prepare('SELECT * FROM alarms WHERE enabled = 1').all();
  // 未完成的在前，最多下发 8 条（屏幕容量）
  const todos = db.prepare('SELECT id, text, done FROM todos ORDER BY done ASC, id ASC LIMIT 8').all();
  return {
    type: 'FULL_SYNC',
    ts: Date.now(),
    server_time: Date.now(),                 // 毫秒级 Unix 时间戳
    tz_offset_min: -new Date().getTimezoneOffset(), // 时区偏移（分钟，东八区=480）
    schedules,      // 全部周日程（固件端按本地星期过滤）
    countdown: topCountdown,
    alarms,
    todos,          // 待办事项
    quote: todayQuote(),
  };
}

class WsEngine {
  constructor() {
    this.deviceSockets = new Set(); // ESP32 连接
    this.webSockets = new Set();    // 浏览器连接
  }

  /** 挂载到已有的 HTTP server（与 REST API 共享端口） */
  attach(httpServer) {
    this.wss = new WebSocketServer({ server: httpServer, path: '/ws' });

    this.wss.on('connection', (ws, req) => {
      // 通过 query 参数区分角色，例如 /ws?role=device
      const url = new URL(req.url, 'http://localhost');
      const role = url.searchParams.get('role') === 'device' ? 'device' : 'web';
      ws._role = role;
      (role === 'device' ? this.deviceSockets : this.webSockets).add(ws);

      console.log(`[WS] ${role} 已连接 (当前 device=${this.deviceSockets.size}, web=${this.webSockets.size})`);

      // 设备上线：更新状态并通知所有 Web 端
      if (role === 'device') {
        deviceState.online = true;
        deviceState.lastSeen = new Date().toISOString();
        this.notifyWebStatus();
      }

      // 新连接立即收到全量数据
      this.safeSend(ws, buildFullSyncPayload());

      ws.on('message', (raw) => this.onMessage(ws, raw));
      ws.on('close', () => {
        (role === 'device' ? this.deviceSockets : this.webSockets).delete(ws);
        if (role === 'device') {
          deviceState.online = this.deviceSockets.size > 0;
          deviceState.lastSeen = new Date().toISOString();
          this.notifyWebStatus();
          console.log('[WS] 设备离线');
        }
      });
      ws.on('error', (err) => console.error('[WS] 连接错误:', err.message));
    });
  }

  /** 统一的下行消息处理入口 */
  onMessage(ws, raw) {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      return; // 忽略非 JSON 帧
    }
    deviceState.lastSeen = new Date().toISOString();
    deviceState.online = true;

    switch (msg.event || msg.type) {
      // ESP32 上报温湿度 + 电池（SHTC3 / ADC）
      case 'env_report':
        if (typeof msg.temp === 'number') deviceState.temp = msg.temp;
        if (typeof msg.humi === 'number') deviceState.humi = msg.humi;
        if (typeof msg.batt === 'number') deviceState.battery = msg.batt;
        this.notifyWebStatus();
        break;

      // 心跳
      case 'heartbeat':
        this.safeSend(ws, { type: 'PONG', ts: Date.now() });
        break;

      // 设备请求时间同步（设备以 NAS 服务器为时间权威）
      case 'time_request':
        this.safeSend(ws, {
          event: 'time_sync',
          epoch_ms: Date.now(),
          tz_offset_min: -new Date().getTimezoneOffset(),
        });
        break;

      // 屏幕镜像快照：固件把当前 UI 状态（文本摘要）上报，Web 端 1:1 渲染
      case 'mirror':
        deviceState.mirror = msg.data || null;
        this.notifyWeb('MIRROR_UPDATE', { mirror: deviceState.mirror });
        break;

      // 闹钟触发：调用 AI 生成播报文案 → 推送 TTS 音频给设备播放
      case 'alarm_triggered': {
        const temp = typeof msg.temp === 'number' ? msg.temp : deviceState.temp;
        const humi = typeof msg.humi === 'number' ? msg.humi : deviceState.humi;
        console.log(`[WS] 闹钟触发 temp=${temp} humi=${humi} → 生成 AI 播报`);
        this.handleAlarmTriggered(ws, temp, humi);
        break;
      }

      default:
        console.log('[WS] 未知消息:', (msg.event || msg.type));
    }
  }

  /**
   * 闹钟触发处理链：
   *  1. 从数据库取"今天的周日程"
   *  2. 调用 LLM 生成播报文案（无 API Key 时使用本地模板兜底）
   *  3. 调用 TTS 引擎合成音频（mp3/pcm base64），下发 tts_play 消息
   */
  async handleAlarmTriggered(deviceWs, temp, humi) {
    try {
      // 今天是周几：JS getDay() 0=周日 → 转 1~7（1=周一...7=周日）
      const day = ((new Date().getDay() + 6) % 7) + 1;
      const today = new Date().toISOString().slice(0, 10);
      const todaySchedules = db
        .prepare('SELECT * FROM weekly_schedules WHERE day_of_week = ? ORDER BY start_time')
        .all(day);
      const topCountdown = db.prepare('SELECT * FROM countdowns WHERE is_top = 1 LIMIT 1').get() || null;

      const text = await generateBroadcastSpeech({ today, temp, humi, todaySchedules, topCountdown });
      const audio = await ttsSpeak(text); // { format, dataBase64 } 或 null（无 TTS 配置）

      // 把 TTS 音频保存为静态文件，固件通过 HTTP 拉流播放（NotifyPlayer）
      // 路径以相对路径下发，固件用其配置的服务器地址自行拼接完整 URL
      let ttsUrl = null;
      if (audio) {
        const ttsDir = path.join(__dirname, '..', '..', 'public', 'tts');
        fs.mkdirSync(ttsDir, { recursive: true });
        fs.writeFileSync(path.join(ttsDir, 'broadcast.ogg'), Buffer.from(audio.dataBase64, 'base64'));
        ttsUrl = '/tts/broadcast.ogg';
      }
      this.safeSend(deviceWs, {
        event: 'tts_play',
        text,                                  // 文案兜底：无音频时可在日志显示
        tts_url: ttsUrl,
      });
      this.notifyWeb('BROADCAST_SENT', { text });
    } catch (err) {
      console.error('[WS] 生成播报失败:', err.message);
    }
  }

  /** 数据变更广播：向所有客户端推送 SYNC_UPDATE（内容与 FULL_SYNC 一致，客户端按需增量使用） */
  broadcastSync() {
    const payload = { ...buildFullSyncPayload(), type: 'SYNC_UPDATE' };
    for (const ws of [...this.deviceSockets, ...this.webSockets]) this.safeSend(ws, payload);
  }

  /** 向 Web 端推送设备状态（在线 / 温湿度） */
  notifyWebStatus() {
    this.notifyWeb('DEVICE_STATUS', {
      online: deviceState.online,
      lastSeen: deviceState.lastSeen,
      temp: deviceState.temp,
      humi: deviceState.humi,
      battery: deviceState.battery,
    });
  }

  notifyWeb(event, data) {
    for (const ws of this.webSockets) this.safeSend(ws, { event, data });
  }

  /** 手动触发"立即同步"（设备状态面板按钮） */
  pushSyncToDevice() {
    const payload = buildFullSyncPayload();
    let sent = 0;
    for (const ws of this.deviceSockets) sent += this.safeSend(ws, payload) ? 1 : 0;
    return sent;
  }

  /** 手动触发一次语音播报（设备状态面板按钮） */
  async triggerBroadcast() {
    const deviceWs = [...this.deviceSockets][0];
    if (!deviceWs) return { ok: false, error: '设备不在线' };
    await this.handleAlarmTriggered(deviceWs, deviceState.temp, deviceState.humi);
    return { ok: true };
  }

  safeSend(ws, obj) {
    try {
      if (ws.readyState === ws.OPEN) {
        ws.send(JSON.stringify(obj));
        return true;
      }
    } catch (err) {
      console.error('[WS] 发送失败:', err.message);
    }
    return false;
  }

  /** 供 /api/status 读取硬件状态 */
  getDeviceState() {
    return { ...deviceState };
  }
}

// 单例导出
module.exports = new WsEngine();
