#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0
/*
 * LiteKVM 自部署跳网中继 (Phase 5 / Task 5.1)
 *
 * 设计要点：
 *   - token 鉴权：静态 token 白名单（token 由两端配对共享密钥 HKDF 派生，见 README
 *     《Token 派生规范》与 tools/derive-token.js）。比较使用 SHA-256 摘要 + 常数时间比较。
 *   - 连接配对：两个客户端 JOIN 同一个 room，凑齐后服务器只做字节流转发。
 *   - 纯转发不解析内容：配对完成后进入 splice 模式（socket.pipe），中继不读取、
 *     不修改、不缓存业务字节。TLS/加密由两端客户端自己负责（默认本服务不做 TLS）。
 *   - 配对成功后不再向数据流里注入任何控制字节（避免污染透明通道）；对端离线通过
 *     TCP FIN/RST 表达。
 *
 * 协议（换行结尾的纯文本握手，最多 512 字节）：
 *   客户端 -> 中继 : JOIN <room> <token>\n
 *   中继 -> 客户端 : OK <connId> <peerIndex>\n            (peerIndex 从 1 开始)
 *   中继 -> 双方   : PAIRED <peerCount>\n                 (凑齐后；此后进入字节流)
 *   中继 -> 客户端 : ERR <code>\n                         (失败后立即关闭连接)
 *
 *   code ∈ invalid-token | missing-token | bad-room | bad-command | room-full |
 *          handshake-too-large | handshake-timeout | rate-limited
 *
 * 环境变量见 loadConfig()，全部有默认值。健康检查：HTTP GET /health。
 */
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const net = require('net');

const VERSION = '0.1.0';
const RELAY_PROTO = 1;

const ROOM_RE = /^[A-Za-z0-9_-]{8,64}$/;
const DEFAULTS = {
  host: '0.0.0.0',
  port: 25902,
  healthPort: 8080,
  roomMaxPeers: 2,
  authMode: 'static',
  handshakeTimeoutMs: 10000,
  maxHandshakeBytes: 512,
  maxAuthFailures: 10,
  authFailureWindowMs: 60000,
  logLevel: 'info',
};

function envInt(env, name, def) {
  const raw = env[name];
  if (raw === undefined || raw === '') return def;
  const n = Number.parseInt(raw, 10);
  if (!Number.isFinite(n) || n < 0) {
    throw new Error(`环境变量 ${name}=${raw} 不是合法非负整数`);
  }
  return n;
}

function loadConfig(env = process.env) {
  const cfg = {
    host: env.RELAY_HOST || DEFAULTS.host,
    port: envInt(env, 'RELAY_PORT', DEFAULTS.port),
    healthPort: envInt(env, 'RELAY_HEALTH_PORT', DEFAULTS.healthPort),
    roomMaxPeers: Math.max(2, envInt(env, 'RELAY_ROOM_MAX_PEERS', DEFAULTS.roomMaxPeers)),
    authMode: (env.RELAY_AUTH_MODE || DEFAULTS.authMode).toLowerCase(),
    tokensFile: env.RELAY_TOKENS_FILE || '',
    handshakeTimeoutMs: envInt(env, 'RELAY_HANDSHAKE_TIMEOUT_MS', DEFAULTS.handshakeTimeoutMs),
    maxHandshakeBytes: envInt(env, 'RELAY_MAX_HANDSHAKE_BYTES', DEFAULTS.maxHandshakeBytes),
    maxAuthFailures: envInt(env, 'RELAY_MAX_AUTH_FAILURES', DEFAULTS.maxAuthFailures),
    authFailureWindowMs: envInt(env, 'RELAY_AUTH_FAILURE_WINDOW_MS', DEFAULTS.authFailureWindowMs),
    httpShutdown: ['1', 'true', 'yes', 'on'].includes(String(env.RELAY_ENABLE_HTTP_SHUTDOWN || '').toLowerCase()),
    logLevel: (env.RELAY_LOG_LEVEL || DEFAULTS.logLevel).toLowerCase(),
  };

  if (!['static', 'open'].includes(cfg.authMode)) {
    throw new Error(`RELAY_AUTH_MODE=${cfg.authMode} 非法（可选 static / open）`);
  }

  // token 白名单：只保存 SHA-256 摘要（32 字节），比较时统一长度 + 常数时间
  const raw = [];
  if (env.RELAY_TOKENS) raw.push(...env.RELAY_TOKENS.split(','));
  if (cfg.tokensFile) {
    const text = fs.readFileSync(cfg.tokensFile, 'utf8');
    for (const line of text.split(/\r?\n/)) {
      const t = line.trim();
      if (t && !t.startsWith('#')) raw.push(t);
    }
  }
  cfg.tokenDigests = raw.map((t) => t.trim()).filter(Boolean).map(digestOf);
  return cfg;
}

