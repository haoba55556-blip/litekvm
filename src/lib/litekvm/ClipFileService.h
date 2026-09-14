// SPDX-License-Identifier: GPL-2.0
// LiteKVM cross-machine file clipboard — TCP transport for ClipFileTransfer.
//
// Spec: docs/plans/2026-08-25-litekvm-mvp.md Phase 4 Task 4.1 / 4.2
//   ClipFileTransfer already moves chunked, sha1-checked files over *any*
//   QIODevice. This service gives it a socket: TCP/25903 (PairingService owns
//   25901, LayoutSync 25902), a paired-device handshake, signal forwarding and
//   disconnect/timeout cleanup.
//
//   server: ClipFileService server(identity, trust);
//           server.setIncomingDirectory(dir);
//           server.listen();                       // 25903
//   client: ClipFileService client(identity, trust);
//           client.sendFilesTo(host, 25903, paths);
//
// One connection carries exactly one ClipFileTransfer. When that transfer
// reaches a terminal state (completed / failed / cancelled) the session — and
// the transfer object with it — is torn down; the next sendFilesTo() opens a
// fresh connection. A listening service refuses further connections while a
// session is live (plan Task 4.1: one transfer at a time).
//
// ---------------------------------------------------------------------------
// !!! SECURITY STATUS — READ BEFORE TRUSTING THIS CHANNEL !!!
//
// The handshake proves only that the peer *claims* a device_id this machine has
// paired with before (a TrustStore lookup). It is NOT cryptographic
// authentication and the TCP stream is NOT encrypted:
//   * device_id is derived from the peer's public key, so it is public
//     information — anyone who learns it can impersonate that device.
//   * no nonce or signature is exchanged, so there is no replay or MITM
//     protection: this handshake is a filter, not a lock.
// This is a deliberate, temporary shortcut. The MVP ships file transfers over
// the deskflow data channel, which is already encrypted and authenticated;
// ClipFileService exists so the transfer logic can run (and be tested) on a
// standalone socket. Before promoting it to the real transport, either run it
// on top of that encrypted channel or extend PairingCrypto with an Ed25519
// sign/verify over a fresh nonce (the trusted pubkey is already in TrustStore).
// See ClipFileService::handleHandshakeMessage().
// ---------------------------------------------------------------------------
#pragma once

#include "ClipFileTransfer.h"
#include "DeviceIdentity.h"
#include "TrustStore.h"

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

#include <optional>

class QTimer;

namespace litekvm {

/**
 * @brief Control-plane service that hands a verified TCP socket to
 * ClipFileTransfer (TCP/25903).
 *
 * Wire framing for the handshake is exactly PairingService's
 * (`[len_hi len_lo 2B BE][UTF-8 JSON]`, 64 KiB cap) with a distinct message
 * family so it can never be mistaken for a clip frame:
 *
 *   CALP_HELLO         device_id, proto, name, platform   (client -> server)
 *   CALP_HELLO_OK      device_id, proto, name, platform   (server -> client)
 *   CALP_HELLO_REJECT  code = NOT_PAIRED | BUSY | PROTO... (either side, then close)
 *
 * One round trip, roles explicit: the initiator greets, the listener answers.
 * Both sides validate the peer's device_id against the TrustStore — the client
 * from CALP_HELLO_OK, the server from CALP_HELLO. The *server* considers the
 * handshake finished the moment it sends CALP_HELLO_OK, i.e. before the peer
 * can have sent anything else, so a conforming peer can never queue clip frames
 * behind its handshake frame; any leftover byte is treated as a protocol error.
 * Only then is the socket handed over — the service stops reading it so
 * ClipFileTransfer is the sole consumer of its bytes.
 */
class ClipFileService : public QObject {
  Q_OBJECT

public:
  enum class Error {
    None,
    Listen,           ///< listen() failed
    Busy,             ///< session in flight, or client used while listening
    NotPaired,        ///< peer's device_id is not in the trust store (or peer rejected us)
    HandshakeTimeout, ///< no CALP_HELLO within handshakeTimeout()
    Protocol,         ///< malformed frame / unexpected message
    Socket,           ///< connect failed or the socket died mid-handshake
  };
  Q_ENUM(Error)
  static QString toString(Error error);

  enum class Role { None, Server, Client };
  Q_ENUM(Role)
  static QString toString(Role role);

  ClipFileService(const DeviceIdentity &self, TrustStore &trust, QObject *parent = nullptr);
  ~ClipFileService() override;

  /// Standalone file-transfer control port (PairingService 25901, LayoutSync 25902).
  static constexpr quint16 defaultPort()
  {
    return ClipFileTransfer::defaultPort();
  }
  static QString protocolVersion();

