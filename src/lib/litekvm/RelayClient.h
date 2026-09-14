// SPDX-License-Identifier: GPL-2.0
// LiteKVM 自部署中继客户端 —— 把一段 TCP 字节流交给用户自己跑的中继服务器转发，
// 让两台不在同一局域网的设备也能碰头（Phase 5 / Task 5.1 的客户端部分）。
//
// 协议见 relay/README.md §6（换行结尾的纯文本握手，最多 512 字节）：
//   C -> R : JOIN <room> <token>\n        room 8-64 位 [A-Za-z0-9_-]
//   R -> C : OK <connId> <peerIndex>\n    鉴权通过、已占位（peerIndex 从 1 起）
//   R -> 双方 : PAIRED <peerCount>\n      凑齐后下发，此后中继只搬字节
//   R -> C : ERR <code>\n                 随后立即断开
//   C -> R : PING -> PONG <version>       仅握手阶段（本类不主动发）
//
// 本类只做「连接 + 握手 + 收发字节」，不解析上层内容：调用方在 paired() 之后就可以
// 把 send() / dataReceived() 当成一条透明管道（README §6：配对后中继不解析、不改写、
// 不缓存业务字节，所以 deskflow 原有的加密通道、文件分块协议都不用为中继改一行）。
//
// 对端离线时中继直接断开本端（不注入任何"通知帧"），因此这里在断开后按指数退避自动
// 重连并重新 JOIN；重新放行（改 .env 里的 RELAY_TOKENS 后 docker compose up -d）
// 也能这么自动恢复。
//
//   litekvm::RelayClient relay;
//   connect(&relay, &litekvm::RelayClient::paired, ...);        // 通道就绪
//   connect(&relay, &litekvm::RelayClient::dataReceived, ...);  // 对端字节
//   relay.setConfig(cfg);
//   relay.start();
//
// 注意：本类默认不做 TLS，与中继服务端一致（README §8.3）——中继是"哑管子"，
// 真正的加密由上层通道负责；token 只是准入凭证，等同密码，别外泄。
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpSocket;
class QTimer;

namespace litekvm {

/// 中继默认端口，与 relay/server.js 的 RELAY_PORT 默认值一致。
inline constexpr quint16 kDefaultRelayPort = 25902;

/**
 * @brief 一套完整的中继配置（设置页四个字段 + 是否启用）。
 */
struct RelayConfig
{
  bool enabled = false;
  QString host;                      ///< 裸主机名 / IP，不含 scheme、不含端口
  quint16 port = kDefaultRelayPort;  ///< 中继 TCP 端口
  QString token;                     ///< 准入令牌（服务器白名单里必须有这个值）
  QString room;                      ///< 留空 = 由令牌派生，两端同令牌即同房间

  bool operator==(const RelayConfig &other) const
  {
    return enabled == other.enabled && host == other.host && port == other.port && token == other.token &&
           room == other.room;
  }
  bool operator!=(const RelayConfig &other) const
  {
    return !(*this == other);
  }
};

/**
 * @brief 单个中继连接：JOIN 握手 + 透明字节转发 + 断线自动重连。
 *
 * 同一时刻只维护一条到中继的 TCP 连接（键鼠共享就是两台设备一对一）。
 */
class RelayClient : public QObject
{
  Q_OBJECT

public:
  enum class State {
    Disabled,       ///< 用户没启用中继
    Idle,           ///< 启用了但还没开始连（配置不全等）
    Connecting,     ///< TCP 连接中
    Handshaking,    ///< 已连上，等 OK/ERR
    WaitingForPeer, ///< 已 JOIN 成功（OK），等对端凑齐
    Paired,         ///< 收到 PAIRED，可以收发字节
    Error,          ///< 连接/鉴权失败（一般会退避重试）
  };
  Q_ENUM(State)
  static QString toString(State state);

  // ------------------------------------------------------------ 解析与派生
  /**
   * @brief 解析用户填的地址：接受 `host`、`host:port`、`kvm://host:port`、
   * `http://host:port/path`、`[::1]:25902`；scheme 与路径会被剥掉。
   * 端口写不通时按 @p defaultPort 处理（host 原样保留，让用户看到真实的 DNS 报错）。
   * @return 只填好 host/port 的配置（enabled/token/room 保持默认，由调用方补）。
   */
  static RelayConfig parseEndpoint(const QString &raw, quint16 defaultPort = kDefaultRelayPort);

  /// 房间号规则与 relay/server.js 一致：8-64 位 [A-Za-z0-9_-]。
  static bool isValidRoom(const QString &room);

