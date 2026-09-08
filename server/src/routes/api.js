/**
 * routes/api.js — OmniDeck REST API
 *
 * 所有写操作成功后会调用 wsEngine.broadcastSync()，
 * 保证 ESP32 固件与 Web 端即时收到 SYNC_UPDATE。
 */
const express = require('express');
const db = require('../db');
const wsEngine = require('../websocket/server');

const router = express.Router();

/* ---------------------- 工具函数 ---------------------- */

/** 校验 HH:MM 时间格式 */
const TIME_RE = /^([01]\d|2[0-3]):[0-5]\d$/;
const DATE_RE = /^\d{4}-\d{2}-\d{2}$/;
const isValidTime = (s) => typeof s === 'string' && TIME_RE.test(s);
const isValidDate = (s) => typeof s === 'string' && DATE_RE.test(s);

/** 通用错误应答 */
const bad = (res, msg) => res.status(400).json({ error: msg });

/* ==================== 1. 周日程 /api/schedule ==================== */

// 列出全部周日程（?day=1..7 可选过滤）
router.get('/schedule', (req, res) => {
  const { day } = req.query;
  if (day !== undefined) {
    const d = parseInt(day, 10);
    if (!(d >= 1 && d <= 7)) return bad(res, 'day 取值 1-7（周一至周日）');
    return res.json(db.prepare('SELECT * FROM weekly_schedules WHERE day_of_week = ? ORDER BY start_time').all(d));
  }
  res.json(db.prepare('SELECT * FROM weekly_schedules ORDER BY day_of_week, start_time').all());
});

// 新增日程
router.post('/schedule', (req, res) => {
  const { day_of_week, start_time, end_time, title, location = '' } = req.body || {};
  if (!(day_of_week >= 1 && day_of_week <= 7)) return bad(res, 'day_of_week 取值 1-7');
  if (!isValidTime(start_time) || !isValidTime(end_time)) return bad(res, '时间格式应为 HH:MM');
  if (end_time <= start_time) return bad(res, '结束时间必须晚于开始时间');
  if (!title || !String(title).trim()) return bad(res, '日程名称不能为空');

  // 冲突检测：同一天内时间段重叠（overlap: a.start < b.end && b.start < a.end）
  const conflict = db.prepare(`
    SELECT id FROM weekly_schedules
    WHERE day_of_week = ?
      AND start_time < ? AND ? < end_time
  `).get(day_of_week, end_time, start_time);
  if (conflict) return res.status(409).json({ error: '与已有日程时间冲突', conflict_id: conflict.id });

  const info = db.prepare(
    'INSERT INTO weekly_schedules (day_of_week, start_time, end_time, title, location) VALUES (?, ?, ?, ?, ?)'
  ).run(day_of_week, start_time, end_time, title.trim(), location);
  const row = db.prepare('SELECT * FROM weekly_schedules WHERE id = ?').get(info.lastInsertRowid);
  wsEngine.broadcastSync();
  res.status(201).json(row);
});

// 修改日程（可选，Web 端点击卡片编辑时使用）
router.put('/schedule/:id', (req, res) => {
  const row = db.prepare('SELECT * FROM weekly_schedules WHERE id = ?').get(req.params.id);
  if (!row) return res.status(404).json({ error: '日程不存在' });
  const merged = { ...row, ...req.body };
  if (!isValidTime(merged.start_time) || !isValidTime(merged.end_time)) return bad(res, '时间格式应为 HH:MM');
  if (merged.end_time <= merged.start_time) return bad(res, '结束时间必须晚于开始时间');
  db.prepare('UPDATE weekly_schedules SET day_of_week=?, start_time=?, end_time=?, title=?, location=? WHERE id=?')
    .run(merged.day_of_week, merged.start_time, merged.end_time, merged.title, merged.location || '', row.id);
  wsEngine.broadcastSync();
  res.json(db.prepare('SELECT * FROM weekly_schedules WHERE id = ?').get(row.id));
});

// 删除日程
router.delete('/schedule/:id', (req, res) => {
  const info = db.prepare('DELETE FROM weekly_schedules WHERE id = ?').run(req.params.id);
  if (!info.changes) return res.status(404).json({ error: '日程不存在' });
  wsEngine.broadcastSync();
  res.json({ ok: true });
});

/* ==================== 2. 倒计时 /api/countdowns ==================== */

router.get('/countdowns', (_req, res) => {
  res.json(db.prepare('SELECT * FROM countdowns ORDER BY target_date').all());
});

router.post('/countdowns', (req, res) => {
  const { title, target_date, is_top = false } = req.body || {};
  if (!title || !String(title).trim()) return bad(res, '事件名称不能为空');
  if (!isValidDate(target_date)) return bad(res, '日期格式应为 YYYY-MM-DD');

  // "同一时间仅允许一个置顶"：若本条要置顶，先取消其它置顶
  if (is_top) db.prepare('UPDATE countdowns SET is_top = 0 WHERE is_top = 1').run();

  const info = db.prepare('INSERT INTO countdowns (title, target_date, is_top) VALUES (?, ?, ?)')
    .run(title.trim(), target_date, is_top ? 1 : 0);
  const row = db.prepare('SELECT * FROM countdowns WHERE id = ?').get(info.lastInsertRowid);
  wsEngine.broadcastSync();
  res.status(201).json(row);
});

