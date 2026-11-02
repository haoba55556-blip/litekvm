// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#include "LanNic.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QVariant>

namespace litekvm {

namespace {

bool containsAny(const QString &blob)
{
  // QStringLiteral, not Latin-1: "蓝牙" is not a single-byte string.
  const QStringList needles = {
      QStringLiteral("wi-fi direct"), QStringLiteral("wifi direct"), QStringLiteral("wi fi direct"),
      QStringLiteral("wifidirect"),   QStringLiteral("bluetooth"),   QStringLiteral("蓝牙"),
      QStringLiteral("hyper-v"),      QStringLiteral("vethernet"),   QStringLiteral("wsl"),
      QStringLiteral("virtualbox"),   QStringLiteral("vmware"),      QStringLiteral("vbox"),
      QStringLiteral("docker"),       QStringLiteral("npcap"),       QStringLiteral("teredo"),
      QStringLiteral("isatap"),       QStringLiteral("loopback"),    QStringLiteral("pseudo-interface"),
      QStringLiteral("pseudo interface")};
  for (const QString &needle : needles) {
    if (blob.contains(needle))
      return true;
  }
  return false;
}

} // namespace

bool isVirtualAdapterName(const QString &humanName, const QString &systemName)
{
  const QString blob = (humanName + QLatin1Char(' ') + systemName).toLower();
  if (containsAny(blob))
    return true;

  // Windows names Wi-Fi Direct "本地连接* 12" / "Local Area Connection* 12".
  // A plain "本地连接" without the star is a real adapter on older systems.
  const bool starred = humanName.contains(QLatin1Char('*'));
  if (starred && (humanName.contains(QStringLiteral("本地连接")) ||
                  humanName.contains(QStringLiteral("local area connection"), Qt::CaseInsensitive)))
    return true;
  return false;
}

bool isUsableLanNic(const NicHint &nic)
{
  if (!nic.isUp || !nic.isRunning || nic.isLoopback || !nic.canMulticast || !nic.hasRoutableIpv4)
    return false;
  return !isVirtualAdapterName(nic.humanName, nic.systemName);
}

QList<NicHint> usableLanNics(const QList<NicHint> &nics)
{
  QList<NicHint> out;
  for (const NicHint &nic : nics) {
    if (isUsableLanNic(nic))
      out.append(nic);
  }
  return out;
}

namespace {

bool isRoutableV4(const QHostAddress &ip)
{
  if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback() || ip.isMulticast())
    return false;
  // 169.254.0.0/16 is APIPA — a disconnected adapter, not a LAN peer.
  const quint32 v = ip.toIPv4Address();
  return (v & 0xFFFF0000u) != 0xA9FE0000u;
}

const QHostAddress kMdnsGroup{QStringLiteral("224.0.0.251")};

} // namespace

QList<QNetworkInterface> currentLanInterfaces()
{
  QList<QNetworkInterface> out;
  for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
    NicHint hint;
    hint.humanName = iface.humanReadableName();
    hint.systemName = iface.name();
    const auto flags = iface.flags();
    hint.isUp = flags.testFlag(QNetworkInterface::IsUp);
    hint.isRunning = flags.testFlag(QNetworkInterface::IsRunning);
    hint.isLoopback = flags.testFlag(QNetworkInterface::IsLoopBack);
    hint.canMulticast = flags.testFlag(QNetworkInterface::CanMulticast);
    for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
      if (isRoutableV4(entry.ip())) {
        hint.hasRoutableIpv4 = true;
        break;
      }
    }
    if (isUsableLanNic(hint))
      out.append(iface);
  }
  return out;
}

int joinMdnsGroups(QUdpSocket *socket)
{
  if (!socket)
    return 0;
  int joined = 0;
  for (const QNetworkInterface &iface : currentLanInterfaces()) {
    if (socket->joinMulticastGroup(kMdnsGroup, iface))
      ++joined;
  }
  return joined;
}

int sendMdnsDatagram(QUdpSocket *socket, const QByteArray &payload, quint16 port)
{
  if (!socket || payload.isEmpty())
    return 0;
  socket->setSocketOption(QAbstractSocket::MulticastTtlOption, QVariant(255));
  const QList<QNetworkInterface> ifaces = currentLanInterfaces();
  if (ifaces.isEmpty()) {
    socket->writeDatagram(payload, kMdnsGroup, port);
    return 1;
  }
  for (const QNetworkInterface &iface : ifaces) {
    // IP_MULTICAST_IF. Without this, Windows sends the datagram only on the
    // default route, so a second NIC on the same PC stays silent.
    socket->setMulticastInterface(iface);
    socket->writeDatagram(payload, kMdnsGroup, port);
  }
  return ifaces.size();
}

} // namespace litekvm
