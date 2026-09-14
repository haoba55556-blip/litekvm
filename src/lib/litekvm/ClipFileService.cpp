// SPDX-License-Identifier: GPL-2.0
// LiteKVM cross-machine file clipboard — TCP transport for ClipFileTransfer.
//
// Spec: docs/plans/2026-08-25-litekvm-mvp.md Phase 4 Task 4.1 / 4.2
// See ClipFileService.h for the wire format and — importantly — the security
// status of the handshake (device_id + TrustStore only, channel not encrypted).
#include "ClipFileService.h"

#include "PairingService.h" // PairingService::platformString()

#include <QDateTime>
#include <QDir>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>

namespace litekvm {

namespace {

// Handshake message family ("CALP" = clipboard application layer protocol).
// Deliberately distinct from ClipFileTransfer's CLIP_* frames: the two use
// different framings and must never be confused.
constexpr char kHelloType[] = "CALP_HELLO";
constexpr char kHelloOkType[] = "CALP_HELLO_OK";
constexpr char kHelloRejectType[] = "CALP_HELLO_REJECT";

constexpr char kCodeNotPaired[] = "NOT_PAIRED";
constexpr char kCodeBusy[] = "BUSY";
constexpr char kCodeProto[] = "PROTO";

constexpr int kFrameHeaderBytes = 2; // big-endian length prefix, as PairingService
constexpr int kMaxFrameSize = 64 * 1024;
constexpr int kDefaultHandshakeTimeoutMs = 10'000;
/// A refused connection keeps its socket open briefly so the peer can read the
/// reject frame before the close (closing with unread data pending would turn
/// the FIN into a RST and the peer would lose the reason).
constexpr int kBusyCloseDelayMs = 500;

QString str(const char *ascii)
{
  return QString::fromLatin1(ascii);
}

bool isType(const QJsonObject &msg, const char *asciiType)
{
  return msg.value(QStringLiteral("type")).toString() == QLatin1String(asciiType);
}

QJsonObject helloMessage(const DeviceIdentity &self)
{
  return QJsonObject{
      {QStringLiteral("type"), str(kHelloType)},
      {QStringLiteral("proto"), ClipFileService::protocolVersion()},
      {QStringLiteral("device_id"), self.deviceId()},
      {QStringLiteral("name"), self.name()},
      {QStringLiteral("platform"), PairingService::platformString()},
  };
}

} // namespace

QString ClipFileService::protocolVersion()
{
  return QStringLiteral("v1");
}

QString ClipFileService::toString(Error error)
{
  switch (error) {
  case Error::None:
    return QStringLiteral("NONE");
  case Error::Listen:
    return QStringLiteral("LISTEN");
  case Error::Busy:
    return QStringLiteral("BUSY");
  case Error::NotPaired:
    return QStringLiteral("NOT_PAIRED");
  case Error::HandshakeTimeout:
    return QStringLiteral("HANDSHAKE_TIMEOUT");
  case Error::Protocol:
    return QStringLiteral("PROTO");
  case Error::Socket:
    return QStringLiteral("SOCKET");
  }
  return QStringLiteral("UNKNOWN");
}

QString ClipFileService::toString(Role role)
{
  switch (role) {
  case Role::None:
    return QStringLiteral("NONE");
  case Role::Server:
    return QStringLiteral("SERVER");
  case Role::Client:
    return QStringLiteral("CLIENT");
  }
  return QStringLiteral("UNKNOWN");
}

QByteArray ClipFileService::encodeFrame(const QJsonObject &message)
{
  const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
  if (payload.isEmpty() || payload.size() > kMaxFrameSize)
    return {};
  QByteArray frame;
  frame.append(char((payload.size() >> 8) & 0xFF));
  frame.append(char(payload.size() & 0xFF));
  frame.append(payload);
  return frame;
}

std::optional<QJsonObject> ClipFileService::decodeFrame(const QByteArray &payload)
{
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return std::nullopt;
  return doc.object();
}

// -------------------------------------------------------------- lifecycle

ClipFileService::ClipFileService(const DeviceIdentity &self, TrustStore &trust, QObject *parent)
  : QObject(parent), m_self(self), m_trust(trust)
{
  connect(&m_server, &QTcpServer::newConnection, this, &ClipFileService::onNewConnection);

  m_handshakeTimer = new QTimer(this);
  m_handshakeTimer->setSingleShot(true);
  connect(m_handshakeTimer, &QTimer::timeout, this, &ClipFileService::onHandshakeTimeout);
}

ClipFileService::~ClipFileService()
{
  // Also cancels a live transfer, which removes its ".litekvm-part" file.
  closeSession();
  m_server.close();
}

bool ClipFileService::listen(quint16 port)
{
  if (m_socket) {
    reportError(Error::Busy, QStringLiteral("a session is already live"));
    return false;
  }
  if (m_server.isListening()) {
    reportError(Error::Busy, QStringLiteral("already listening on port %1").arg(serverPort()));
    return false;
  }
  if (!m_server.listen(QHostAddress::AnyIPv4, port)) {
    reportError(Error::Listen,
                QStringLiteral("listen failed on port %1: %2").arg(port).arg(m_server.errorString()));
    return false;
  }
  m_role = Role::Server;
  return true;
}

void ClipFileService::stop()
{
  closeSession();
  m_server.close();
  m_role = Role::None;
}

bool ClipFileService::isListening() const
{
  return m_server.isListening();
}

quint16 ClipFileService::serverPort() const
{
  return m_server.serverPort();
}

void ClipFileService::sendFilesTo(const QString &host, quint16 port, const QStringList &paths)
{
  if (paths.isEmpty()) {
    reportError(Error::Protocol, QStringLiteral("sendFilesTo(): no paths given"));
    return;
  }
  if (m_socket || m_transfer) {
    reportError(Error::Busy, QStringLiteral("a session is already live (one transfer at a time)"));
    return;
  }
  if (m_server.isListening()) {
    reportError(Error::Busy, QStringLiteral("this service is listening; use a client instance to send"));
    return;
  }

  m_pendingPaths = paths;

  auto *socket = new QTcpSocket(this);
  attachSocket(socket, Role::Client);

  connect(socket, &QTcpSocket::connected, this, [this, socket] {
    if (socket != m_socket)
      return;
    sendHello(socket);
  });
  // A refused/unreachable peer never reaches connectToHost() success, and Qt
  // emits no disconnected() in that case — report it here.
  connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
    if (socket != m_socket || m_helloDone)
      return;
    reportError(Error::Socket, QStringLiteral("connect to peer failed: %1").arg(socket->errorString()));
    closeSession();
  });

  socket->connectToHost(host, port);
}

