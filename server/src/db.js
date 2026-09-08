/**
 * db.js — SQLite 数据库初始化与访问层
 *
 * OmniDeck 三张核心表：
 *   1. weekly_schedules — 周日程表（day_of_week 1~7 表示周一至周日）
 *   2. countdowns       — 倒计时表（is_top 用于"唯一置顶"标记）
 *   3. alarms           — 闹钟表（days_mask 形如 "[1,2,3,4,5]"）
 *
 * 数据库文件存放在 /app/data（Docker 挂载卷），本地开发时为 server/data。
 */
const path = require('path');
const fs = require('fs');
const Database = require('better-sqlite3');

// 数据目录：Docker 内为 /app/data（compose 已挂载持久卷），本地默认 server/data
const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, '..', 'data');
fs.mkdirSync(DATA_DIR, { recursive: true });

const db = new Database(path.join(DATA_DIR, 'omnideck.db'));
// WAL 模式：读写并发性能更好，掉电更安全，适合 NAS 长期运行
db.pragma('journal_mode = WAL');

/* ------------------------------------------------------------------
 * 建表（IF NOT EXISTS：容器升级重启后保留旧数据）
 * ------------------------------------------------------------------ */
db.exec(`
  CREATE TABLE IF NOT EXISTS weekly_schedules (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    day_of_week INTEGER NOT NULL CHECK (day_of_week BETWEEN 1 AND 7), -- 1=周一 ... 7=周日
    start_time  TEXT    NOT NULL,  -- "09:00"
    end_time    TEXT    NOT NULL,  -- "10:30"
    title       TEXT    NOT NULL,  -- 日程名称
    location    TEXT    DEFAULT '' -- 地点（可选）
  );

  CREATE TABLE IF NOT EXISTS countdowns (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    title       TEXT NOT NULL,
    target_date TEXT NOT NULL,       -- "2027-06-07"
    is_top      INTEGER DEFAULT 0    -- 是否置顶到 RLCD 屏幕（全局仅一个为 1）
  );

  CREATE TABLE IF NOT EXISTS alarms (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    time      TEXT    NOT NULL,      -- "07:30"
    days_mask TEXT    NOT NULL DEFAULT '[]', -- "[1,2,3,4,5]"
    enabled   INTEGER DEFAULT 1
  );

  CREATE TABLE IF NOT EXISTS todos (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    text       TEXT    NOT NULL,     -- 待办内容
    done       INTEGER DEFAULT 0,    -- 是否完成
    created_at TEXT    DEFAULT (datetime('now','localtime'))
  );
`);

/** 预置少量演示数据（仅当表为空时），方便首次部署后立刻看到效果 */
function seedIfEmpty() {
  const scheduleCount = db.prepare('SELECT COUNT(*) AS c FROM weekly_schedules').get().c;
  if (scheduleCount === 0) {
    const stmt = db.prepare(
      'INSERT INTO weekly_schedules (day_of_week, start_time, end_time, title, location) VALUES (?, ?, ?, ?, ?)'
    );
    stmt.run(1, '09:00', '11:30', 'ESP32 固件二次开发', '书房');
    stmt.run(3, '14:00', '17:30', 'ESP32 固件二次开发', '书房');
    stmt.run(5, '19:30', '21:00', '健身 / 跑步', '健身房');
  }
  const todoCount = db.prepare('SELECT COUNT(*) AS c FROM todos').get().c;
  if (todoCount === 0) {
    const stmt = db.prepare('INSERT INTO todos (text) VALUES (?)');
    stmt.run('整理 ESP32 代码');
    stmt.run('英语单词打卡');
    stmt.run('准备周五会议');
  }
}
seedIfEmpty();

module.exports = db;