const LOG_RANK = { debug: 10, info: 20, warn: 30, error: 40 };

function digestOf(token) {
  return crypto.createHash('sha256').update(Buffer.from(token, 'utf8')).digest();
}

/// 常数时间：与白名单中每个摘要逐字节比较，不提前返回（不泄漏命中的位置）
function tokenAllowed(digests, token) {
  if (!token) return false;
  const got = digestOf(token);
  let hit = 0;
  for (const want of digests) {
    hit |= crypto.timingSafeEqual(got, want) ? 1 : 0;
  }
  return hit === 1;
}

class Relay {
  constructor(cfg, opts = {}) {
    this.cfg = cfg;
    this.now = opts.now || (() => Date.now());
    this.rooms = new Map(); // roomId -> { id, peers: [conn], createdAt }
    this.failures = new Map(); // ip -> { count, resetAt }
    this.connSeq = 0;
    this.tcpServer = null;
    this.httpServer = null;
    this.stats = {
      version: VERSION,
      relay_proto: RELAY_PROTO,
      connections: 0,
      joined: 0,
      paired: 0,
      rejected: 0,
      rate_limited: 0,
      auth_failures: 0,
      bytes_forwarded: 0,
    };
    if (cfg.authMode === 'static' && cfg.tokenDigests.length === 0) {
      this.log('error', 'no-tokens-configured', {
        hint: '需设置 RELAY_TOKENS 或 RELAY_TOKENS_FILE；否则所有 JOIN 都会被拒绝',
      });
    }
    if (cfg.authMode === 'open') {
      this.log('warn', 'auth-disabled', { hint: 'RELAY_AUTH_MODE=open 仅供本机调试，切勿用于公网' });
    }
  }

  log(level, event, fields = {}) {
    if (LOG_RANK[level] < LOG_RANK[this.cfg.logLevel]) return;
    const line = { ts: new Date(this.now()).toISOString(), level, event, ...fields };
    process.stdout.write(`${JSON.stringify(line)}\n`);
  }

  start() {
    return new Promise((resolve, reject) => {
      this.tcpServer = net.createServer((socket) => this.onConnection(socket));
      this.tcpServer.on('error', reject);
      this.tcpServer.listen(this.cfg.port, this.cfg.host, () => {
        this.port = this.tcpServer.address().port;
        if (this.cfg.healthPort < 0) return resolve(this.announce());
        this.httpServer = http.createServer((req, res) => this.onHttp(req, res));
        this.httpServer.on('error', reject);
        this.httpServer.listen(this.cfg.healthPort, this.cfg.host, () => {
          this.healthPort = this.httpServer.address().port;
          resolve(this.announce());
        });
      });
    });
  }

  announce() {
    this.log('info', 'listening', {
      version: VERSION,
      relay_proto: RELAY_PROTO,
      tcp_port: this.port,
      health_port: this.cfg.healthPort < 0 ? null : this.healthPort,
      auth_mode: this.cfg.authMode,
      room_max_peers: this.cfg.roomMaxPeers,
      token_count: this.cfg.tokenDigests.length,
    });
    return this;
  }

  async stop() {
    const closers = [];
    for (const room of this.rooms.values()) {
      for (const conn of room.peers) conn.socket.destroy();
    }
    this.rooms.clear();
    if (this.tcpServer) closers.push(new Promise((r) => this.tcpServer.close(r)));
    if (this.httpServer) closers.push(new Promise((r) => this.httpServer.close(r)));
    await Promise.all(closers);
    this.log('info', 'stopped', { bytes_forwarded: this.stats.bytes_forwarded });
  }

