// SPDX-License-Identifier: GPL-2.0
// LiteKVM 自部署中继客户端（协议见 relay/README.md §5-§6）。
#include "RelayClient.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <QAbstractSocket>
#include <QDebug>
#include <QRegularExpression>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

namespace litekvm {

namespace {

/// 与 relay/server.js 的 RELAY_MAX_HANDSHAKE_BYTES 一致
constexpr int kMaxHandshakeBytes = 512;
/// 服务端握手超时 10s，客户端留 2s 余量，免得两边同时掐
constexpr int kHandshakeTimeoutMs = 12'000;
/// PAIRED 之前最多替上层缓存 1 MiB，防止对端迟迟不来时把内存吃掉
constexpr int kMaxPendingBytes = 1024 * 1024;
/// 令牌长度上限：JOIN 报文要塞进 512 字节里（room ≤ 64 + 固定前缀）
constexpr int kMaxTokenBytes = 400;
constexpr int kReconnectMinMs = 1'000;
constexpr int kReconnectMaxMs = 60'000;

const QByteArray kRelaySalt = QByteArrayLiteral("litekvm-relay-v1");

/// HKDF-Extract + Expand (RFC 5869) with SHA-256，与 PairingCrypto.cpp 同一套写法；
/// 这里走中继自己的 salt/info 上下文（README §5）。
QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len)
{
  // extract
  uint8_t prk[EVP_MAX_MD_SIZE];
  unsigned int prkLen = 0;
  HMAC(EVP_sha256(), salt.constData(), salt.size(), reinterpret_cast<const uint8_t *>(ikm.constData()),
       ikm.size(), prk, &prkLen);

  // expand
  QByteArray okm;
  okm.reserve(len);
  uint8_t t[EVP_MAX_MD_SIZE];
  unsigned int tLen = 0;
  int offset = 0;
  uint8_t iteration = 1;
  while (offset < len) {
    QByteArray input(reinterpret_cast<const char *>(t), tLen);
    input.append(info);
    input.append(char(iteration));
    HMAC(EVP_sha256(), prk, prkLen, reinterpret_cast<const uint8_t *>(input.constData()), input.size(), t, &tLen);
    const int take = qMin(int(tLen), len - offset);
    okm.append(reinterpret_cast<const char *>(t), take);
    offset += take;
    ++iteration;
  }
  return okm;
}

/// base64url 无填充（房间里只允许 [A-Za-z0-9_-]，正好就是 base64url 的字母表）
QString base64Url(const QByteArray &data)
{
  return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

/// 把服务端的错误码翻成人话（对照 relay/README.md §9 排错表）
QString relayErrorText(const QString &code)
{
  if (code == QLatin1String("invalid-token"))
    return QStringLiteral("中继拒绝了连接（invalid-token）：令牌和服务器的 RELAY_TOKENS 不一致");
  if (code == QLatin1String("missing-token"))
    return QStringLiteral("中继拒绝了连接（missing-token）：没有带上令牌");
  if (code == QLatin1String("bad-room"))
    return QStringLiteral("中继拒绝了连接（bad-room）：房间号需要 8-64 位 A-Za-z0-9_-");
  if (code == QLatin1String("bad-command"))
    return QStringLiteral("中继拒绝了连接（bad-command）：握手报文不被识别");
  if (code == QLatin1String("room-full"))
    return QStringLiteral("中继拒绝了连接（room-full）：这个房间已经有两端在线了");
  if (code == QLatin1String("handshake-too-large"))
    return QStringLiteral("中继拒绝了连接（handshake-too-large）：握手表超过 512 字节");
  if (code == QLatin1String("handshake-timeout"))
    return QStringLiteral("中继拒绝了连接（handshake-timeout）：握手超时");
  if (code == QLatin1String("rate-limited"))
    return QStringLiteral("中继拒绝了连接（rate-limited）：同一 IP 鉴权失败次数过多，已被中继熔断");
  return QStringLiteral("中继拒绝了连接（%1）").arg(code.isEmpty() ? QStringLiteral("未知错误") : code);
}

} // namespace

QString RelayClient::toString(State state)
{
  switch (state) {
  case State::Disabled:
    return QStringLiteral("DISABLED");
  case State::Idle:
    return QStringLiteral("IDLE");
  case State::Connecting:
    return QStringLiteral("CONNECTING");
  case State::Handshaking:
    return QStringLiteral("HANDSHAKING");
  case State::WaitingForPeer:
    return QStringLiteral("WAITING_FOR_PEER");
  case State::Paired:
    return QStringLiteral("PAIRED");
  case State::Error:
    return QStringLiteral("ERROR");
  }
  return QStringLiteral("UNKNOWN");
}

RelayClient::RelayClient(QObject *parent) : QObject(parent)
{
  m_handshakeTimer = new QTimer(this);
  m_handshakeTimer->setSingleShot(true);
  m_handshakeTimer->setInterval(kHandshakeTimeoutMs);
  connect(m_handshakeTimer, &QTimer::timeout, this, &RelayClient::onHandshakeTimeout);

  m_reconnectTimer = new QTimer(this);
  m_reconnectTimer->setSingleShot(true);
  connect(m_reconnectTimer, &QTimer::timeout, this, &RelayClient::onReconnectTimeout);
}

RelayClient::~RelayClient() = default;

// ------------------------------------------------------------------ 解析与派生

RelayConfig RelayClient::parseEndpoint(const QString &raw, quint16 defaultPort)
{
  RelayConfig config;
  config.port = defaultPort > 0 ? defaultPort : kDefaultRelayPort;

  QString text = raw.trimmed();
  if (text.isEmpty())
    return config;

  // 剥掉 kvm:// https:// 之类的 scheme（README §4：也可以直接粘完整地址）
  static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
  text.remove(scheme);

  // 只关心 host[:port]，路径 / 查询串一律丢掉
  if (const int slash = text.indexOf(QLatin1Char('/')); slash >= 0)
    text.truncate(slash);

  // IPv6 字面量： [::1]:25902
  if (text.startsWith(QLatin1Char('['))) {
    const int close = text.indexOf(QLatin1Char(']'));
    if (close < 0) {
      config.host = text;
      return config;
    }
    config.host = text.mid(1, close - 1);
    const QString rest = text.mid(close + 1);
    const QString portText = rest.startsWith(QLatin1Char(':')) ? rest.mid(1) : QString();
    bool ok = false;
    if (const uint port = portText.toUInt(&ok); ok && port > 0 && port <= 65535)
      config.port = quint16(port);
    return config;
  }

  // host:port（host 里带不了冒号，取最后一个）
  if (const int colon = text.lastIndexOf(QLatin1Char(':')); colon > 0) {
    bool ok = false;
    const uint port = text.mid(colon + 1).toUInt(&ok);
    if (ok && port > 0 && port <= 65535) {
      config.host = text.left(colon);
      config.port = quint16(port);
      return config;
    }
  }

  // 没写端口（或者端口不是数字）：整串当主机名，让用户看到真实的 DNS 报错
  config.host = text;
  return config;
}

bool RelayClient::isValidRoom(const QString &room)
{
  static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{8,64}\\z"));
  return re.match(room).hasMatch();
}

QString RelayClient::roomForToken(const QString &token)
{
  const QByteArray ikm = token.toUtf8();
  if (ikm.isEmpty())
    return {};
  return base64Url(hkdfSha256(ikm, kRelaySalt, QByteArrayLiteral("ROOM_ID"), 16));
}

QString RelayClient::deriveRoom(const QByteArray &sharedSecret)
{
  if (sharedSecret.isEmpty())
    return {};
  return base64Url(hkdfSha256(sharedSecret, kRelaySalt, QByteArrayLiteral("ROOM_ID"), 16));
}

QString RelayClient::deriveToken(const QByteArray &sharedSecret)
{
  if (sharedSecret.isEmpty())
    return {};
  return base64Url(hkdfSha256(sharedSecret, kRelaySalt, QByteArrayLiteral("RELAY_TOKEN"), 32));
}

QString RelayClient::effectiveRoom() const
{
  if (!m_config.room.isEmpty())
    return m_config.room;
  return roomForToken(m_config.token);
}

// ------------------------------------------------------------------ 生命周期

void RelayClient::setConfig(const RelayConfig &config)
{
  RelayConfig next = config;
  next.host = next.host.trimmed();
  next.token = next.token.trimmed();
  next.room = next.room.trimmed();
  if (next.port == 0)
    next.port = kDefaultRelayPort;

  if (next == m_config)
    return;

  m_config = next;
  m_reconnectAttempts = 0;
  m_reconnectTimer->stop();
  closeChannel(QStringLiteral("中继设置已变更，连接已重开"));

  if (!m_config.enabled) {
    m_lastError.clear();
    setState(State::Disabled);
    return;
  }
  connectToRelay();
}

void RelayClient::start()
{
  if (!m_config.enabled) {
    setState(State::Disabled);
    return;
  }
  if (m_state == State::Connecting || m_state == State::Handshaking || m_state == State::WaitingForPeer ||
      m_state == State::Paired)
    return; // 已经在跑了
  if (m_reconnectTimer->isActive())
    return; // 等退避计时器到点自己会连
  connectToRelay();
}

void RelayClient::stop()
{
  m_config.enabled = false;
  m_reconnectAttempts = 0;
  m_reconnectTimer->stop();
  closeChannel(QStringLiteral("中继已停用"));
  m_lastError.clear();
  setState(State::Disabled);
}

void RelayClient::connectToRelay()
{
  if (!m_config.enabled) {
    setState(State::Disabled);
    return;
  }

  // 配置不全属于"用户还没填完"，退避重试也没意义：填好之后 setConfig() 会重新进来。
  if (m_config.host.isEmpty()) {
    reportError(QStringLiteral("还没填中继地址：请填你自己中继服务器的地址，例如 relay.example.com:25902"));
    setState(State::Error);
    return;
  }
  if (m_config.token.isEmpty()) {
    reportError(QStringLiteral("还没填中继令牌：两台设备用同一个令牌，并把它加进服务器的 RELAY_TOKENS"));
    setState(State::Error);
    return;
  }
  if (m_config.token.toUtf8().size() > kMaxTokenBytes) {
    reportError(QStringLiteral("中继令牌太长（超过 %1 字节），中继的握手包放不下").arg(kMaxTokenBytes));
    setState(State::Error);
    return;
  }
  if (!m_config.room.isEmpty() && !isValidRoom(m_config.room)) {
    reportError(QStringLiteral("房间号不合法：需要 8-64 位 A-Za-z0-9_-（留空则由令牌自动派生）"));
    setState(State::Error);
    return;
  }

  teardownSocket();
  m_rxBuffer.clear();
  m_pendingOverflow = false;

  m_socket = new QTcpSocket(this);
  connect(m_socket, &QTcpSocket::connected, this, &RelayClient::onConnected);
  connect(m_socket, &QTcpSocket::readyRead, this, &RelayClient::onReadyRead);
  connect(m_socket, &QTcpSocket::disconnected, this, &RelayClient::onDisconnected);
  connect(m_socket, &QTcpSocket::errorOccurred, this, &RelayClient::onSocketError);

  setState(State::Connecting);
  m_socket->connectToHost(m_config.host, m_config.port);
}

// ------------------------------------------------------------------ socket 事件

void RelayClient::onConnected()
{
  if (!m_socket)
    return;

  // 关掉 Nagle：键鼠事件是小包，攒着只会加延迟（服务端也 setNoDelay(true)）
  m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

  QByteArray join;
  join.append(QByteArrayLiteral("JOIN "));
  join.append(effectiveRoom().toUtf8());
  join.append(QLatin1Char(' '));
  join.append(m_config.token.toUtf8());
  join.append(QLatin1Char('\n'));
  m_socket->write(join);

  setState(State::Handshaking);
  m_handshakeTimer->start();
}

void RelayClient::onReadyRead()
{
  if (!m_socket)
    return;

  m_rxBuffer.append(m_socket->readAll());

  if (m_state == State::Paired) {
    flushIncoming();
    return;
  }

  // 握手阶段：一行一条报文
  while (true) {
    const qsizetype nl = m_rxBuffer.indexOf('\n');
    if (nl < 0) {
      if (m_rxBuffer.size() > kMaxHandshakeBytes) {
        const QString message = QStringLiteral("中继握手包超过 %1 字节，已断开").arg(kMaxHandshakeBytes);
        reportError(message);
        teardownSocket();
        scheduleReconnect(message);
      }
      return;
    }

    const QByteArray line = m_rxBuffer.left(nl).trimmed();
    m_rxBuffer.remove(0, nl + 1);
    handleHandshakeLine(line);

    if (!m_socket || m_state == State::Paired)
      break;
  }

  // PAIRED 那一包里可能紧跟着对端的业务字节（中继配对后立刻 splice），别丢
  if (m_state == State::Paired)
    flushIncoming();
}

void RelayClient::handleHandshakeLine(const QByteArray &line)
{
  const QStringList parts = QString::fromUtf8(line).split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (parts.isEmpty())
    return;

  const QString command = parts.first().toUpper();

  if (command == QLatin1String("OK")) {
    // 鉴权通过：重置退避，之后只剩"等对端"
    m_handshakeTimer->stop();
    m_reconnectAttempts = 0;
    m_lastError.clear();
    setState(State::WaitingForPeer);
    Q_EMIT authenticated(parts.value(1), parts.value(2).toInt());
    return;
  }

  if (command == QLatin1String("PAIRED")) {
    m_handshakeTimer->stop();
    m_lastError.clear();
    setState(State::Paired);
    flushPending(); // 先把配对前排队的字节发出去，再通知上层
    Q_EMIT paired();
    return;
  }

  if (command == QLatin1String("PONG"))
    return; // 本类不主动 PING，收到就忽略

  if (command == QLatin1String("ERR")) {
    const QString message = relayErrorText(parts.value(1));
    reportError(message);
    // 服务端发完 ERR 就关连接，这里主动收尾，省得等 TCP 自己超时
    teardownSocket();
    scheduleReconnect(message);
    return;
  }

  // 未知报文：当成协议错误断开（中继在这个阶段只会回上面几种）
  const QString message = QStringLiteral("中继返回了无法识别的握手报文：%1").arg(QString::fromUtf8(line));
  reportError(message);
  teardownSocket();
  scheduleReconnect(message);
}

void RelayClient::onDisconnected()
{
  if (!m_socket)
    return;

  const bool wasLive = (m_state == State::Paired || m_state == State::WaitingForPeer);
  const QString reason = wasLive ? QStringLiteral("与中继的连接断开了（对端离线或网络中断），正在重连")
                                 : QStringLiteral("与中继的连接断开了，正在重连");
  teardownSocket();
  if (wasLive)
    Q_EMIT unpaired(reason);
  scheduleReconnect(m_lastError.isEmpty() ? reason : m_lastError);
}

void RelayClient::onSocketError()
{
  if (!m_socket)
    return;

  const QString message = QStringLiteral("中继连接失败：%1").arg(m_socket->errorString());
  reportError(message);

  // 连接从未建立时 Qt 不一定发 disconnected()，所以这里也安排重连（scheduleReconnect 幂等）
  if (m_state != State::Paired && m_state != State::WaitingForPeer) {
    teardownSocket();
    scheduleReconnect(message);
  }
}

void RelayClient::onHandshakeTimeout()
{
  if (m_state != State::Handshaking)
    return;
  const QString message = QStringLiteral("中继握手超时：检查地址/端口是否可达（云厂商安全组、ufw 都要放行）");
  reportError(message);
  teardownSocket();
  scheduleReconnect(message);
}

void RelayClient::onReconnectTimeout()
{
  if (!m_config.enabled)
    return;
  connectToRelay();
}

// ------------------------------------------------------------------ 收发

void RelayClient::send(const QByteArray &data)
{
  if (data.isEmpty())
    return;

  if (m_state != State::Paired || !m_socket) {
    // 还没配对（或正在重连）：先缓存。这一段数据对上层来说"已经发出去了"，
    // 真的丢掉会让上层协议卡死，所以宁可在上限内多留一会儿。
    if (m_pending.size() + data.size() > kMaxPendingBytes) {
      if (!m_pendingOverflow) {
        m_pendingOverflow = true;
        Q_EMIT errorOccurred(QStringLiteral("中继仍未配对，超过 %1 KiB 的待发数据被丢弃").arg(kMaxPendingBytes / 1024));
      }
      return;
    }
    m_pending.append(data);
    return;
  }

  m_socket->write(data);
}

void RelayClient::flushIncoming()
{
  if (m_rxBuffer.isEmpty())
    return;
  const QByteArray data = m_rxBuffer;
  m_rxBuffer.clear();
  Q_EMIT dataReceived(data);
}

void RelayClient::flushPending()
{
  if (m_pending.isEmpty() || !m_socket)
    return;
  const QByteArray data = m_pending;
  m_pending.clear();
  m_pendingOverflow = false;
  m_socket->write(data);
}

// ------------------------------------------------------------------ 内部

void RelayClient::teardownSocket()
{
  m_handshakeTimer->stop();
  if (!m_socket)
    return;

  // 先摘掉信号，避免 abort() 触发 onDisconnected() 又绕回来
  m_socket->disconnect(this);
  m_socket->abort();
  m_socket->deleteLater();
  m_socket = nullptr;
  m_rxBuffer.clear();
}

void RelayClient::closeChannel(const QString &reason)
{
  const bool wasLive = (m_state == State::Paired || m_state == State::WaitingForPeer);
  teardownSocket();
  if (wasLive)
    Q_EMIT unpaired(reason);
}

void RelayClient::scheduleReconnect(const QString &reason, bool retryable)
{
  if (!m_config.enabled) {
    setState(State::Disabled);
    return;
  }
  if (!retryable) {
    setState(State::Error);
    return;
  }
  setState(State::Error);
  if (m_reconnectTimer->isActive())
    return; // 已经安排过了（errorOccurred 和 disconnected 可能都来一遍）

  const int attempt = ++m_reconnectAttempts;
  const int delay = qMin(kReconnectMinMs << qMin(attempt - 1, 6), kReconnectMaxMs);
  qInfo().noquote() << QStringLiteral("relay: 第 %1 次重连将在 %2 ms 后开始（%3）")
                           .arg(attempt)
                           .arg(delay)
                           .arg(reason);
  m_reconnectTimer->start(delay);
}

void RelayClient::reportError(const QString &message)
{
  if (message == m_lastError)
    return; // 同一条错误只报一次，免得每次退避重试都弹一遍
  m_lastError = message;
  qWarning().noquote() << QStringLiteral("relay:") << message;
  Q_EMIT errorOccurred(message);
}

void RelayClient::setState(State state)
{
  if (state == m_state)
    return;
  m_state = state;
  Q_EMIT stateChanged(m_state);
}

} // namespace litekvm
