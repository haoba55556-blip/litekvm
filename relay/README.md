# LiteKVM 自部署跳网中继（Phase 5 / Task 5.1）

两台设备不在同一个局域网时，LiteKVM 客户端可以把键鼠/剪贴板流量交给**你自己跑的中继**转发。
中继是一个零依赖的 Node.js 小程序，`docker compose up -d` 一条命令起；不解析任何业务内容，
只做「两个客户端碰头 + 双向字节搬运」。

- **鉴权**：两端必须带同一把配对派生的**令牌**，服务器只认识自己白名单里的令牌（只存 SHA-256 摘要）。
- **配对**：两个客户端 `JOIN` 同一个 **room**，凑齐后服务器回 `PAIRED`，此后进入纯字节转发。
- **不做 TLS**：中继看不到也改不了内容（密钥在两端），加密由客户端自己的 TLS 通道负责。见《安全模型》。

```
设备 A ──┐                                   ┌── 设备 B
         │  TLS（两端各自加密，中继穿不透）      │
      ┌──▼──────────── 你的 VPS ──────────────▼──┐
      │ relay: JOIN <room> <token> → PAIRED      │
      │ 之后：socket A ⇄ socket B 纯字节搬运       │
      └──────────────────────────────────────────┘
```

---

## 1. 30 秒本机验证（装 Node ≥ 18 即可，无需 Docker）

```bash
cd relay

# 1) 算一组「模拟配对共享密钥」派生的 room + token
node tools/derive-token.js
# 2) 拿上面输出里的 token 起服务（room 由客户端自己算）
RELAY_TOKENS="<粘贴 token>" node server.js
# 3) 另开两个终端，各跑一个最小客户端，眼看它转发
node test/peer-cli.js 127.0.0.1 25902 <room> <token> --keep
node test/peer-cli.js 127.0.0.1 25902 <room> <token> --send "hello-from-A"
# 4) 健康检查
curl -s http://127.0.0.1:8080/health
```

自动化自测（13 组用例，含鉴权反例、1 MiB 双向转发、房间隔离、限流、优雅停机）：

```bash
node test/selftest.js      # 或 npm test
```

> **本地没有装 Docker 也能做的验证**：本机（Windows + Node v22）已实测通过，见
> `test/selftest.js` 输出 `24/24 通过`。Docker 镜像本机未构建（开发机无 Docker），
> Dockerfile / compose 路径需在 Linux 上再跑一次 `docker compose up -d` 复核。

## 2. Docker Compose 一条命令启动

```bash
cd relay
cp .env.example .env
# 编辑 .env：RELAY_TOKENS=<你的令牌>（可多个，逗号分隔）
docker compose up -d
docker compose ps          # STATUS 应为 healthy
docker compose logs -f     # JSON 日志
```

## 3. VPS 部署教程（Ubuntu 22.04+，1 核 1G 内存 / 一般 4G 小鸡都绰绰有余）

中继只搬运字节，不做转码，CPU/内存占用可以忽略；吃的是**出口带宽**（键鼠事件每秒几 KB，
文件剪贴板传大文件时才吃带宽）。

```bash
# ① 装 Docker（官方脚本，国内机器可换镜像源）
curl -fsSL https://get.docker.com | sh
sudo systemctl enable --now docker
docker compose version      # 需要 compose v2

# ② 放代码。三种方式任选：
#    a) git clone 你的 LiteKVM 仓库，cd 到 relay/
#    b) 只把这个 relay/ 目录 scp 上去：scp -r relay user@vps:/opt/litekvm-relay
#    c) 自己 build 好镜像推到私有 registry，服务器上只用 image: 起

# ③ 配置令牌
cd /opt/litekvm-relay
cp .env.example .env
# 在客户端「设置 → 自定义中继 → 复制令牌」拿到令牌（见第 4 节），粘进 .env 的 RELAY_TOKENS=
chmod 600 .env

# ④ 起服务
docker compose up -d && docker compose logs -f
# 期望日志：
# {"event":"listening","tcp_port":25902,"health_port":8080,"auth_mode":"static","token_count":1,...}

# ⑤ 放行端口（只放中继端口，健康检查端口不要开公网）
sudo ufw allow 25902/tcp                          # Debian/Ubuntu
sudo firewall-cmd --permanent --add-port=25902/tcp && sudo firewall-cmd --reload   # CentOS/RHEL
# 云厂商（阿里云/腾讯云/AWS）还要在控制台安全组放行 25902/tcp

# ⑥ 验证外部连通（在你自己电脑上执行）
nc -vz <你的VPS公网IP> 25902
```

