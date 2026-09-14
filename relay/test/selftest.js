#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0
/*
 * LiteKVM 中继自测（端到端，零依赖）
 *
 * 真实动作：以子进程方式启动 relay/server.js，起真实 TCP 客户端连上去，
 * 验证：token 鉴权（正/反例）、同 room 配对、双向字节透明转发（含大数据 + 奇偶
 * 校验)、配对前发送的数据不丢、房间隔离、第三端被拒、握手异常、限流、健康检查、
 * SIGTERM 优雅退出。
 *
 * 运行：node test/selftest.js      （退出码 0 = 全部通过）
 */
'use strict';

const { spawn } = require('child_process');
const crypto = require('crypto');
const net = require('net');
const path = require('path');

const { derive } = require('../tools/derive-token.js');

const SERVER = path.join(__dirname, '..', 'server.js');
const results = [];
let failures = 0;
let skipped = 0;

function check(name, ok, detail = '') {
  results.push({ name, ok, detail });
  if (!ok) failures += 1;
  const mark = ok ? 'PASS' : 'FAIL';
  process.stdout.write(`[${mark}] ${name}${detail ? ` — ${detail}` : ''}\n`);
}

function skip(name, reason) {
  skipped += 1;
  process.stdout.write(`[SKIP] ${name} — ${reason}\n`);
}

const delay = (ms) => new Promise((r) => setTimeout(r, ms));
const sha = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