// ------------------------------------------------------------- settings

void ClipFileService::setIncomingDirectory(const QString &dir)
{
  m_incomingDir = QDir::cleanPath(dir);
}

QString ClipFileService::incomingDirectory() const
{
  return m_incomingDir;
}

void ClipFileService::setAutoAccept(bool on)
{
  m_autoAccept = on;
}

bool ClipFileService::autoAccept() const
{
  return m_autoAccept;
}

void ClipFileService::setChunkSize(int bytes)
{
  m_chunkSize = bytes;
}

int ClipFileService::chunkSize() const
{
  return m_chunkSize;
}

void ClipFileService::setMaxTransferBytes(qint64 bytes)
{
  m_maxTransferBytes = bytes > 0 ? bytes : 0;
}

qint64 ClipFileService::maxTransferBytes() const
{
  return m_maxTransferBytes;
}

void ClipFileService::setHandshakeTimeout(int ms)
{
  m_handshakeTimeoutMs = ms > 0 ? ms : kDefaultHandshakeTimeoutMs;
}

int ClipFileService::handshakeTimeout() const
{
  return m_handshakeTimeoutMs;
}

// ----------------------------------------------------------------- status

ClipFileService::Role ClipFileService::role() const
{
  return m_role;
}

bool ClipFileService::hasPeer() const
{
  return m_helloDone;
}

QString ClipFileService::peerDeviceId() const
{
  return m_peerDeviceId;
}

QString ClipFileService::peerName() const
{
  return m_peerName;
}

bool ClipFileService::isBusy() const
{
  return m_transfer && m_transfer->isActive();
}

ClipFileTransfer *ClipFileService::transfer() const
{
  return m_transfer;
}

// ------------------------------------------------------------- receiver side

void ClipFileService::acceptOffer(qint64 resumeFrom)
{
  if (m_transfer)
    m_transfer->acceptOffer(resumeFrom);
}

void ClipFileService::rejectOffer(const QString &reason)
{
  if (m_transfer)
    m_transfer->rejectOffer(reason);
}

void ClipFileService::cancel()
{
  if (m_transfer)
    m_transfer->cancel();
}

// ---------------------------------------------------------------- server side