运维要点：

| 事项 | 做法 |
|---|---|
| 开机自启 / 崩溃自愈 | compose 里已写 `restart: unless-stopped` |
| 优雅停机 | `docker compose down`（Linux 下 `docker stop` 发 SIGTERM，服务会先关连接再退出） |
| 升级 | `git pull && docker compose up -d --build` |
| 看状态 | `docker compose ps`、`curl -s http://127.0.0.1:8080/health`（room/peers/转发字节数/鉴权失败数） |
| 令牌轮换 | 改 `.env` 的 `RELAY_TOKENS`（可同时保留新旧，逗号分隔）→ `docker compose up -d` |
| 日志轮转 | compose 里已限制 `max-size=10m`、`max-file=3` |
| 卸载 | `docker compose down --rmi local` |
| 反代/TLS | **不需要**。中继不是 HTTP 服务，直接裸 TCP；health 端口只绑 127.0.0.1 |

> 内存与并发：每个已配对房间只占两个 socket（几百字节缓冲），内存随房间数线性增长，
> 100 个并发房间也就几十 MB。真正的瓶颈是 VPS 的月流量配额。

## 4. 客户端怎么填（relay URL / token）

客户端「设置 → 自定义中继」三个字段（不填则走局域网直连，行为不变）：

| 字段 | 填写内容 | 举例 |
|---|---|---|
| 中继地址 | 裸 `host:port`（也接受 `kvm://host:port` / `http://host:port`，客户端会剥掉 scheme） | `relay.example.com:25902` 或 `1.2.3.4:25902` |
| 房间号 | 两端**自动派生**，一般不用手填；手填时两端必须完全一致 | `z7p_yhgBI6qV_-bQZsBqvw` |
| 中继令牌 | 两端**自动派生**，一般不用手填；手填时也要和服务器白名单一致 | `iD2H25VQw2Y4GRxJBhcGs8Y9eT_…` |

使用流程：

1. 设备 A、B 先按 Phase 2 的流程完成**本地配对**（PIN + 公钥互信）。
2. 客户端发现同网直连失败（或用户在设置里选了「始终走中继」）时，用配对共享密钥派生出
   room/token，`JOIN` 到你填的中继地址。
3. 用户只需把一个值交给服务器运维者：**令牌**。做法是在设置页点「复制令牌」，
   粘进 VPS 的 `.env` → `RELAY_TOKENS=`。两端设备的派生结果天然相同，无需手工对齐。

**客户端侧接入点（本仓库 C++ 侧待实现，本任务只交付 relay/）：**

- 派生：新增 `src/lib/litekvm/RelayCrypto.{h,cpp}`（复用 `PairingCrypto` 的 HKDF 思路，
  一致实现见第 5 节公式）。**本任务未改动 `src/` 下任何文件。**
- 连接：复用现有 `QTcpSocket`，握手按第 6 节；配对后 `readyRead` 直接把字节塞进 deskflow
  的加密通道（相当于把原来的直连 socket 换成中继 socket，上层协议零改动）。
- 配置持久化：建议加在 `TrustStore` 同级的一个 `relay.json`（地址/是否启用/是否手工覆盖 token）。

## 5. 令牌从哪来（派生规范）

令牌**不是**服务器随机生成的，而是两端设备配对共享密钥的 HKDF 派生结果，服务器只需知道白名单：

```
sharedSecret = 32 字节，配对阶段双方各自导出的同一把共享密钥
               （与 PairingCrypto 的 PAIR_CODE 同源；客户端接入时从同一 HKDF 上下文再岔一路即可）
room  = base64url( HKDF-SHA256(ikm = sharedSecret, salt = "litekvm-relay-v1", info = "ROOM_ID",     L = 16) )
token = base64url( HKDF-SHA256(ikm = sharedSecret, salt = "litekvm-relay-v1", info = "RELAY_TOKEN", L = 32) )
```

- `salt` 固定为 `litekvm-relay-v1`，与配对用的 `litekvm-pair-v1` 分开，避免同一密钥直接复用。
- 参考实现（可执行）：`tools/derive-token.js`；C++ 侧对应 OpenSSL `EVP_PKEY_HKDF` / `HKDF()`。
- room 16 字节 / token 32 字节 base64url：43 字符，能安全放进 `.env`、命令行和配置 JSON。
- `ROOM_ID` 里没有秘密，所以房间号可以随便露；**token 是准入凭证，等同于密码，别公开**。