  onHttp(req, res) {
    const path = (req.url || '/').split('?')[0];
    if (req.method === 'POST' && path === '/shutdown') {
      // 仅当显式开启 且 来自本机回环地址时才允许（给容器/systemd 之外的运维留一个优雅停口）
      const loopback = isLoopback(req.socket.remoteAddress);
      if (!this.cfg.httpShutdown || !loopback) {
        res.writeHead(403, { 'content-type': 'application/json' });
        res.end('{"status":"forbidden"}\n');
        return;
      }
      res.writeHead(202, { 'content-type': 'application/json' });
      res.end('{"status":"stopping"}\n');
      this.log('info', 'shutdown-requested', { via: 'http' });
      if (this.requestShutdown) setImmediate(this.requestShutdown);
      return;
    }
    if (req.method === 'GET' && (path === '/health' || path === '/metrics' || path === '/')) {
      let peers = 0;
      for (const room of this.rooms.values()) peers += room.peers.length;
      const body = JSON.stringify({
        status: 'ok',
        version: VERSION,
        relay_proto: RELAY_PROTO,
        uptime_s: Math.round(process.uptime()),
        rooms: this.rooms.size,
        peers,
        auth_mode: this.cfg.authMode,
        ...this.stats,
      });
      res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
      res.end(`${body}\n`);
      return;
    }
    res.writeHead(404, { 'content-type': 'text/plain' });
    res.end('not found\n');
  }

  onConnection(socket) {
    const ip = socket.remoteAddress || 'unknown';
    this.stats.connections += 1;

    if (this.isRateLimited(ip)) {
      this.stats.rejected += 1;
      this.stats.rate_limited += 1;
      this.log('warn', 'rejected', { reason: 'rate-limited', ip });
      socket.end('ERR rate-limited\n');
      return;
    }

    const conn = {
      id: `c${++this.connSeq}`,
      ip,
      socket,
      buffer: Buffer.alloc(0),
      leftover: Buffer.alloc(0),
      authed: false,
      room: null,
      peerIndex: 0,
      openedAt: this.now(),
    };

    socket.setNoDelay(true);
    socket.setKeepAlive(true, 30000);
    socket.on('error', () => {});
    socket.on('close', () => this.onClose(conn));
    socket.on('data', (chunk) => this.onData(conn, chunk));

    conn.handshakeTimer = setTimeout(() => {
      if (conn.authed) return;
      this.reject(conn, 'handshake-timeout');
    }, this.cfg.handshakeTimeoutMs);
    if (conn.handshakeTimer.unref) conn.handshakeTimer.unref();
  }

  isRateLimited(ip) {
    const st = this.failures.get(ip);
    if (!st) return false;
    if (this.now() >= st.resetAt) {
      this.failures.delete(ip);
      return false;
    }
    return st.count >= this.cfg.maxAuthFailures;
  }

  noteAuthFailure(ip) {
    const st = this.failures.get(ip);
    if (!st || this.now() >= st.resetAt) {
      this.failures.set(ip, { count: 1, resetAt: this.now() + this.cfg.authFailureWindowMs });
      return;
    }
    st.count += 1;
  }

  reject(conn, code) {
    this.stats.rejected += 1;
    this.log('warn', 'rejected', { reason: code, ip: conn.ip, conn: conn.id });
    conn.socket.end(`ERR ${code}\n`);
  }

  onData(conn, chunk) {
    if (conn.authed) {
      // 配对后由 pipe() 负责搬运，这里不再处理，避免重复写入对端
      return;
    }
    conn.buffer = Buffer.concat([conn.buffer, chunk]);
    if (conn.buffer.length > this.cfg.maxHandshakeBytes) {
      this.reject(conn, 'handshake-too-large');
      return;
    }
    const nl = conn.buffer.indexOf(0x0a);
    if (nl < 0) return;

    const line = conn.buffer.slice(0, nl).toString('utf8').trim();
    conn.leftover = conn.buffer.slice(nl + 1);
    conn.buffer = Buffer.alloc(0);
    socket_pause(conn.socket); // 配对前不再消费数据；字节留在内核缓冲区，配对后 resume
    this.handleCommand(conn, line);
  }

  handleCommand(conn, line) {
    const parts = line.split(/\s+/).filter(Boolean);
    const cmd = (parts[0] || '').toUpperCase();

    if (cmd === 'PING') {
      conn.socket.write(`PONG ${VERSION}\n`);
      return;
    }
    if (cmd !== 'JOIN') {
      this.reject(conn, 'bad-command');
      return;
    }
    const roomId = parts[1] || '';
    const token = parts[2] || '';

    if (!ROOM_RE.test(roomId)) {
      this.reject(conn, 'bad-room');
      return;
    }
    if (!token) {
      this.noteAuthFailure(conn.ip);
      this.stats.auth_failures += 1;
      this.reject(conn, 'missing-token');
      return;
    }
    if (this.cfg.authMode === 'static' && !tokenAllowed(this.cfg.tokenDigests, token)) {
      this.noteAuthFailure(conn.ip);
      this.stats.auth_failures += 1;
      this.log('warn', 'auth-failed', {
        ip: conn.ip,
        room: roomId,
        token_digest: digestOf(token).toString('hex').slice(0, 8),
      });
      this.reject(conn, 'invalid-token');
      return;
    }
    this.failures.delete(conn.ip);

    const room = this.rooms.get(roomId) || { id: roomId, peers: [], createdAt: this.now() };
    this.rooms.set(roomId, room);
    if (room.peers.length >= this.cfg.roomMaxPeers) {
      this.reject(conn, 'room-full');
      return;
    }

    conn.authed = true;
    conn.room = room;
    room.peers.push(conn);
    conn.peerIndex = room.peers.length;
    this.stats.joined += 1;

    clearTimeout(conn.handshakeTimer);
    conn.socket.write(`OK ${conn.id} ${conn.peerIndex}\n`);
    this.log('info', 'joined', {
      conn: conn.id,
      ip: conn.ip,
      room: roomId,
      peer_index: conn.peerIndex,
      peers: room.peers.length,
    });

    if (room.peers.length === this.cfg.roomMaxPeers) this.beginRelay(room);
  }

