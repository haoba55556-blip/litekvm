#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0
/*
 * LiteKVM relay token / room 派生工具（HKDF-SHA256）
 *
 * 这个文件是「客户端侧派生逻辑」的可执行参考实现：中继服务器的 token 来自
 * 两台设备配对时得到的共享密钥，两端各自本地派生，不经过网络交换。
 *
 *   sharedSecret = 配对阶段双方导出的 32 字节共享密钥（见 README《Token 派生规范》）
 *   room  = base64url( HKDF-SHA256(ikm=sharedSecret, salt="litekvm-relay-v1", info="ROOM_ID",     L=16) )
 *   token = base64url( HKDF-SHA256(ikm=sharedSecret, salt="litekvm-relay-v1", info="RELAY_TOKEN", L=32) )
 *
 * 用法：
 *   node tools/derive-token.js                      # 随机生成一组「模拟配对共享密钥」并输出
 *   node tools/derive-token.js <hex-shared-secret>  # 用给定的 32 字节密钥派生
 *   node tools/derive-token.js --other <hex>        # 打印「另一侧同样能算出的值」用于核对
 */
'use strict';

const crypto = require('crypto');

const RELAY_SALT = 'litekvm-relay-v1';

function hkdf(ikm, info, length) {
  // node 内置 HKDF：RFC 5869
  return Buffer.from(crypto.hkdfSync('sha256', ikm, Buffer.from(RELAY_SALT), Buffer.from(info), length));
}

function b64url(buf) {
  return buf.toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function derive(sharedSecret) {
  const ikm = Buffer.isBuffer(sharedSecret) ? sharedSecret : Buffer.from(sharedSecret, 'hex');
  if (ikm.length !== 32) {
    throw new Error(`共享密钥必须是 32 字节（64 位 hex），当前 ${ikm.length} 字节`);
  }
  return {
    sharedSecretHex: ikm.toString('hex'),
    room: b64url(hkdf(ikm, 'ROOM_ID', 16)),
    token: b64url(hkdf(ikm, 'RELAY_TOKEN', 32)),
  };
}

function main() {
  const args = process.argv.slice(2).filter((a) => a !== '--other');
  const hex = args[0];
  const secret = hex ? Buffer.from(hex, 'hex') : crypto.randomBytes(32);
  const out = derive(secret);
  process.stdout.write(
    `${JSON.stringify(
      {
        ...out,
        relay_url: 'kvm://relay.example.com:25902',
        next_step: '把 token 填进中继服务器 .env 的 RELAY_TOKENS，把它和 room 一起填进两端客户端设置页',
        note: '两侧设备独立派生出相同的 room/token；示例中的 relay_url 请替换成你的 VPS 地址',
      },
      null,
      2,
    )}\n`,
  );
}

if (require.main === module) main();

module.exports = { derive, hkdf, b64url, RELAY_SALT };
