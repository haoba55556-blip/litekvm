// SPDX-License-Identifier: GPL-2.0
// LiteKVM pairing protocol over TCP/25901 — length-prefixed JSON frames.
//
// Both roles live here: server (displays PIN, accepts) and client (initiator,
// user types the PIN shown on the server's screen).
//
// Spec: docs/discovery-pairing-spec.md §4-§7 (litekvm-design repo)
#pragma once

#include "DeviceIdentity.h"
#include "DiscoveryService.h"
#include "PairingCrypto.h"
#include "TrustStore.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

#include <utility>

namespace litekvm {

class PairingService : public QObject {
  Q_OBJECT

public:
  enum class Error { Timeout, Rejected, WrongCode, RateLimited, ProtoMismatch, Busy };
  static QString toString(Error e);

  PairingService(const DeviceIdentity &self, TrustStore &trust, QObject *parent = nullptr);

  /// server role
  bool listen(quint16 port = 25901);
  void stop();

  /// client role
  void pairWith(const DiscoveredPeer &peer);
  void submitPairCode(const QString &code);
  void cancelPairing();

  static QString platformString();

Q_SIGNALS:
  // server side
  void pairingStarted(const QString &peerFingerprint);
  // client side: challenge received, code derived locally; UI asks the user
  // to type the code displayed on the remote screen
  void pairChallenge(const QString &peerName, const QString &peerFingerprint, const QString &expectedCode);
  void pairingSucceeded(const QString &deviceId, const QString &name);
  void pairingFailed(Error error);
  void errorOccurred(const QString &message);

private Q_SLOTS:
  void onNewConnection();
  void onDisconnected();
  void onReadyRead();

private:
  struct NoncePair {
    QByteArray first;
    QByteArray second;
  };

  void handleFrame(QTcpSocket *sock, const QJsonObject &msg);
  void handlePairRequest(QTcpSocket *sock, const QJsonObject &msg);
  void handlePairVerify(QTcpSocket *sock, const QJsonObject &msg);
  void handleClientFrame(QTcpSocket *sock, const QJsonObject &msg);

  static QJsonObject frameToObj(const QByteArray &payload, bool *ok);
  static QByteArray objToFrame(const QJsonObject &obj);
  void sendJson(QTcpSocket *sock, const QJsonObject &obj);
  void sendError(QTcpSocket *sock, Error code);
  QString codeFor(QTcpSocket *sock) const;
  void resetSession(QTcpSocket *sock);

  const DeviceIdentity &m_self;
  TrustStore &m_trust;

  QTcpServer m_server;
  QSet<QTcpSocket *> m_sessions;
  QMap<QTcpSocket *, QByteArray> m_buffers;
  QMap<QTcpSocket *, NoncePair> m_nonces;
  QMap<QTcpSocket *, QString> m_codes;
  QMap<QTcpSocket *, int> m_wrongAttempts;
  QTcpSocket *m_activePeer = nullptr;
  QDateTime m_lockUntil;

  // client state
  DiscoveredPeer m_clientTarget;
  QTcpSocket *m_clientSocket = nullptr;
  QByteArray m_clientNonceA;
  NoncePair m_clientNonces;
  QString m_clientCode;
};

} // namespace litekvm