  beginRelay(room) {
    const [a, b] = room.peers;
    room.splicedAt = this.now();
    this.stats.paired += 1;

    for (const conn of room.peers) {
      conn.socket.write(`PAIRED ${room.peers.length}\n`);
      conn.socket.on('data', (chunk) => {
        this.stats.bytes_forwarded += chunk.length;
      });
    }
    this.log('info', 'paired', { room: room.id, peers: room.peers.map((c) => c.id) });

    a.relayPeer = b;
    b.relayPeer = a;
    a.socket.pipe(b.socket);
    b.socket.pipe(a.socket);

    // 配对前缓存/内核里排队的字节，配对后立即补发（保证字节透明、不丢首包）
    this.flushLeftover(a);
    this.flushLeftover(b);
    socket_resume(a.socket);
    socket_resume(b.socket);
  }

  flushLeftover(conn) {
    if (!conn.leftover || conn.leftover.length === 0 || !conn.relayPeer) return;
    const buf = conn.leftover;
    conn.leftover = Buffer.alloc(0);
    this.stats.bytes_forwarded += buf.length;
    conn.relayPeer.socket.write(buf);
  }

  onClose(conn) {
    clearTimeout(conn.handshakeTimer);
    if (conn.room) {
      const room = conn.room;
      const idx = room.peers.indexOf(conn);
      if (idx >= 0) room.peers.splice(idx, 1);
      this.log('info', 'left', { conn: conn.id, room: room.id, peers: room.peers.length });

      // 对端离线：直接断开，客户端据此感知并重新 JOIN
      for (const peer of room.peers) {
        if (peer.relayPeer === conn) {
          peer.relayPeer = null;
          peer.socket.destroy();
        }
      }
      if (room.peers.length === 0) this.rooms.delete(room.id);
    }
  }
}

function socket_pause(socket) {
  try {
    socket.pause();
  } catch {
    /* ignore */
  }
}
function socket_resume(socket) {
  try {
    socket.resume();
  } catch {
    /* ignore */
  }
}

function isLoopback(addr) {
  if (!addr) return false;
  return addr === '127.0.0.1' || addr === '::1' || addr === '::ffff:127.0.0.1' || addr.startsWith('127.');
}

function main() {
  let cfg;
  try {
    cfg = loadConfig();
  } catch (err) {
    process.stderr.write(`${JSON.stringify({ level: 'error', event: 'config-error', error: String(err.message) })}\n`);
    process.exit(2);
    return;
  }
  const relay = new Relay(cfg);
  relay.start().catch((err) => {
    process.stderr.write(`${JSON.stringify({ level: 'error', event: 'listen-failed', error: String(err.message) })}\n`);
    process.exit(1);
  });

  let stopping = false;
  const shutdown = (sig) => {
    if (stopping) return;
    stopping = true;
    relay.log('info', 'shutdown', { signal: sig });
    relay.stop().then(() => process.exit(0));
    setTimeout(() => process.exit(0), 3000).unref();
  };
  process.on('SIGTERM', () => shutdown('SIGTERM'));
  process.on('SIGINT', () => shutdown('SIGINT'));
  // 与信号处理共用同一条优雅停机路径（Windows 上 process.kill 走 TerminateProcess，
  // 信号处理函数不会执行，故提供一个回环地址专用、默认关闭的 HTTP 停口）
  relay.requestShutdown = () => shutdown('HTTP');
}

if (require.main === module) main();

module.exports = { Relay, loadConfig, tokenAllowed, digestOf, VERSION, RELAY_PROTO };