// 更新倒计时（主要用于切换置顶开关）
router.put('/countdowns/:id', (req, res) => {
  const row = db.prepare('SELECT * FROM countdowns WHERE id = ?').get(req.params.id);
  if (!row) return res.status(404).json({ error: '倒计时不存在' });
  const merged = { ...row, ...req.body };
  if (!isValidDate(merged.target_date)) return bad(res, '日期格式应为 YYYY-MM-DD');
  if (merged.is_top) db.prepare('UPDATE countdowns SET is_top = 0 WHERE is_top = 1 AND id != ?').run(row.id);
  db.prepare('UPDATE countdowns SET title=?, target_date=?, is_top=? WHERE id=?')
    .run(merged.title, merged.target_date, merged.is_top ? 1 : 0, row.id);
  wsEngine.broadcastSync();
  res.json(db.prepare('SELECT * FROM countdowns WHERE id = ?').get(row.id));
});

router.delete('/countdowns/:id', (req, res) => {
  const info = db.prepare('DELETE FROM countdowns WHERE id = ?').run(req.params.id);
  if (!info.changes) return res.status(404).json({ error: '倒计时不存在' });
  wsEngine.broadcastSync();
  res.json({ ok: true });
});

/* ==================== 3. 闹钟 /api/alarms ==================== */

router.get('/alarms', (_req, res) => {
  res.json(db.prepare('SELECT * FROM alarms').all());
});

// 新增/更新闹钟：带 id 视为更新（Web 端"保存闹钟配置"语义）
router.post('/alarms', (req, res) => {
  const { id, time, days_mask = '[]', enabled = true } = req.body || {};
  if (!isValidTime(time)) return bad(res, '闹钟时间格式应为 HH:MM');
  // days_mask 允许传数组或 "[1,2]" 字符串，统一序列化存储
  let mask = days_mask;
  if (Array.isArray(mask)) mask = JSON.stringify(mask);
  try {
    const arr = JSON.parse(mask);
    if (!Array.isArray(arr) || arr.some((d) => !(d >= 1 && d <= 7))) throw new Error();
  } catch {
    return bad(res, 'days_mask 应为 1-7 组成的数组，例如 "[1,2,3,4,5]"');
  }
  if (id) {
    const row = db.prepare('SELECT * FROM alarms WHERE id = ?').get(id);
    if (!row) return res.status(404).json({ error: '闹钟不存在' });
    db.prepare('UPDATE alarms SET time=?, days_mask=?, enabled=? WHERE id=?')
      .run(time, mask, enabled ? 1 : 0, id);
  } else {
    db.prepare('INSERT INTO alarms (time, days_mask, enabled) VALUES (?, ?, ?)').run(time, mask, enabled ? 1 : 0);
  }
  wsEngine.broadcastSync();
  res.json(db.prepare('SELECT * FROM alarms').all());
});

router.delete('/alarms/:id', (req, res) => {
  const info = db.prepare('DELETE FROM alarms WHERE id = ?').run(req.params.id);
  if (!info.changes) return res.status(404).json({ error: '闹钟不存在' });
  wsEngine.broadcastSync();
  res.json({ ok: true });
});

/* ==================== 4. 待办事项 /api/todos ==================== */

router.get('/todos', (_req, res) => {
  // 未完成的在前（保持创建顺序），已完成的沉底
  res.json(db.prepare('SELECT * FROM todos ORDER BY done ASC, id ASC').all());
});

router.post('/todos', (req, res) => {
  const { text } = req.body || {};
  if (!text || !String(text).trim()) return bad(res, '待办内容不能为空');
  const info = db.prepare('INSERT INTO todos (text) VALUES (?)').run(text.trim());
  const row = db.prepare('SELECT * FROM todos WHERE id = ?').get(info.lastInsertRowid);
  wsEngine.broadcastSync();
  res.status(201).json(row);
});

// 更新（勾选完成 / 修改文字）
router.put('/todos/:id', (req, res) => {
  const row = db.prepare('SELECT * FROM todos WHERE id = ?').get(req.params.id);
  if (!row) return res.status(404).json({ error: '待办不存在' });
  const merged = { ...row, ...req.body };
  db.prepare('UPDATE todos SET text=?, done=? WHERE id=?')
    .run(merged.text, merged.done ? 1 : 0, row.id);
  wsEngine.broadcastSync();
  res.json(db.prepare('SELECT * FROM todos WHERE id = ?').get(row.id));
});

router.delete('/todos/:id', (req, res) => {
  const info = db.prepare('DELETE FROM todos WHERE id = ?').run(req.params.id);
  if (!info.changes) return res.status(404).json({ error: '待办不存在' });
  wsEngine.broadcastSync();
  res.json({ ok: true });
});

/* ==================== 5. 硬件状态 /api/status ==================== */

router.get('/status', (_req, res) => {
  res.json({
    device: wsEngine.getDeviceState(),
    countdowns: db.prepare('SELECT * FROM countdowns ORDER BY target_date').all(),
    alarms: db.prepare('SELECT * FROM alarms').all(),
  });
});

/* ==================== 5. 远程控制 ==================== */

// [立即同步数据]：向 ESP32 手动推送最新 JSON
router.post('/sync', (_req, res) => {
  const sent = wsEngine.pushSyncToDevice();
  res.json({ ok: true, sent, online: wsEngine.getDeviceState().online });
});

// [语音测试/播报今日日程]：触发一次 AI 播报
router.post('/broadcast', async (_req, res) => {
  try {
    res.json(await wsEngine.triggerBroadcast());
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

module.exports = router;