## 6. 协议（透明转发，中继不解析内容）

握手是换行结尾的纯文本，最多 512 字节，10 秒不完成即断开：

| 方向 | 报文 | 说明 |
|---|---|---|
| C → R | `JOIN <room> <token>` | room 为 8–64 位 `[A-Za-z0-9_-]` |
| R → C | `OK <connId> <peerIndex>` | 鉴权通过、已占位（peerIndex 从 1 起） |
| R → 双方 | `PAIRED <peerCount>` | 凑齐后下发；**此后不再注入任何控制字节** |
| R → C | `ERR <code>` | 随后立即关闭连接 |
| C → R | `PING` | 存活探测，回 `PONG <version>`（仅握手阶段） |

错误码：`invalid-token` / `missing-token` / `bad-room` / `bad-command` / `room-full` /
`handshake-too-large` / `handshake-timeout` / `rate-limited`。

配对完成后中继进入 splice 模式（`socket.pipe`）：

- **不解析、不改写、不缓存**业务字节；连配对前客户端提前发出的数据也会一字不差地补发
  （已实测 4096 字节含全部 256 种字节值，SHA-256 一致）。
- 对端离线不做任何"通知帧"，直接用 TCP FIN/RST 表达，客户端收到断开后重新 `JOIN` 即可。
- 因此上层协议（deskflow 加密通道、文件分块等）**完全不需要为中继做兼容改动**。

## 7. 配置项

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `RELAY_PORT` | `25902` | 中继端口（`0` = 随机，便于测试） |
| `RELAY_HOST` | `0.0.0.0` | 监听地址 |
| `RELAY_HEALTH_PORT` | `8080` | 健康检查/指标 HTTP 端口，`-1` 关闭 |
| `RELAY_TOKENS` | 空 | 令牌白名单，逗号分隔（**空 = 全部拒绝**） |
| `RELAY_TOKENS_FILE` | 空 | 文件白名单，每行一个，`#` 注释；与上面合并 |
| `RELAY_AUTH_MODE` | `static` | `static` 白名单校验 / `open` 不校验（**仅本机调试**） |
| `RELAY_ROOM_MAX_PEERS` | `2` | 单房间端数上限（键鼠共享就是 2） |
| `RELAY_MAX_AUTH_FAILURES` | `10` | 同 IP 窗口期内鉴权失败上限，超过则熔断 |
| `RELAY_AUTH_FAILURE_WINDOW_MS` | `60000` | 熔断窗口 |
| `RELAY_HANDSHAKE_TIMEOUT_MS` | `10000` | 握手超时 |
| `RELAY_MAX_HANDSHAKE_BYTES` | `512` | 单次握手最大字节数 |
| `RELAY_ENABLE_HTTP_SHUTDOWN` | 关 | `1` 开启 `POST /shutdown`（仅回环地址，优雅停机） |
| `RELAY_LOG_LEVEL` | `info` | `debug` / `info` / `warn` / `error`，JSON 行日志 |

## 8. 安全模型

1. **准入**：无有效令牌直接 `ERR invalid-token` 断开；令牌比较用 SHA-256 摘要 + 常数时间比较，
   白名单里连明文令牌都不存。日志只记录令牌摘要前 8 位（`token_digest`）。
2. **房间即边界**：不同 room 之间零串流（已实测），猜不到 room 的人连不到你的通道；
   即便猜到 room，没有令牌也进不来。
3. **内容保密**：中继是"哑管子"。deskflow 两端的加密通道（TLS）独立于中继存在，
   中继既没有密钥也不改字节。**默认不给中继加 TLS 是刻意的**：加一层中继自己的 TLS 只会
   增加运维负担（证书续期），并不提升端到端安全性——真正的加密在两端。
4. **抗滥用**：握手超时、握手长度上限、单房间端数上限、同 IP 鉴权失败熔断。
   进一步防护可在 VPS 层加 `fail2ban` 或云厂商防 DDoS。
5. **不要做的事**：不要把 `open` 模式暴露公网；不要把 health 端口 `/health` 开公网
   （compose 默认已绑 127.0.0.1）；不要在多人共享的机器上把 token 写进 shell history。

## 9. 排错

