# OmniDeck 智能桌面控制台

基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 AIoT 桌面控制台：
ESP32-S3 + 4.2" RLCD 全反射屏 (ST7305) + SHTC3 温湿度 + PCF85063 RTC，
配群晖 NAS Docker 服务端与 Web 管理端。

```
OmniDeck/
├── server/                 # NAS 服务端 (Node.js + Express + WS + SQLite + Vue 3)
│   ├── src/
│   │   ├── index.js        # 入口: HTTP (容器内 8080, REST + 静态页 + WS 共端口)
│   │   ├── db.js           # SQLite 三张表: weekly_schedules / countdowns / alarms
│   │   ├── routes/api.js   # REST API
│   │   ├── websocket/      # WebSocket 引擎 (FULL_SYNC / SYNC_UPDATE / tts_play)
│   │   └── ai/broadcaster.js # LLM + TTS 播报引擎 (可选，无 Key 时本地模板兜底)
│   ├── public/             # Vue 3 + Tailwind Web 管理端 (免构建)
│   ├── Dockerfile
│   └── docker-compose.yml  # 群晖 Container Manager 一键部署
└── firmware/
    └── xiaozhi-esp32-backup/
        ├── main/boards/omnideck/     # OmniDeck 板级适配 (新增)
        └── partitions/v2/omnideck_16m.csv
```

## 快速开始 — 服务端（群晖）

```bash
cd server && docker compose up -d --build
# 浏览器打开 http://NAS_IP:8081
# 设备 WebSocket: ws://NAS_IP:8081/ws?role=device
```

数据持久化在 `server/data/`（SQLite + WAL）。AI 播报环境变量见 `.env.example`。

### REST API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET/POST | `/api/schedule` | 周日程列表 / 新增（自动冲突检测 409） |
| PUT/DELETE | `/api/schedule/:id` | 修改 / 删除 |
| GET/POST | `/api/countdowns` | 倒计时列表 / 新增 |
| PUT/DELETE | `/api/countdowns/:id` | 修改（唯一置顶）/ 删除 |
| GET/POST | `/api/alarms` | 闹钟列表 / 保存 |
| GET | `/api/status` | 设备在线状态 + 最新温湿度 |
| POST | `/api/sync` | 手动向设备推送全量数据 |
| POST | `/api/broadcast` | 手动触发一次 AI 播报 |

### WebSocket 协议 (`/ws`)

- 下行: `FULL_SYNC` / `SYNC_UPDATE`（schedules + countdown + alarms）、`tts_play`（tts_url + text）、`DEVICE_STATUS`、`MIRROR_UPDATE`
- 上行: `env_report`（temp/humi）、`alarm_triggered`（temp/humi → 服务端回 TTS）、`mirror`（屏幕镜像快照）、`heartbeat`

## 快速开始 — 固件

```bash
cd firmware/xiaozhi-esp32-backup
idf.py set-target esp32s3
idf.py menuconfig          # → OmniDeck Settings: 填 NAS 的 ws://IP:8081/ws?role=device
                           # → Board Type 选择 "OmniDeck"
idf.py build flash monitor
```

板级目录 `main/boards/omnideck/`：

| 文件 | 内容 |
|---|---|
| `config.h` | 引脚映射（I2C=8/9，RTC INT=14，KEY=0，LCD/音频占位需按 PCB 修改） |
| `omnideck.cc` | 板类: I2C 总线、ES8311 音频、ST7305 显示、按键（短按切页/长按配网） |
| `custom_lcd_display.*` | ST7305 400×300 横屏驱动（派生自 waveshare RLCD 4.2 板）（派生自 waveshare RLCD 4.2 板） |
| `shtc3.*` | SHTC3 温湿度驱动（CRC 校验） |
| `pcf85063.*` | PCF85063 RTC 驱动（24h 制、每日闹钟、TF 标志） |
| `omnideck_app.*` | WS 客户端 + LittleFS 缓存 + 60s RTC 轮询比对 + 镜像上报 |
| `omnideck_ui.*` | `renderOmniDeckUI()` 四区域布局（35/125/110/130px） |
| `alarm_coordinator.*` | RTC INT 中断 → 铃声 → 上报 → TTS 拉流播放 → 按键打断 |

离线容错：`SYNC_UPDATE` 写入 `/littlefs/schedule_cache.json`、`/littlefs/events.json`；
Wi-Fi/NAS 断开后屏幕继续用本地 RTC + 缓存正常显示。分区表 `omnideck_16m.csv` 额外划分 1MB storage 分区。

> ⚠️ LCD / I2S 引脚在 `config.h` 中为常见默认值，烧录前请对照实际 PCB 核对。

## 待办 / 建议

- [ ] 本地 `npm start` 与 Docker 构建验证（开发机未装 Node/Docker，首次部署时验证）
- [ ] ESP-IDF 5.5.2 下 `idf.py build` 全量编译验证
- [ ] 自定义 `alarm_ring` 音频替换内置提示音
