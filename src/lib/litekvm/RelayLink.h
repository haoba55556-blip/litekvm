// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#pragma once

#include "DeviceIdentity.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QUrl>

namespace litekvm {

/**
 * @brief Connects out to a self-hosted relay (relay/main.go) and joins a
 * room. When both peers are present the socket becomes a transparent pipe;
 * caller sends/receives framed application data (input stream, end-to-end
 * encrypted upstream by deskflow TLS).
 *
 * Usage:
 *   link.joinRelay(QUrl("relay.example.com:25910"), token, Role::Host);
 *   ... on peerJoined() -> start deskflow core over this socket
 */
class RelayLink : public QObject {
  Q_OBJECT

public:
  enum class Role { Host, Guest };

  explicit RelayLink(const DeviceIdentity &self, QObject *parent = nullptr);

  void joinRelay(const QUrl &relayUrl, const QString &roomToken, Role role);
  void leave();
  bool joined() const
  {
    return m_joined;
  }

  void sendData(const QByteArray &data);

Q_SIGNALS:
  void peerJoined();
  void dataReceived(const QByteArray &data);
  void relayError(const QString &message);

private Q_SLOTS:
  void onConnected();
  void onReadyRead();

private:
  const DeviceIdentity &m_self;
  QTcpSocket m_socket;
  QByteArray m_buffer;
  QString m_room;
  Role m_role = Role::Guest;
  bool m_joined = false;
};

} // namespace litekvm