function startServer(env = {}) {
  const proc = spawn(process.execPath, [SERVER], {
    env: { ...process.env, RELAY_PORT: '0', RELAY_HEALTH_PORT: '0', ...env },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const logs = [];
  let resolveReady;
  let rejectReady;
  const ready = new Promise((res, rej) => {
    resolveReady = res;
    rejectReady = rej;
  });
  let buf = '';
  const onChunk = (chunk, stream) => {
    buf += chunk.toString('utf8');
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (!line) continue;
      logs.push(line);
      if (stream === 'out') {
        try {
          const obj = JSON.parse(line);
          if (obj.event === 'listening') resolveReady({ proc, logs, ...obj });
        } catch {
          /* 非 JSON 日志忽略 */
        }
      }
    }
  };
  proc.stdout.on('data', (c) => onChunk(c, 'out'));
  proc.stderr.on('data', (c) => {
    logs.push(String(c));
    process.stderr.write(`[server:stderr] ${c}`);
  });
  proc.on('exit', (code, sig) => rejectReady(new Error(`server 提前退出 code=${code} sig=${sig}`)));
  const timeout = setTimeout(() => rejectReady(new Error('server 启动超时')), 10000);
  ready.finally(() => clearTimeout(timeout));
  return ready;
}

class Peer {
  constructor(socket) {
    this.socket = socket;
    this.buf = Buffer.alloc(0);
    this.lines = [];
    this.raw = false;
    this.autoRaw = true;
    this.closed = false;
    this.endSeen = false;
    this.waiters = [];
    this.dataChunks = [];
    socket.on('data', (chunk) => {
      this.buf = Buffer.concat([this.buf, chunk]);
      // 收到 PAIRED 之后流里就是业务字节，必须停止按行切分（否则会吃掉含 \n 的字段）
      while (!this.raw) {
        const nl = this.buf.indexOf(0x0a);
        if (nl < 0) break;
        const line = this.buf.slice(0, nl).toString('utf8');
        this.buf = this.buf.slice(nl + 1);
        this.lines.push(line);
        if (this.autoRaw && line.startsWith('PAIRED')) {
          this.raw = true;
          break;
        }
      }
      this.dataChunks.push(chunk);
      this.wake();
    });
    socket.on('end', () => {
      this.endSeen = true;
      this.wake();
    });
    socket.on('close', () => {
      this.closed = true;
      this.wake();
    });
    socket.on('error', () => {});
  }

  static async connect(port, host = '127.0.0.1') {
    const socket = await new Promise((resolve, reject) => {
      const s = net.connect({ port, host }, () => resolve(s));
      s.on('error', reject);
    });
    socket.setNoDelay(true);
    return new Peer(socket);
  }

  wake() {
    for (const w of this.waiters.splice(0)) w();
  }

  write(data) {
    this.socket.write(data);
  }

  async nextLine(timeoutMs = 3000) {
    const deadline = Date.now() + timeoutMs;
    while (this.lines.length === 0) {
      if (Date.now() > deadline) throw new Error(`等待一行超时（已收到 ${JSON.stringify(this.lines)}）`);
      if (this.closed && this.lines.length === 0) throw new Error('连接已关闭且没有新行');
      await Promise.race([new Promise((r) => this.waiters.push(r)), delay(50)]);
    }
    return this.lines.shift();
  }

  /// 收集 length 字节（跳过握手行）
  async readBytes(length, timeoutMs = 8000) {
    const deadline = Date.now() + timeoutMs;
    while (this.buf.length < length) {
      if (Date.now() > deadline) {
        throw new Error(`等待 ${length} 字节超时（只有 ${this.buf.length}）`);
      }
      await Promise.race([new Promise((r) => this.waiters.push(r)), delay(50)]);
    }
    const out = this.buf.slice(0, length);
    this.buf = this.buf.slice(length);
    return out;
  }

  async closedWithin(timeoutMs = 3000) {
    const deadline = Date.now() + timeoutMs;
    while (!this.closed && Date.now() < deadline) {
      await Promise.race([new Promise((r) => this.waiters.push(r)), delay(50)]);
    }
    return this.closed;
  }

  end() {
    this.socket.end();
  }
  destroy() {
    this.socket.destroy();
  }
}

function randomBuffer(size) {
  // 覆盖全部 256 种字节值，验证中继不改任何字节
  const b = Buffer.alloc(size);
  for (let i = 0; i < size; i += 1) b[i] = i % 256;
  const pad = crypto.randomBytes(Math.min(64, size));
  pad.copy(b, 0);
  return b;
}

async function health(port) {
  return new Promise((resolve, reject) => {
    const req = require('http').get({ host: '127.0.0.1', port, path: '/health' }, (res) => {
      let body = '';
      res.on('data', (c) => (body += c));
      res.on('end', () => resolve({ status: res.statusCode, json: JSON.parse(body) }));
    });
    req.on('error', reject);
  });
}

async function main() {
  process.stdout.write('=== LiteKVM relay 自测 ===\n');
  const pairA = derive(crypto.randomBytes(32)); // 模拟设备 A/B 配对派生的共享密钥（两端同一把）
  const pairOther = derive(crypto.randomBytes(32)); // 另一对设备
  const pairC = derive(crypto.randomBytes(32)); // 第三对设备（断开重连用例）

  process.stdout.write(
    `派生示例（模拟配对共享密钥）：room=${pairA.room} token=${pairA.token.slice(0, 8)}…（共 ${pairA.token.length} 字符）\n`,
  );

  const srv = await startServer({
    RELAY_AUTH_MODE: 'static',
    RELAY_TOKENS: `${pairA.token},${pairOther.token},${pairC.token}`,
    RELAY_MAX_AUTH_FAILURES: '1000', // 限流单独用例，避免干扰其它用例
    RELAY_ENABLE_HTTP_SHUTDOWN: '1',
  });
  const tcpPort = srv.tcp_port;
  const hp = srv.health_port;
  const p = srv.proc;
  const allLogs = srv.logs;

  try {
    // ---- 用例 1：正确 token 两端配对 + 双向字节透明转发 ----
    const a = await Peer.connect(tcpPort);
    a.write(`JOIN ${pairA.room} ${pairA.token}\n`);
    const okA = await a.nextLine();
    check('1a JOIN 正确 token 收到 OK', /^OK c\d+ 1$/.test(okA), `实际: ${JSON.stringify(okA)}`);

    // 配对前先写业务数据：验证中继不会吞掉首包
    const prePayload = randomBuffer(4096);
    a.write(prePayload);

    const b = await Peer.connect(tcpPort);
    b.write(`JOIN ${pairA.room} ${pairA.token}\n`);
    const okB = await b.nextLine();
    check('1b 第二端 JOIN 收到 OK(peerIndex=2)', /^OK c\d+ 2$/.test(okB), `实际: ${JSON.stringify(okB)}`);
    const pairedA = await a.nextLine();
    const pairedB = await b.nextLine();
    check('1c 双方都收到 PAIRED', pairedA === 'PAIRED 2' && pairedB === 'PAIRED 2', `${pairedA} / ${pairedB}`);

    const gotPre = await b.readBytes(prePayload.length);
    check(
      '1d 配对前发送的数据配对后补发且逐字节一致',
      sha(gotPre) === sha(prePayload),
      `${gotPre.length} bytes sha256=${sha(gotPre).slice(0, 16)}…`,
    );

    // ---- 用例 2：大数据双向转发（1 MiB）----
    const big = randomBuffer(1024 * 1024);
    const back = randomBuffer(256 * 1024);
    const t0 = Date.now();
    a.write(big);
    const gotBig = await b.readBytes(big.length);
    const msForward = Date.now() - t0;
    b.write(back);
    const gotBack = await a.readBytes(back.length);
    check('2a A→B 1 MiB 转发，sha256 一致', sha(gotBig) === sha(big), `${gotBig.length} bytes, ${msForward} ms`);
    check('2b B→A 256 KiB 转发，sha256 一致', sha(gotBack) === sha(back), `${gotBack.length} bytes`);

    // ---- 用例 3：错误 token 被拒 ----
    const bad = await Peer.connect(tcpPort);
    bad.write(`JOIN ${pairA.room} ${pairA.token.slice(0, -1)}X\n`);
    const badLine = await bad.nextLine();
    check('3a 错误 token 返回 ERR invalid-token', badLine === 'ERR invalid-token', `实际: ${JSON.stringify(badLine)}`);
    check('3b 鉴权失败后连接被关闭', await bad.closedWithin(2000), 'closed=true');

    // ---- 用例 4：缺 token 被拒 ----
    const noTok = await Peer.connect(tcpPort);
    noTok.write(`JOIN ${pairA.room}\n`);
    const noTokLine = await noTok.nextLine();
    check('4 缺 token 返回 ERR missing-token', noTokLine === 'ERR missing-token', `${noTokLine}`);

    // ---- 用例 5：room 满（第三端）----
    const third = await Peer.connect(tcpPort);
    third.write(`JOIN ${pairA.room} ${pairA.token}\n`);
    const thirdLine = await third.nextLine();
    check('5 第三端 JOIN 已满 room 被拒（ERR room-full）', thirdLine === 'ERR room-full', `实际: ${JSON.stringify(thirdLine)}`);

    // ---- 用例 6：鉴权非法命令 / 非法 room ----
    const badCmd = await Peer.connect(tcpPort);
    badCmd.write('HELLO world\n');
    check('6a 非 JOIN 命令返回 ERR bad-command', (await badCmd.nextLine()) === 'ERR bad-command');
    const badRoom = await Peer.connect(tcpPort);
    badRoom.write(`JOIN short ${pairA.token}\n`); // room 少于 8 字符
    check('6b 非法 room 返回 ERR bad-room', (await badRoom.nextLine()) === 'ERR bad-room');

    // ---- 用例 7：房间隔离（另一对设备的数据不会串到 A/B）----
    const o1 = await Peer.connect(tcpPort);
    o1.write(`JOIN ${pairOther.room} ${pairOther.token}\n`);
    await o1.nextLine();
    const o2 = await Peer.connect(tcpPort);
    o2.write(`JOIN ${pairOther.room} ${pairOther.token}\n`);
    await o2.nextLine();
    await o1.nextLine(); // PAIRED
    await o2.nextLine();
    const marker = Buffer.from('ISOLATION-PROBE-'.repeat(64));
    o1.write(marker);
    const gotMarker = await o2.readBytes(marker.length);
    check('7a 另一房间内部转发正常', sha(gotMarker) === sha(marker), `${gotMarker.length} bytes`);
    const leak = await Promise.race([
      a.readBytes(marker.length, 800).then(
        () => true,
        () => false,
      ),
      delay(800).then(() => false),
    ]);
    check('7b 房间之间没有串流（A 端未收到另一房间数据）', leak === false, leak ? '收到不该有的数据！' : '无泄漏');

    // ---- 用例 8：对端断开 -> 本端连接被关闭（客户端据此重新 JOIN）----
    o1.destroy();
    o2.destroy();
    await delay(200);
    const c1 = await Peer.connect(tcpPort);
    c1.write(`JOIN ${pairC.room} ${pairC.token}\n`);
    await c1.nextLine();
    const d1 = await Peer.connect(tcpPort);
    d1.write(`JOIN ${pairC.room} ${pairC.token}\n`);
    await d1.nextLine();
    await c1.nextLine();
    await d1.nextLine();
    c1.destroy();
    check('8 对端断开后本端被断开', await d1.closedWithin(3000), 'closed=true');

    // ---- 用例 9：健康检查 ----
    const h = await health(hp);
    check(
      '9 GET /health 返回 200 + JSON',
      h.status === 200 && h.json.status === 'ok' && h.json.paired >= 2 && h.json.bytes_forwarded > big.length,
      JSON.stringify({ status: h.status, rooms: h.json.rooms, paired: h.json.paired, bytes: h.json.bytes_forwarded }),
    );
    const rejected = await new Promise((resolve, reject) => {
      require('http').get({ host: '127.0.0.1', port: hp, path: '/nope' }, (res) => resolve(res.statusCode)).on('error', reject);
    });
    check('9b 未知 HTTP 路径返回 404', rejected === 404, `status=${rejected}`);

    // ---- 用例 10：PING 存活探测 ----
    const ping = await Peer.connect(tcpPort);
    ping.write('PING\n');
    const pong = await ping.nextLine();
    check('10 PING 返回 PONG', pong.startsWith('PONG '), pong);
    ping.destroy();

    // ---- 用例 11：限流（独立实例，阈值 2）----
    const rl = await startServer({ RELAY_AUTH_MODE: 'static', RELAY_TOKENS: pairA.token, RELAY_MAX_AUTH_FAILURES: '2' });
    const rlPort = rl.tcp_port;
    try {
      const codes = [];
      for (let i = 0; i < 3; i += 1) {
        const c = await Peer.connect(rlPort);
        c.write(`JOIN ${pairA.room} wrong-token-${i}\n`);
        codes.push(await c.nextLine());
        await c.closedWithin(1000);
      }
      check(
        '11 连续鉴权失败后被限流（rate-limited）',
        codes[0] === 'ERR invalid-token' && codes[1] === 'ERR invalid-token' && codes[2] === 'ERR rate-limited',
        codes.join(' | '),
      );
      const alive = await Peer.connect(rlPort);
      alive.write(`JOIN ${pairA.room} ${pairA.token}\n`);
      check('11b 限流期间合法 token 也被拒（IP 维度熔断）', (await alive.nextLine()) === 'ERR rate-limited');
    } finally {
      rl.proc.kill('SIGTERM');
      await delay(300);
    }

    // ---- 用例 12：open 模式（仅调试用）----
    const open = await startServer({ RELAY_AUTH_MODE: 'open', RELAY_TOKENS: '' });
    try {
      const o = await Peer.connect(open.tcp_port);
      o.write(`JOIN ${pairA.room} anything\n`);
      check('12 open 模式下任意 token 可入（调试模式）', /^OK /.test(await o.nextLine()));
      o.destroy();
    } finally {
      open.proc.kill('SIGTERM');
      await delay(300);
    }

    // ---- 用例 13a：HTTP 停口触发与信号同一优雅停机路径（Windows 也可验证）----
    const shutdownResult = await new Promise((resolve) => {
      p.on('exit', (code, sig) => resolve({ code, sig }));
      const req = require('http').request({ host: '127.0.0.1', port: hp, path: '/shutdown', method: 'POST' }, (res) => res.resume());
      req.on('error', () => {});
      req.end();
      setTimeout(() => resolve(null), 4000);
    });
    check(
      '13a POST /shutdown 触发优雅停机（exit code=0）',
      shutdownResult !== null && shutdownResult.code === 0 && !shutdownResult.sig,
      JSON.stringify(shutdownResult),
    );

    // ---- 用例 13b/13c：停口默认关闭时拒绝；SIGTERM 路径 ----
    const sigSrv = await startServer({ RELAY_AUTH_MODE: 'static', RELAY_TOKENS: pairA.token });
    const forbidden = await new Promise((resolve) => {
      const req = require('http').request({ host: '127.0.0.1', port: sigSrv.health_port, path: '/shutdown', method: 'POST' }, (res) => {
        res.resume();
        resolve(res.statusCode);
      });
      req.on('error', () => resolve(0));
      req.end();
    });
    check('13b 停口未开启时 POST /shutdown 返回 403', forbidden === 403, `status=${forbidden}`);
    if (process.platform === 'win32') {
      skip(
        '13c SIGTERM 优雅停机',
        'Windows 无真实信号：process.kill(pid,"SIGTERM") 实为 TerminateProcess，不执行信号处理函数；该路径需在 Linux/Docker 验证（docker stop 发的就是 SIGTERM）',
      );
      sigSrv.proc.kill('SIGTERM');
    } else {
      const r = await new Promise((resolve) => {
        sigSrv.proc.on('exit', (code, sig) => resolve({ code, sig }));
        sigSrv.proc.kill('SIGTERM');
        setTimeout(() => resolve(null), 4000);
      });
      check('13c SIGTERM 优雅停机（exit code=0）', r !== null && r.code === 0 && !r.sig, JSON.stringify(r));
    }
    await delay(200);

    // ---- 用例 14：日志脱敏（不出现明文 token）----
    const logText = allLogs.join('\n');
    check(
      '14 日志里没有明文 token（只记录摘要前缀）',
      !logText.includes(pairA.token) && /auth-failed/.test(logText),
      `token 未泄漏；日志 ${allLogs.length} 行`,
    );
  } finally {
    try {
      p.kill('SIGKILL');
    } catch {
      /* ignore */
    }
  }

  process.stdout.write(
    `\n=== 结果：${results.length - failures}/${results.length} 通过${skipped ? `，${skipped} 跳过（平台限制）` : ''} ===\n`,
  );
  process.exitCode = failures === 0 ? 0 : 1;
}

main().catch((err) => {
  process.stderr.write(`自测异常：${err && err.stack ? err.stack : err}\n`);
  process.exitCode = 1;
});
