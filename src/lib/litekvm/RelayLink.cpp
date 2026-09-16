// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// RelayLink — client-side connector to a self-hosted LiteKVM relay.
//
// Enables cross-network pairing: both peers connect out to the relay with a
// shared room token; the relay splices the TCP streams. All data remains
// end-to-end encrypted by the deskflow TLS layer (relay sees ciphertext only).
//
// Part of litekvm-pro feature 1 (different-network connectivity).
#include "RelayLink.h"

#include <QTimer>

namespace litekvm {

namespace {
constexpr int kRegisterTimeoutMs = 10'000;
} // namespace

RelayLink::RelayLink(const DeviceIdentity &self, QObject *parent)
  : QObject(parent), m_self(self)
{
  connect(&m_socket, &QTcpSocket::connected, this, &RelayLink::onConnected);
  connect(&m_socket, &QTcpSocket::readyRead, this, &RelayLink::onReadyRead);
  connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
    if (e != QAbstractSocket::RemoteHostClosedError)
      Q_EMIT relayError(QStringLiteral("relay connection error: %1").arg(int(e)));
  });
}

void RelayLink::joinRelay(const QUrl &relayUrl, const QString &roomToken, Role role)
{
  m_role = role;
  m_room = roomToken;

  QString host = relayUrl.host();
  quint16 port = relayUrl.port(25910);
  m_socket.connectToHost(host, port);

  QTimer::singleShot(kRegisterTimeoutMs, this, [this] {
    if (m_socket.state() != QAbstractSocket::ConnectedState)
      Q_EMIT relayError(QStringLiteral("relay connect timeout"));
  });
}

void RelayLink::leave()
{
  m_socket.disconnectFromHost();
  m_joined = false;
}

void RelayLink::onConnected()
{
  // REGISTER frame: 2-byte length prefix + JSON
  const QJsonObject reg{{"type", "REGISTER"},
                        {"room", m_room},
                        {"role", m_role == Role::Host ? "host" : "guest"},
                        {"device_id", m_self.deviceId()}};
  const QByteArray payload = QJsonDocument(reg).toJson(QJsonDocument::Compact);
  QByteArray frame;
  frame.append(char((payload.size() >> 8) & 0xFF));
  frame.append(char(payload.size() & 0xFF));
  frame.append(payload);
  m_socket.write(frame);
  // wait for PEER_JOINED before reporting ready
}

void RelayLink::onReadyRead()
{
  m_buffer.append(m_socket.readAll());

  while (m_buffer.size() >= 2) {
    const int len = (quint8(m_buffer[0]) << 8) | quint8(m_buffer[1]);

    // small frames = relay control messages; large = spliced data
    if (m_buffer.size() < 2 + len && len <= 4096)
      return; // need more of a control frame

    if (len <= 4096) {
      const QByteArray payload = m_buffer.mid(2, len);
      m_buffer.remove(0, 2 + len);

      const auto doc = QJsonDocument::fromJson(payload);
      if (doc.object().value("type") == QLatin1String("PEER_JOINED")) {
        m_joined = true;
        Q_EMIT peerJoined();
      } else if (doc.object().value("type") == QLatin1String("ROOM_FULL")) {
        Q_EMIT relayError(QStringLiteral("relay room already has two peers"));
        leave();
        return;
      } else if (doc.object().value("type") == QLatin1String("ROLE_TAKEN")) {
        Q_EMIT relayError(QStringLiteral("relay room role already taken"));
        leave();
        return;
      }
    } else {
      // spliced application data (post-join)
      if (!m_joined)
        return; // data before join is a protocol error; wait it out
      const QByteArray data = m_buffer.left(2 + len);
      m_buffer.remove(0, 2 + len);
      Q_EMIT dataReceived(data.mid(2));
    }
  }
}

void RelayLink::sendData(const QByteArray &data)
{
  if (!m_joined)
    return;
  QByteArray frame;
  frame.append(char((data.size() >> 8) & 0xFF));
  frame.append(char(data.size() & 0xFF));
  frame.append(data);
  m_socket.write(frame);
}

} // namespace litekvm