  // ------------------------------------------------------------- wire (tests)
  /// `[len_hi len_lo][JSON]`, empty when the payload exceeds 64 KiB.
  static QByteArray encodeFrame(const QJsonObject &message);
  static std::optional<QJsonObject> decodeFrame(const QByteArray &payload);

  // --------------------------------------------------------------- server
  /// Listens on @p port (any IPv4 interface). False + errorOccurred() when the
  /// port is taken or a session is already live.
  bool listen(quint16 port = defaultPort());
  /// Stops listening and drops the current session (cancels its transfer, which
  /// removes the half-written ".litekvm-part" file).
  void stop();
  bool isListening() const;
  quint16 serverPort() const;

  // --------------------------------------------------------------- client
  /// Connects to @p host:@p port, completes the paired-device handshake and
  /// sends @p paths. Every failure surfaces as errorOccurred().
  void sendFilesTo(const QString &host, quint16 port, const QStringList &paths);

  // ------------------------------------------------- receiver-side actions
  /// Answer the current CLIP_OFFER; a no-op when no transfer is live. The
  /// underlying ClipFileTransfer reports an accept/reject in the wrong state
  /// as a protocol failure (which also tears the session down).
  void acceptOffer(qint64 resumeFrom = 0);
  void rejectOffer(const QString &reason = QStringLiteral("USER"));
  /// Cancel the current transfer, if any.
  void cancel();

  // --------------------------------------------------------------- settings
  // Applied to every ClipFileTransfer this service creates.
  void setIncomingDirectory(const QString &dir);
  QString incomingDirectory() const;
  void setAutoAccept(bool on);
  bool autoAccept() const;
  void setChunkSize(int bytes);
  int chunkSize() const;
  void setMaxTransferBytes(qint64 bytes);
  qint64 maxTransferBytes() const;
  /// Connect + CALP_HELLO deadline. 0 restores the default (10 s).
  void setHandshakeTimeout(int ms);
  int handshakeTimeout() const;

  // ----------------------------------------------------------------- status
  Role role() const;
  /// True once the handshake finished (peer identity accepted).
  bool hasPeer() const;
  QString peerDeviceId() const;
  QString peerName() const;
  /// True while a ClipFileTransfer is running.
  bool isBusy() const;
  /// The live transfer, or nullptr between sessions (it is destroyed with the
  /// session, so read state from the forwarded signals once it ends).
  ClipFileTransfer *transfer() const;

Q_SIGNALS:
  void peerConnected(const QString &deviceId);
  void peerDisconnected();
  void errorOccurred(const QString &message);

  // --- ClipFileTransfer signals, forwarded unchanged
  void offerReceived(const litekvm::ClipFileOffer &offer);
  void progressChanged(const litekvm::TransferProgress &progress);
  void entryCompleted(int entryIndex, const QString &localPath);
  void transferCompleted(const QStringList &localPaths, const QString &transferId);
  void transferCancelled(const QString &transferId);
  void transferFailed(litekvm::ClipFileTransfer::Error error, const QString &detail);

private Q_SLOTS:
  void onNewConnection();
  void onSocketReadyRead();
  void onSocketDisconnected();
  void onHandshakeTimeout();

private:
  void attachSocket(QTcpSocket *socket, Role role);
  void sendHello(QTcpSocket *socket);
  void handleHandshakeMessage(QTcpSocket *socket, const QJsonObject &msg);
  void completeHandshake();
  void notePeerSeen(const QString &deviceId, const QHostAddress &address);
  void rejectHandshake(QTcpSocket *socket, Error error, const char *code,
                       const QString &claimedDeviceId = QString());
  void refuseBusy(QTcpSocket *socket);
  void startTransfer();
  void forwardTransferSignals(ClipFileTransfer *transfer);
  void closeSession();
  void reportError(Error error, const QString &detail);
  void sendJson(QTcpSocket *socket, const QJsonObject &message);

  const DeviceIdentity &m_self;
  TrustStore &m_trust;

  QTcpServer m_server;
  QTcpSocket *m_socket = nullptr;
  ClipFileTransfer *m_transfer = nullptr;
  QTimer *m_handshakeTimer = nullptr;

  Role m_role = Role::None;
  QByteArray m_buffer;
  bool m_helloDone = false;      ///< peer identity accepted; socket handed over
  bool m_closing = false;        ///< re-entrancy guard for closeSession()
  QString m_peerDeviceId;
  QString m_peerName;
  QStringList m_pendingPaths;    ///< client: staged by sendFilesTo()

  QString m_incomingDir;
  bool m_autoAccept = false;
  int m_chunkSize = ClipFileTransfer::defaultChunkSize();
  qint64 m_maxTransferBytes = 0; ///< 0 keeps the ClipFileTransfer default
  int m_handshakeTimeoutMs = 10'000;
};

} // namespace litekvm
