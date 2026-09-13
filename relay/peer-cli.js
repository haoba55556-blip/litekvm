#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0
/*
 * 手工验证用最小客户端（相当于 LiteKVM 客户端里那一段中继连接的占位实现）。
 * 用于两开终端对着一个真在跑的中继服务做眼见为实的转发验证。
 *
 *   # 终端 1
 *   node test/peer-cli.js 127.0.0.1 25902 <room> <token> --send "hello-from-A"
 *   # 终端 2
 *   node test/peer-cli.js 127.0.0.1 25902 <room> <token> --keep
 *
 * 参数：--send <文本>  配对成功后发送这段文本
 *       --keep        转发完不退出，继续回显收到的字节（Ctrl+C 结束）
 *       --bad-token   故意用错令牌，验证鉴权拒绝
 */
'use strict';

const net = require('net');

function usage() {
  process.stdout.write(
    'usage: node test/peer-cli.js <host> <port> <room> <token> [--send "text"] [--keep] [--bad-token]\n',
  );
  process.exit(2);
}

const [host, portRaw, room, rawToken] = process.argv.slice(2);
if (!host || !portRaw || !room || !rawToken) usage();
const port = Number.parseInt(portRaw, 10);
const token = process.argv.includes('--bad-token') ? `${rawToken}-WRONG` : rawToken;
const sendIdx = process.argv.indexOf('--send');
const sendText = sendIdx >= 0 ? process.argv[sendIdx + 1] : '';
const keep = process.argv.includes('--keep');

const sock = net.connect({ host, port }, () => {
  process.stdout.write(`[cli] TCP 已连接 ${host}:${port}\n`);
  sock.write(`JOIN ${room} ${token}\n`);
});
sock.setNoDelay(true);

let paired = false;
let buf = Buffer.alloc(0);

sock.on('data', (chunk) => {
  if (paired) {
    process.stdout.write(`[cli] 收到转发的业务字节 ${chunk.length}B: ${JSON.stringify(chunk.toString('utf8').slice(0, 120))}\n`);
    return;
  }
  buf = Buffer.concat([buf, chunk]);
  while (!paired) {
    const nl = buf.indexOf(0x0a);
    if (nl < 0) break;
    const line = buf.slice(0, nl).toString('utf8');
    buf = buf.slice(nl + 1);
    process.stdout.write(`[cli] 中继应答: ${line}\n`);
    if (line.startsWith('ERR')) {
      process.stdout.write('[cli] 被拒绝，退出\n');
      sock.destroy();
      process.exitCode = 3;
      return;
    }
    if (line.startsWith('PAIRED')) {
      paired = true;
      if (sendText) {
        process.stdout.write(`[cli] 发送: ${JSON.stringify(sendText)}\n`);
        sock.write(sendText);
      }
      if (!keep && !sendText) sock.end();
      if (!keep && sendText) setTimeout(() => sock.end(), 400);
    }
  }
});

sock.on('close', () => {
  process.stdout.write('[cli] 连接关闭\n');
});
sock.on('error', (err) => {
  process.stdout.write(`[cli] 连接错误: ${err.message}\n`);
  process.exitCode = 4;
});
