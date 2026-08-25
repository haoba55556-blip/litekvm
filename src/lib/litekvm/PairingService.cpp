// SPDX-License-Identifier: GPL-2.0
// LiteKVM pairing protocol over TCP/25901 — length-prefixed JSON frames.
//
// Spec: docs/discovery-pairing-spec.md §4-§7 (litekvm-design repo)
#include "PairingService.h"

namespace litekvm {

namespace {
constexpr int kFrameHeaderBytes = 2;      // big-endian length prefix
constexpr int kMaxFrameSize = 64 * 1024;
constexpr int kMaxWrongCodeAttempts = 5;
constexpr int kLockoutSeconds = 600;      // 10 minutes per spec §5
} // namespace

PairingService::PairingService(const DeviceIdentity &self, TrustStore &trust, QObject *parent)
  : QObject(parent), m_self(self), m_trust(trust)
{
}

bool PairingService::listen(quint16 port)
{
  connect(&m_server, &QTcpServer::newConnection, this, &PairingService::onNewConnection);
  if (!m_server.listen(QHostAddress::AnyIPv4, port)) {
    Q_EMIT errorOccurred(QStringLiteral("listen failed on port %1").arg(port));
    return false;
  }
  return true;
}

void PairingService::stop()
{
  m_server.close();
  for (auto *s : m_sessions)
    s->disconnectFromHost();
}

// ---------------------------------------------------------------- server side

void PairingService::onNewConnection()
{
  while (m_server.hasPendingConnections()) {
    QTcpSocket *sock = m_server.nextPendingConnection();
    if (!sock)
      continue;
    m_buffers[sock].clear();
    connect(sock, &QTcpSocket::readyRead, this, &PairingService::onReadyRead);
    connect(sock, &QTcpSocket::disconnected, this, &PairingService::onDisconnected);
    m_sessions.insert(sock);
  }
}

void PairingService::onDisconnected()
{
  auto *sock = qobject_cast<QTcpSocket *>(sender());
  if (!sock)
    return;
  m_buffers.remove(sock);
  m_nonces.remove(sock);
  m_sessions.remove(sock);
  sock->deleteLater();
}

QJsonObject PairingService::frameToObj(const QByteArray &payload, bool *ok)
{
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
  *ok = err.error == QJsonParseError::NoError && doc.isObject();
  return doc.object();
}

QByteArray PairingService::objToFrame(const QJsonObject &obj)
{
  const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  QByteArray frame;
  frame.append(char((payload.size() >> 8) & 0xFF));
  frame.append(char(payload.size() & 0xFF));
  frame.append(payload);
  return frame;
}

void PairingService::sendJson(QTcpSocket *sock, const QJsonObject &obj)
{
  sock->write(objToFrame(obj));
}

void PairingService::onReadyRead()
{
  auto *sock = qobject_cast<QTcpSocket *>(sender());
  if (!sock)
    return;

  QByteArray &buf = m_buffers[sock];
  buf.append(sock->readAll());

  while (true) {
    if (buf.size() < kFrameHeaderBytes)
      return;
    const int payloadLen = (quint8(buf[0]) << 8) | quint8(buf[1]);
    if (payloadLen > kMaxFrameSize) {
      sendError(sock, Error::ProtoMismatch);
      sock->disconnectFromHost();
      return;
    }
    if (buf.size() < kFrameHeaderBytes + payloadLen)
      return; // wait for more

    const QByteArray payload = buf.mid(kFrameHeaderBytes, payloadLen);
    buf.remove(0, kFrameHeaderBytes + payloadLen);

    bool ok = false;
    const QJsonObject obj = frameToObj(payload, &ok);
    if (!ok) {
      sendError(sock, Error::ProtoMismatch);
      sock->disconnectFromHost();
      return;
    }
    handleFrame(sock, obj);
  }
}

void PairingService::handleFrame(QTcpSocket *sock, const QJsonObject &msg)
{
  const QString type = msg.value("type").toString();

  // ---- rate limiting: lockout after N bad codes --------------------------
  if (m_lockUntil.isValid() && QDateTime::currentDateTimeUtc() < m_lockUntil) {
    sendError(sock, Error::RateLimited);
    sock->disconnectFromHost();
    return;
  }

  if (type == QLatin1String("PAIR_REQUEST")) {
    handlePairRequest(sock, msg);
  } else if (type == QLatin1String("PAIR_VERIFY")) {
    handlePairVerify(sock, msg);
  } else {
    sendError(sock, Error::ProtoMismatch);
    sock->disconnectFromHost();
  }
}

void PairingService::handlePairRequest(QTcpSocket *sock, const QJsonObject &msg)
{
  // one in-flight pairing at a time (spec §7 BUSY)
  if (m_activePeer && m_activePeer != sock) {
    sendError(sock, Error::Busy);
    sock->disconnectFromHost();
    return;
  }

  const QString proto = msg.value("proto").toString();
  if (proto != QLatin1String("v1")) {
    sendError(sock, Error::ProtoMismatch);
    sock->disconnectFromHost();
    return;
  }

  const QString nonceAHex = msg.value("nonce_a").toString();
  const QByteArray nonceA = QByteArray::fromHex(nonceAHex.toUtf8());
  if (nonceA.size() != 32) {
    sendError(sock, Error::ProtoMismatch);
    sock->disconnectFromHost();
    return;
  }

  // B generates its own nonce and derives the shared code
  const QByteArray nonceB = randomNonce(32);

  // fingerprints: ours + the requester's from the request (fp_a field)
  const QString fpA = msg.value("fp_a").toString();
  const QString code = derivePairCode(nonceA, nonceB, fpA, m_self.fingerprint());

  m_nonces[sock] = {nonceA, nonceB};
  m_codes[sock] = code;
  m_wrongAttempts[sock] = 0;
  m_activePeer = sock;

  QJsonObject reply{
      {"type", "PAIR_CHALLENGE"},
      {"nonce_b", QString::fromUtf8(nonceB.toHex())},
      {"device_id", m_self.deviceId()},
      {"name", m_self.name()},
      {"platform", platformString()},
      {"fp_b", m_self.fingerprint()},
      {"code", code}, // shown to the local user on B's screen by the UI layer
  };
  sendJson(sock, reply);
  Q_EMIT pairingStarted(fpA);
}

void PairingService::handlePairVerify(QTcpSocket *sock, const QJsonObject &msg)
{
  const auto nonceIt = m_nonces.find(sock);
  if (nonceIt == m_nonces.end()) {
    sendError(sock, Error::ProtoMismatch);
    sock->disconnectFromHost();
    return;
  }

  const QString input = msg.value("code_input").toString();
  if (input != m_codes.value(sock)) {
    const int attempts = ++m_wrongAttempts[sock];
    if (attempts >= kMaxWrongCodeAttempts) {
      m_lockUntil = QDateTime::currentDateTimeUtc().addSecs(kLockoutSeconds);
      Q_EMIT pairingFailed(Error::RateLimited);
      sendError(sock, Error::RateLimited);
      resetSession(sock);
      sock->disconnectFromHost();
      return;
    }
    Q_EMIT pairingFailed(Error::WrongCode);
    sendError(sock, Error::WrongCode);
    return;
  }

  // code correct — verify HMAC proof then exchange keys
  const QByteArray expectedMac =
      hmacPairVerify(m_codes.value(sock).toUtf8(), nonceIt->first, nonceIt->second);
  const QByteArray macA = QByteArray::fromHex(msg.value("hmac_a").toString().toUtf8());
  if (!verifyPairHmac(macA, m_codes.value(sock).toUtf8(), nonceIt->first, nonceIt->second)) {
    sendError(sock, Error::WrongCode);
    return;
  }

  // store the peer into trust before answering OK
  TrustEntry entry;
  entry.deviceId = msg.value("peer_device_id").toString();
  entry.pubkey = QByteArray::fromBase64(msg.value("pubkey").toString().toUtf8());
  entry.name = msg.value("peer_name").toString();
  entry.platform = msg.value("peer_platform").toString();
  entry.pairedAt = QDateTime::currentDateTimeUtc();
  entry.lastAddr = sock->peerAddress().toString();
  entry.lastSeen = entry.pairedAt;
  if (entry.deviceId.isEmpty() || entry.pubkey.isEmpty()) {
    sendError(sock, Error::ProtoMismatch);
    return;
  }
  m_trust.upsert(entry);

  const QByteArray pubB64 = QByteArray(reinterpret_cast<const char *>(m_self.publicKey().data()),
                                       qsizetype(m_self.publicKey().size()))
                                .toBase64();
  const QString hmacB =
      QString::fromUtf8(hmacPairVerify(codeFor(sock).toUtf8(), nonceIt->first, nonceIt->second).toHex());
  QJsonObject accept{
      {"type", "PAIR_ACCEPT"},
      {"hmac_b", hmacB},
      {"pubkey", QString::fromLatin1(pubB64)},
      {"device_id", m_self.deviceId()},
      {"name", m_self.name()},
      {"platform", platformString()},
  };
  sendJson(sock, accept);
  Q_EMIT pairingSucceeded(entry.deviceId, entry.name);
  resetSession(sock);
}

QString PairingService::codeFor(QTcpSocket *sock) const
{
  return m_codes.value(sock);
}

void PairingService::resetSession(QTcpSocket *sock)
{
  m_nonces.remove(sock);
  m_codes.remove(sock);
  m_wrongAttempts.remove(sock);
  if (m_activePeer == sock)
    m_activePeer = nullptr;
}

void PairingService::sendError(QTcpSocket *sock, Error code)
{
  QJsonObject err{{"type", "PAIR_ERROR"}, {"code", toString(code)}};
  sendJson(sock, err);
}

QString PairingService::toString(Error e)
{
  switch (e) {
  case Error::Timeout:
    return QStringLiteral("TIMEOUT");
  case Error::Rejected:
    return QStringLiteral("REJECTED");
  case Error::WrongCode:
    return QStringLiteral("WRONG_CODE");
  case Error::RateLimited:
    return QStringLiteral("RATE_LIMITED");
  case Error::ProtoMismatch:
    return QStringLiteral("PROTO_MISMATCH");
  case Error::Busy:
    return QStringLiteral("BUSY");
  }
  return QStringLiteral("UNKNOWN");
}

QString PairingService::platformString()
{
#if defined(Q_OS_WIN)
  return QStringLiteral("win");
#elif defined(Q_OS_MAC)
  return QStringLiteral("mac");
#else
  return QStringLiteral("linux");
#endif
}

// ---------------------------------------------------------------- client side

void PairingService::pairWith(const DiscoveredPeer &peer)
{
  auto *sock = new QTcpSocket(this);
  connect(sock, &QTcpSocket::connected, this, [this, sock] {
    // A → PAIR_REQUEST
    m_clientNonceA = randomNonce(32);
    QJsonObject req{
        {"type", "PAIR_REQUEST"},
        {"proto", "v1"},
        {"nonce_a", QString::fromUtf8(m_clientNonceA.toHex())},
        {"fp_a", m_self.fingerprint()},
        {"device_id", m_self.deviceId()},
    };
    sendJson(sock, req);
  });

  connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
    QByteArray &buf = m_buffers[sock];
    buf.append(sock->readAll());
    while (buf.size() >= kFrameHeaderBytes) {
      const int len = (quint8(buf[0]) << 8) | quint8(buf[1]);
      if (buf.size() < kFrameHeaderBytes + len)
        return;
      bool ok = false;
      const QJsonObject msg = frameToObj(buf.mid(kFrameHeaderBytes, len), &ok);
      buf.remove(0, kFrameHeaderBytes + len);
      if (!ok)
        continue;
      handleClientFrame(sock, msg);
    }
  });

  connect(sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
    Q_EMIT pairingFailed(Error::Timeout);
  });

  m_clientTarget = peer;
  sock->connectToHost(peer.host, peer.port);
}