void ClipFileService::onNewConnection()
{
  while (m_server.hasPendingConnections()) {
    QTcpSocket *socket = m_server.nextPendingConnection();
    if (!socket)
      continue;
    if (m_socket) {
      refuseBusy(socket);
      continue;
    }
    attachSocket(socket, Role::Server);
    // Symmetric verification, asymmetric traffic: the initiator greets and the
    // listener answers with CALP_HELLO_OK.
  }
}

void ClipFileService::refuseBusy(QTcpSocket *socket)
{
  connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->readAll(); }); // drain -> clean FIN
  connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
  sendJson(socket,
           QJsonObject{{QStringLiteral("type"), str(kHelloRejectType)},
                       {QStringLiteral("code"), str(kCodeBusy)},
                       {QStringLiteral("proto"), protocolVersion()}});
  QTimer::singleShot(kBusyCloseDelayMs, socket, [socket] {
    if (socket->state() != QAbstractSocket::UnconnectedState)
      socket->disconnectFromHost();
  });
}

void ClipFileService::onHandshakeTimeout()
{
  if (m_helloDone || !m_socket)
    return;
  if (m_socket->state() == QAbstractSocket::ConnectedState) {
    reportError(Error::HandshakeTimeout,
                QStringLiteral("no CALP_HELLO from %1 within %2 ms").arg(m_socket->peerAddress().toString()).arg(m_handshakeTimeoutMs));
  } else {
    reportError(Error::Socket, QStringLiteral("peer did not connect within %1 ms").arg(m_handshakeTimeoutMs));
  }
  closeSession();
}

void ClipFileService::onSocketDisconnected()
{
  auto *socket = qobject_cast<QTcpSocket *>(sender());
  if (!socket || socket != m_socket) {
    if (socket)
      socket->deleteLater();
    return;
  }
  closeSession();
}

// ---------------------------------------------------------------- handshake

void ClipFileService::onSocketReadyRead()
{
  auto *socket = qobject_cast<QTcpSocket *>(sender());
  if (!socket || socket != m_socket)
    return;

  m_buffer.append(socket->readAll());

  // The handshake is one request and one answer, so at most a couple of tiny
  // frames are ever parsed here. Once completeHandshake() has run, the socket
  // belongs to ClipFileTransfer and this loop must not touch it again.
  while (socket == m_socket && !m_helloDone) {
    if (m_buffer.size() < kFrameHeaderBytes)
      return; // wait for the length prefix

    const int payloadLen = (quint8(m_buffer.at(0)) << 8) | quint8(m_buffer.at(1));
    if (payloadLen < 1 || payloadLen > kMaxFrameSize) {
      reportError(Error::Protocol, QStringLiteral("bad handshake frame length %1").arg(payloadLen));
      closeSession();
      return;
    }
    if (m_buffer.size() < kFrameHeaderBytes + payloadLen)
      return; // wait for the rest of the frame

    const QByteArray payload = m_buffer.mid(kFrameHeaderBytes, payloadLen);
    m_buffer.remove(0, kFrameHeaderBytes + payloadLen);

    const auto msg = decodeFrame(payload);
    if (!msg) {
      reportError(Error::Protocol, QStringLiteral("handshake frame is not a JSON object"));
      closeSession();
      return;
    }

    handleHandshakeMessage(socket, *msg);
  }
}

void ClipFileService::attachSocket(QTcpSocket *socket, Role role)
{
  m_socket = socket;
  m_role = role;
  m_buffer.clear();
  m_helloDone = false;
  m_peerDeviceId.clear();
  m_peerName.clear();

  socket->setParent(this);
  connect(socket, &QTcpSocket::readyRead, this, &ClipFileService::onSocketReadyRead);
  connect(socket, &QTcpSocket::disconnected, this, &ClipFileService::onSocketDisconnected);

  // Covers both "connect to the peer" and "peer answered CALP_HELLO".
  m_handshakeTimer->start(m_handshakeTimeoutMs);
}

void ClipFileService::sendHello(QTcpSocket *socket)
{
  sendJson(socket, helloMessage(m_self));
}

void ClipFileService::sendJson(QTcpSocket *socket, const QJsonObject &message)
{
  if (!socket || socket->state() != QAbstractSocket::ConnectedState)
    return;
  const QByteArray frame = encodeFrame(message);
  if (!frame.isEmpty())
    socket->write(frame);
}

