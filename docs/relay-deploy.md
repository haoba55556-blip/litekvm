# LiteKVM 自部署中继部署指南

跨网互联（功能 1）需要一台你自己控制的 VPS 跑中继。中继只转发密文，看不到你的键鼠数据。

## 部署三步（4G 内存 VPS 即可）

### 1. 装 Docker（官方脚本）

```bash
curl -fsSL https://get.docker.com | sh
sudo systemctl enable --now docker
```

### 2. 起中继容器

```bash
# 把整个 relay/ 目录传到 VPS 后：
cd relay
sudo docker compose up -d
sudo docker logs -f litekvm-relay   # 看到 "listening on :25910" 即成功
```

### 3. 防火墙只开需要的端口

```bash
sudo ufw allow 22/tcp
sudo ufw allow 25910/tcp
sudo ufw enable
```

## 生成房间令牌（token）

每对设备一个随机 token，就是房间密码：

```bash
# VPS 或任意 Linux/Mac
openssl rand -hex 16
# Windows PowerShell
[guid]::NewGuid().ToString("N")
```

## 客户端配置（两端都要填）

LiteKVM → 菜单 Edit → LiteKVM Pro 设置…：

| 字段 | 值 |
|---|---|
| 中继服务器地址 | `你的VPS公网IP:25910` |
| 房间令牌 | 上一步生成的 token |

两端填**同一个地址 + 同一个 token**，一端选主机（被控端）、一端选访客（控制端）。

## 安全模型

- **端到端加密**：键鼠数据在 deskflow TLS 层加密后才进中继，中继只能看到密文
- **token 即凭证**：拿到 token 才能进房间，请勿外传；泄露后换一个即可
- **房间即用即毁**：双方断开后房间保留 10 分钟（防闪断重连），之后自动回收
- **容量硬限制**：每个房间最多 2 个连接、1 host + 1 guest，超出直接拒绝

## 常见问题

**Q: 两台电脑都能上外网，但连不上中继？**
先在 VPS 上 `sudo docker ps` 确认容器活着；本机 `telnet VPS_IP 25910` 确认端口通；云厂商控制台的**安全组**也要放行 25910（ufw 之外还有一层）。

**Q: 中继挂了会影响局域网直连吗？**
不会。局域网设备走 mDNS 直连，中继只在跨网时使用。

**Q: 流量经中继会慢吗？**
键鼠事件数据量极小（每秒几 KB），中继转发延迟通常 <5ms（同城 VPS），远低于人手感知阈值。

## 协议速查（开发者）

```
帧格式: 2字节大端长度 + JSON/数据载荷
C->R:  {"type":"REGISTER","room":"<token>","role":"host"|"guest"}
R->C:  {"type":"PEER_JOINED"}   双方到齐时各发一次
R->C:  {"type":"ROOM_FULL"}     房间已有2人
R->C:  {"type":"ROLE_TAKEN"}    同角色重复注册
之后:  原始字节在 host/guest 之间 1:1 转发
```
