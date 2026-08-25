// SPDX-License-Identifier: GPL-2.0
// LiteKVM mDNS advertiser — raw multicast-DNS announce on UDP/5353.
#pragma once

#include "DiscoveryService.h"
#include "DeviceIdentity.h"

#include <QHostAddress>
#include <QObject>
#include <QUdpSocket>

#include <vector>

namespace litekvm {

/**
 * @brief Publishes this device as _litekvm._tcp.local. via raw mDNS.
 * Qt 6.8 lacks a public publish API, hence the hand-rolled responder.
 */
class MdnsAdvertiser : public QObject {
  Q_OBJECT

public:
  enum class PeerState { Pairable, Busy };

  explicit MdnsAdvertiser(QObject *parent = nullptr);

  bool start(const DeviceIdentity &self, uint16_t pairingPort, PeerState state = PeerState::Pairable);
  void setState(PeerState state);
  void stop();

private Q_SLOTS:
  void onReadyRead();

private:
  QStringList txtFields() const;
  QByteArray buildAnnouncement() const;
  void announce();
  bool hasQuestionTail(const QByteArray &datagram) const;
  static QString sanitizeInstanceName(const QString &raw);
  static std::vector<QHostAddress> allLocalAddresses();

  QUdpSocket m_socket;
  const DeviceIdentity *m_self = nullptr;
  uint16_t m_pairingPort = 25901;
  PeerState m_state = PeerState::Pairable;
};

} // namespace litekvm