void ClipFileService::handleHandshakeMessage(QTcpSocket *socket, const QJsonObject &msg)
{
  if (msg.value(QStringLiteral("type")).toString().isEmpty()) {
    reportError(Error::Protocol, QStringLiteral("handshake frame without a type"));
    closeSession();
    return;
  }

  if (isType(msg, kHelloRejectType)) {
    const QString code = msg.value(QStringLiteral("code")).toString();
    Error error = Error::Protocol;
    if (code == QLatin1String(kCodeNotPaired))
      error = Error::NotPaired;
    else if (code == QLatin1String(kCodeBusy))
      error = Error::Busy;
    reportError(error, QStringLiteral("handshake rejected by peer: %1").arg(code));
    closeSession();
    return;
  }

  const QString proto = msg.value(QStringLiteral("proto")).toString();
  if (proto != protocolVersion()) {
    rejectHandshake(socket, Error::Protocol, kCodeProto);
    return;
  }

  // The listener answers CALP_HELLO with CALP_HELLO_OK and is done the moment
  // the answer is on the wire; the initiator greets and is done when that
  // answer arrives. Either way the socket changes owner inside the callback
  // that handled the peer's *last* handshake frame, so a conforming peer cannot
  // have a clip frame in flight yet (see the header comment).
  if (m_role == Role::Server) {
    if (!isType(msg, kHelloType)) {
      reportError(Error::Protocol, QStringLiteral("expected CALP_HELLO from the connecting peer"));
      closeSession();
      return;
    }

    // ---- TEMPORARY identity check (see the security note in the header) ----
    // device_id is public information derived from the peer's key, so a
    // matching trust-store entry only proves the peer *claims* to be a paired
    // device. The real fix is an Ed25519 signature over a fresh nonce, verified
    // against TrustEntry::pubkey, plus an encrypted channel — neither of which
    // exists in this MVP.
    const QString deviceId = msg.value(QStringLiteral("device_id")).toString();
    if (deviceId.isEmpty() || !m_trust.contains(deviceId)) {
      rejectHandshake(socket, Error::NotPaired, kCodeNotPaired, deviceId);
      return;
    }

    m_peerDeviceId = deviceId;
    m_peerName = msg.value(QStringLiteral("name")).toString();
    notePeerSeen(deviceId, socket->peerAddress());

    sendJson(socket, QJsonObject{{QStringLiteral("type"), str(kHelloOkType)},
                                 {QStringLiteral("proto"), protocolVersion()},
                                 {QStringLiteral("device_id"), m_self.deviceId()},
                                 {QStringLiteral("name"), m_self.name()},
                                 {QStringLiteral("platform"), PairingService::platformString()}});
    completeHandshake();
    return;
  }

  // client role: the peer must have accepted *our* CALP_HELLO
  if (!isType(msg, kHelloOkType)) {
    reportError(Error::Protocol, QStringLiteral("expected CALP_HELLO_OK from the listening peer"));
    closeSession();
    return;
  }

  const QString deviceId = msg.value(QStringLiteral("device_id")).toString();
  if (deviceId.isEmpty() || !m_trust.contains(deviceId)) {
    rejectHandshake(socket, Error::NotPaired, kCodeNotPaired, deviceId);
    return;
  }

  m_peerDeviceId = deviceId;
  m_peerName = msg.value(QStringLiteral("name")).toString();
  notePeerSeen(deviceId, socket->peerAddress());
  completeHandshake();
}

void ClipFileService::completeHandshake()
{
  if (m_helloDone || !m_socket)
    return;

  // From here the socket belongs to ClipFileTransfer, which reads it directly.
  // A byte that is already buffered here cannot be handed over, so treat it as
  // the protocol violation it is instead of silently dropping it.
  if (!m_buffer.isEmpty()) {
    reportError(Error::Protocol, QStringLiteral("peer sent clip data before the handshake finished"));
    closeSession();
    return;
  }

  QTcpSocket *socket = m_socket;
  m_handshakeTimer->stop();
  disconnect(socket, &QTcpSocket::readyRead, this, &ClipFileService::onSocketReadyRead);
  m_helloDone = true;

  Q_EMIT peerConnected(m_peerDeviceId);
  startTransfer();
}

void ClipFileService::notePeerSeen(const QString &deviceId, const QHostAddress &address)
{
  // Keep the trusted entry's informational fields fresh (spec §6: reconnect
  // trusts device_id, the address is a hint only).
  if (const TrustEntry *entry = m_trust.find(deviceId)) {
    TrustEntry updated = *entry;
    updated.lastSeen = QDateTime::currentDateTimeUtc();
    updated.lastAddr = address.toString();
    m_trust.upsert(updated);
  }
}