void PairingService::submitPairCode(const QString &code)
{
  if (!m_clientSocket || m_clientNonces.first.isEmpty())
    return;
  const QByteArray macA = hmacPairVerify(code.toUtf8(), m_clientNonceA, m_clientNonces.second);
  QJsonObject verify{
      {"type", "PAIR_VERIFY"},
      {"code_input", code},
      {"hmac_a", QString::fromUtf8(macA.toHex())},
      {"pubkey",
       QString::fromLatin1(
           QByteArray(reinterpret_cast<const char *>(m_self.publicKey().data()),
                      qsizetype(m_self.publicKey().size()))
               .toBase64())},
      {"peer_device_id", m_self.deviceId()},
      {"peer_name", m_self.name()},
      {"peer_platform", platformString()},
  };
  sendJson(m_clientSocket, verify);
}

void PairingService::cancelPairing()
{
  if (m_clientSocket) {
    m_clientSocket->abort();
    m_clientSocket->deleteLater();
    m_clientSocket = nullptr;
  }
  m_clientNonces = {};
  Q_EMIT pairingFailed(Error::Rejected); // treat cancel as rejection locally
}

void PairingService::handleClientFrame(QTcpSocket *sock, const QJsonObject &msg)
{
  const QString type = msg.value("type").toString();

  if (type == QLatin1String("PAIR_CHALLENGE")) {
    const QByteArray nonceB = QByteArray::fromHex(msg.value("nonce_b").toString().toUtf8());
    const QString fpB = msg.value("fp_b").toString();
    m_clientNonces = {m_clientNonceA, nonceB};

    // both sides derive the same code; UI shows it to the B-side user,
    // A-side user types what they see on B's screen
    const QString code = derivePairCode(m_clientNonceA, nonceB, m_self.fingerprint(), fpB);
    m_clientCode = code;

    m_clientSocket = sock;
    Q_EMIT pairChallenge(msg.value("name").toString(), fpB, code);
    return;
  }

  if (type == QLatin1String("PAIR_ACCEPT")) {
    // verify B's hmac then persist B into our trust store
    const QByteArray macB = QByteArray::fromHex(msg.value("hmac_b").toString().toUtf8());
    if (!verifyPairHmac(macB, m_clientCode.toUtf8(), m_clientNonces.first, m_clientNonces.second)) {
      Q_EMIT pairingFailed(Error::WrongCode);
      return;
    }
    TrustEntry entry;
    entry.deviceId = msg.value("device_id").toString();
    entry.pubkey = QByteArray::fromBase64(msg.value("pubkey").toString().toUtf8());
    entry.name = msg.value("name").toString();
    entry.platform = msg.value("platform").toString();
    entry.pairedAt = QDateTime::currentDateTimeUtc();
    entry.lastAddr = m_clientTarget.host;
    entry.lastSeen = entry.pairedAt;
    m_trust.upsert(entry);

    Q_EMIT pairingSucceeded(entry.deviceId, entry.name);
    sock->disconnectFromHost();
    return;
  }

  if (type == QLatin1String("PAIR_ERROR")) {
    const QString codeStr = msg.value("code").toString();
    Error e = Error::ProtoMismatch;
    if (codeStr == QLatin1String("WRONG_CODE"))
      e = Error::WrongCode;
    else if (codeStr == QLatin1String("RATE_LIMITED"))
      e = Error::RateLimited;
    else if (codeStr == QLatin1String("BUSY"))
      e = Error::Busy;
    else if (codeStr == QLatin1String("REJECTED"))
      e = Error::Rejected;
    else if (codeStr == QLatin1String("TIMEOUT"))
      e = Error::Timeout;
    Q_EMIT pairingFailed(e);
  }
}

} // namespace litekvm
