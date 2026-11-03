// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Which IPv4 adapters may carry LiteKVM mDNS.
//
// A socket bound to 0.0.0.0 sends multicast on the default route only.
// Machines with a second LAN (or with Wi-Fi Direct / Hyper-V / WSL adapters
// that Windows treats as the multicast interface) then never see each other.
// Callers set IP_MULTICAST_IF on every adapter this filter accepts.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

class QNetworkInterface;
class QUdpSocket;

namespace litekvm {

struct NicHint
{
  QString humanName;
  QString systemName;
  bool isUp = false;
  bool isRunning = false;
  bool isLoopback = false;
  bool canMulticast = false;
  bool hasRoutableIpv4 = false;
};

/// True for adapters that must not carry discovery: Wi-Fi Direct, Bluetooth
/// PAN, Hyper-V / WSL vEthernet, and other virtual NICs.
bool isVirtualAdapterName(const QString &humanName, const QString &systemName);

/// Up, running, non-loopback, multicast-capable, with a real IPv4 address,
/// and not a virtual adapter.
bool isUsableLanNic(const NicHint &nic);

QList<NicHint> usableLanNics(const QList<NicHint> &nics);

/// Live adapters that pass isUsableLanNic(). Empty when none qualify.
QList<QNetworkInterface> currentLanInterfaces();

/// Join 224.0.0.251 on every current LAN adapter. Returns how many joins
/// succeeded.
int joinMdnsGroups(QUdpSocket *socket);

/// Send one datagram on every current LAN adapter by setting
/// IP_MULTICAST_IF (QUdpSocket::setMulticastInterface) before each write.
/// Returns how many sends were attempted. Falls back to a single write on
/// the socket's default interface when no adapter qualifies.
int sendMdnsDatagram(QUdpSocket *socket, const QByteArray &payload, quint16 port = 5353);

} // namespace litekvm