void ClipFileService::rejectHandshake(QTcpSocket *socket, Error error, const char *code,
                                     const QString &claimedDeviceId)
{
  sendJson(socket,
           QJsonObject{{QStringLiteral("type"), str(kHelloRejectType)},
                       {QStringLiteral("code"), str(code)},
                       {QStringLiteral("proto"), protocolVersion()}});
  if (error == Error::NotPaired) {
    reportError(error,
                QStringLiteral("peer %1 is not a paired device (%2; temporary device_id-only check, channel not authenticated)")
                    .arg(claimedDeviceId.isEmpty() ? QStringLiteral("<no device_id>") : claimedDeviceId.left(8),
                         str(code)));
  } else {
    reportError(error, QStringLiteral("handshake refused (%1)").arg(str(code)));
  }

  // Stop parsing, then disconnectFromHost() drains the reject frame before the
  // socket closes (abort() would drop it, leaving the peer with a bare RST).
  closeSession();
}

// ---------------------------------------------------------------- transfer

void ClipFileService::startTransfer()
{
  if (!m_socket)
    return;

  auto *transfer = new ClipFileTransfer(m_socket, this);
  m_transfer = transfer;
  transfer->setIncomingDirectory(m_incomingDir.isEmpty() ? ClipFileTransfer::defaultIncomingDirectory()
                                                         : m_incomingDir);
  transfer->setAutoAccept(m_autoAccept);
  if (m_chunkSize > 0)
    transfer->setChunkSize(m_chunkSize);
  if (m_maxTransferBytes > 0)
    transfer->setMaxTransferBytes(m_maxTransferBytes);

  forwardTransferSignals(transfer);

  if (m_role == Role::Client && !m_pendingPaths.isEmpty()) {
    const QStringList paths = m_pendingPaths;
    m_pendingPaths.clear();
    transfer->sendLocalPaths(paths);
  }
}

void ClipFileService::forwardTransferSignals(ClipFileTransfer *transfer)
{
  connect(transfer, &ClipFileTransfer::offerReceived, this, &ClipFileService::offerReceived);
  connect(transfer, &ClipFileTransfer::progressChanged, this, &ClipFileService::progressChanged);
  connect(transfer, &ClipFileTransfer::entryCompleted, this, &ClipFileService::entryCompleted);

  // A session carries exactly one transfer: every terminal state tears the
  // session down (which cancels the transfer and removes its ".litekvm-part").
  connect(transfer, &ClipFileTransfer::transferCompleted, this,
          [this](const QStringList &localPaths, const QString &transferId) {
            Q_EMIT transferCompleted(localPaths, transferId);
            closeSession();
          });
  connect(transfer, &ClipFileTransfer::transferCancelled, this, [this](const QString &transferId) {
    Q_EMIT transferCancelled(transferId);
    closeSession();
  });
  connect(transfer, &ClipFileTransfer::transferFailed, this,
          [this](ClipFileTransfer::Error error, const QString &detail) {
            // Already reported through transferFailed(); no extra errorOccurred().
            Q_EMIT transferFailed(error, detail);
            closeSession();
          });
}

// ----------------------------------------------------------------- cleanup

void ClipFileService::closeSession()
{
  if (m_closing)
    return; // cancel() below re-enters through transferCancelled()
  m_closing = true;

  const bool hadPeer = m_helloDone;

  // Drop the state first: any signal emitted from here on must not re-enter.
  QTcpSocket *socket = m_socket;
  ClipFileTransfer *transfer = m_transfer;
  m_socket = nullptr;
  m_transfer = nullptr;
  m_handshakeTimer->stop();
  m_buffer.clear();
  m_helloDone = false;
  m_pendingPaths.clear();
  m_peerDeviceId.clear();
  m_peerName.clear();

  if (transfer) {
    // cancel() tells the peer and removes the half-written ".litekvm-part".
    // deleteLater(), never delete: cancel() may be emitting right now.
    transfer->cancel();
    transfer->deleteLater();
  }

  if (socket) {
    // Flush before dropping the socket: a terminal control frame
    // (CALP_HELLO_REJECT / CLIP_DONE / ...) just written sits in Qt's
    // internal buffer, and deleteLater() below would abort() and drop it.
    socket->flush();
    socket->disconnect(this); // stop our own handlers before dropping it
    if (socket->state() != QAbstractSocket::UnconnectedState)
      socket->disconnectFromHost(); // drains pending frames before closing
    socket->deleteLater();
  }

  m_closing = false;

  if (hadPeer)
    Q_EMIT peerDisconnected();
}

void ClipFileService::reportError(Error error, const QString &detail)
{
  Q_EMIT errorOccurred(QStringLiteral("%1: %2").arg(toString(error), detail));
}

} // namespace litekvm