  /**
   * @brief 房间号留空时的兜底派生：
   * base64url( HKDF-SHA256(ikm = token, salt = "litekvm-relay-v1", info = "ROOM_ID", L = 16) )
   * 两端令牌相同就落在同一个房间，用户只需要填「地址 + 端口 + 令牌」三个字段。
   * 房间号不是秘密（README §5），真正的边界是令牌。
   */
  static QString roomForToken(const QString &token);

  /// README §5 规范派生：HKDF(配对共享密钥, salt="litekvm-relay-v1", info="ROOM_ID", 16) → base64url。
  /// 32 字节共享密钥来自配对阶段（当前配对流程还没持久化它，先备好给后续接入用）。
  static QString deriveRoom(const QByteArray &sharedSecret);

  /// README §5 规范派生：HKDF(配对共享密钥, salt="litekvm-relay-v1", info="RELAY_TOKEN", 32) → base64url。
  /// 输出就是要粘进中继服务器 .env 的 RELAY_TOKENS 值。
  static QString deriveToken(const QByteArray &sharedSecret);

  // ------------------------------------------------------------------ 生命周期
  explicit RelayClient(QObject *parent = nullptr);
  ~RelayClient() override;

  /// 换配置：与当前配置相同则什么都不做；启用则（重）连，禁用则断开。
  void setConfig(const RelayConfig &config);
  [[nodiscard]] const RelayConfig &config() const
  {
    return m_config;
  }

  /// 实际会用的房间号：手工填的（合法时），否则由令牌派生。
  [[nodiscard]] QString effectiveRoom() const;

  [[nodiscard]] bool enabled() const
  {
    return m_config.enabled;
  }
  [[nodiscard]] bool paired() const
  {
    return m_state == State::Paired;
  }
  [[nodiscard]] State state() const
  {
    return m_state;
  }

  /// 最近一次失败的人话描述（成功连上/配对后清空）；空 = 无错误。
  [[nodiscard]] QString lastError() const
  {
    return m_lastError;
  }

public Q_SLOTS:
  /// 启用并按当前配置连接（幂等：已在连接/已配对时什么都不做）。
  void start();
  /// 断开并停用（用户关掉中继开关时调用）。
  void stop();
  /// 已配对时原样交给对端；未配对时先缓存，配对后自动补发（上限见 m_pending 注释）。
  void send(const QByteArray &data);

Q_SIGNALS:
  void stateChanged(litekvm::RelayClient::State state);
  /// 鉴权通过（收到 OK），此时只差对端。
  void authenticated(const QString &connId, int peerIndex);
  /// 两端凑齐，通道就绪；之后才能放心 send()。
  void paired();
  /// 通道断了：对端离线、中继重启、本机断网或用户停用。reason 已拼好可直接展示。
  void unpaired(const QString &reason);
  /// 对端字节（握手阶段之后中继不再注入任何控制字节，这里收到的就是纯业务数据）。
  void dataReceived(const QByteArray &data);
  /// 失败原因（同一条错误只报一次，避免每次退避重试都弹一次）。
  void errorOccurred(const QString &message);

private Q_SLOTS:
  void onConnected();
  void onReadyRead();
  void onDisconnected();
  void onSocketError();
  void onHandshakeTimeout();
  void onReconnectTimeout();

private:
  /// 建连（含配置校验）；配置不全时直接进 Error 且不重试。
  void connectToRelay();
  /// 解析一行握手报文。
  void handleHandshakeLine(const QByteArray &line);
  /// PAIRED 之后：把已收到的字节交给上层。
  void flushIncoming();
  /// 把配对前缓存的数据补发出去。
  void flushPending();
  /// 断开底层 socket（不发信号、不安排重连）。
  void teardownSocket();
  /// teardownSocket() + 若本来是活通道则发 unpaired()。
  void closeChannel(const QString &reason);
  /// 安排一次退避重连（幂等：同一个失败可能同时来自 errorOccurred 与 disconnected）。
  void scheduleReconnect(const QString &reason);
  /// 记一条错误并（同一内容只）发一次 errorOccurred。
  void reportError(const QString &message);
  void setState(State state);

  RelayConfig m_config;

  QTcpSocket *m_socket = nullptr;
  QTimer *m_handshakeTimer = nullptr;
  QTimer *m_reconnectTimer = nullptr;

  /// 握手阶段的按行缓冲；PAIRED 之后不再使用。
  QByteArray m_rxBuffer;
  /// PAIRED 之前缓存的上行数据（避免往透明通道里塞"配对前残留"）。
  QByteArray m_pending;
  bool m_pendingOverflow = false;

  State m_state = State::Disabled;
  QString m_lastError;
  int m_reconnectAttempts = 0;
};

} // namespace litekvm