| 现象 | 原因 / 对策 |
|---|---|
| 客户端拿到 `ERR invalid-token` | 令牌不一致：两端必须派生自同一把配对密钥；服务器的 `RELAY_TOKENS` 必须含这个值 |
| `ERR room-full` | 该房间已有两端在线。旧连接没断干净 / 第三端误入，检查 `peers` |
| `ERR rate-limited` | 同 IP 失败次数超阈值，等 `RELAY_AUTH_FAILURE_WINDOW_MS` 后自动恢复 |
| 客户端连不上端口 | 安全组/ufw 未放行 25902；或容器没起（`docker compose ps`） |
| 日志里 `no-tokens-configured` | 忘了设 `RELAY_TOKENS`，此时所有 JOIN 都会被拒绝 |
| 通了但很快断开 | 对端一断，中继就关本端（设计如此）；客户端应自动重新 `JOIN` |
| 延迟高 | 看 VPS 地理位置；中继只增一跳，同城中继端到端增量约 1 个 RTT |
| Windows 上 `docker stop` 不生效 | 这是平台差异，容器场景请用 Linux VPS |

## 10. 自测说明

`test/selftest.js` 会以子进程方式**真的启动**中继、**真的建立 TCP 连接**，覆盖：

| 用例 | 内容 |
|---|---|
| 1 | 正确令牌两端 `OK` + `PAIRED`；配对前发送的 4096 字节补发且 SHA-256 一致 |
| 2 | A→B 1 MiB、B→A 256 KiB 双向转发，`sha256` 逐一比对；打印耗时 |
| 3–6 | 错令牌 / 缺令牌 / 第三端 / 非法命令 / 非法 room 全部被拒并断开 |
| 7 | 两个房间互不串流 |
| 8 | 对端断开 → 本端被断开 |
| 9 | `GET /health` 200 + JSON 指标；未知路径 404 |
| 10 | `PING` → `PONG` |
| 11 | 同 IP 连续鉴权失败后熔断（含合法令牌也被拒） |
| 12 | `open` 调试模式 |
| 13 | `POST /shutdown` 优雅停机（exit 0）；停口未开启时 403；SIGTERM 路径 |
| 14 | 日志不含明文令牌 |

> 说明：**Windows 上 SIGTERM 无法验证**（`process.kill` 走 `TerminateProcess`，不执行信号处理函数），
> 脚本会打印 `[SKIP]` 并说明原因；Linux 上同一用例会真正验证。Windows 本机改由与信号
> **共用同一条停机代码路径**的 `POST /shutdown` 覆盖（用例 13a）。

手工两终端演示用 `test/peer-cli.js`（见第 1 节）；`--bad-token` 可复现被拒场景。

## 11. 后续：移植成 Go 单二进制（接口对照）

计划原文写的是 Go + libpion/turn，但当前开发机没有 Go/Docker，MVP 用 Node 落地以「立刻可跑可验」。
若之后要单二进制分发（发版免依赖），行为逐项对齐即可：

| 当前 (Node) | Go 对应 |
|---|---|
| `net.createServer` + `JOIN` 行解析 | `net.Listen("tcp")` + `bufio.Reader.ReadString('\n')` |
| `socket.pipe(peer)` splice | `io.Copy(dst, src)` ×2 或 `net.TCPConn.ReadFrom/WriteTo`（零拷贝） |
| `crypto.timingSafeEqual` 令牌比较 | `crypto/subtle.ConstantTimeCompare` |
| `crypto.hkdfSync`（`tools/derive-token.js`） | `golang.org/x/crypto/hkdf` |
| `Map<roomId, peers>` 房间表 | `sync.Map` / `map[string][]*peer` + `sync.Mutex` |
| `http.createServer` `/health` | `net/http` 同路径同 JSON 字段 |
| 环境变量 `RELAY_*` | 完全同名，用 `os.Getenv` + 相同默认值 |
| JSON 行日志字段 | 同名同结构（`event` / `room` / `conn` / `peer_index` / `bytes_forwarded`） |

协议不变 = 客户端不用跟着改。若未来真要上 TURN/WebRTC（P2P 打洞降延迟），
本中继可作为「直连失败时的兜底」长期保留。

## 12. 已知限制

- 仓库里客户端 C++ 侧的「自定义中继」设置页与 `JOIN` 逻辑**尚未实现**（本任务只交付 relay/）。
- 中继不做房间持久化：进程重启后所有连接断开，客户端重连即可。
- 不支持「同房间多端转发」（`ROOM_MAX_PEERS=2` 的键鼠一对一模型）；多机串联请用多对房间。
- 未做流量限额/统计持久化；`/health` 的字节数会随进程重启归零。
