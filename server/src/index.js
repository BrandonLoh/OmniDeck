/**
 * index.js — OmniDeck 服务端入口
 *
 * Express HTTP (REST + 静态 Web 管理端) 与 WebSocket (/ws) 共享同一端口，
 * Docker 部署时映射 8080:8080 即可。
 */
const path = require('path');
const express = require('express');
const http = require('http');
const apiRouter = require('./routes/api');
const wsEngine = require('./websocket/server');

const PORT = parseInt(process.env.PORT || '8080', 10);

const app = express();
app.use(express.json({ limit: '2mb' }));

// Web 管理端静态资源（Vue 3 单页应用）
app.use(express.static(path.join(__dirname, '..', 'public')));

// REST API
app.use('/api', apiRouter);

// 兜底：未知 API 返回 404 JSON（而非 HTML）
app.use('/api', (_req, res) => res.status(404).json({ error: '接口不存在' }));

// HTTP server + WebSocket 挂载
const server = http.createServer(app);
wsEngine.attach(server);

server.listen(PORT, () => {
  console.log(`[OmniDeck] 服务已启动: http://0.0.0.0:${PORT}`);
  console.log(`[OmniDeck] WebSocket 端点: ws://0.0.0.0:${PORT}/ws?role=device`);
});
