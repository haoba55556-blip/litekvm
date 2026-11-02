// SPDX-License-Identifier: GPL-2.0
// LiteKVM mDNS discovery — raw multicast-DNS listener for peer announcements.
//
// Qt 6.8 has no mDNS API (QDnsLookup is unicast-only), so discovery listens on
// UDP/5353 and parses our own announcement packets produced by MdnsAdvertiser.
//
// Spec: docs/discovery-pairing-spec.md §3 (litekvm-design repo)
#include "DiscoveryService.h"
#include "LanNic.h"
#include "MdnsAdvertiser.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QUdpSocket>

namespace litekvm {

const QString DiscoveryService::kServiceType = QStringLiteral("_litekvm._tcp.local.");

namespace {
constexpr qint64 kStaleAfterMs = 30'000;   // drop peers silent for 30s
} // namespace

DiscoveryService::DiscoveryService(QObject *parent) : QObject(parent) {}

DiscoveryService::~DiscoveryService()
{
  stop();
}

QString DiscoveryService::normalizeHost(const QString &host)
{
  QString trimmed = host.trimmed();
  const int scope = trimmed.indexOf(QLatin1Char('%'));
  if (scope > 0)
    trimmed = trimmed.left(scope);
  if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']')))
    trimmed = trimmed.mid(1, trimmed.size() - 2);

  QHostAddress addr(trimmed);
  if (addr.isNull())
    return trimmed;

  if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
    if (addr.isMulticast() || addr.isLoopback())
      return {};
    return addr.toString();
  }

  bool ok = false;
  const quint32 v4 = addr.toIPv4Address(&ok);
  if (ok) {
    const QHostAddress mapped(v4);
    if (mapped.isMulticast() || mapped.isLoopback())
      return {};
    return mapped.toString();
  }
  if (addr.isMulticast() || addr.isLoopback())
    return {};
  return addr.toString();
}

QByteArray DiscoveryService::buildBrowseQuery()
{
  // DNS header: id=0, flags=0 (query), qdcount=1. Question is
  // _litekvm._tcp.local. PTR IN.
  auto appendName = [](QByteArray &out, const QString &name) {
    const QStringList labels = name.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (const QString &label : labels) {
      const QByteArray utf8 = label.toUtf8();
      out.append(char(utf8.size()));
      out.append(utf8);
    }
    out.append(char(0));
  };
  QByteArray packet;
  packet.append(char(0));
  packet.append(char(0)); // id
  packet.append(char(0));
  packet.append(char(0)); // flags
  packet.append(char(0));
  packet.append(char(1)); // qdcount
  packet.append(QByteArray(6, char(0))); // an/ns/ar
  appendName(packet, kServiceType);
  packet.append(char(0));
  packet.append(char(12)); // PTR
  packet.append(char(0));
  packet.append(char(1)); // IN
  return packet;
}

bool DiscoveryService::start(const DeviceIdentity &self, const TrustStore *trust)
{
  stop();
  m_self = &self;
  m_trust = trust;

  // One socket on 5353 joined to every real IPv4 interface. Binding only the
  // first CanMulticast adapter misses the LAN on machines that also have
  // Hyper-V / WSL / VPN adapters (those are often enumerated first).
  m_socket = new QUdpSocket(this);
  const auto share = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
  bool bound = m_socket->bind(QHostAddress::AnyIPv4, 5353, share);
  if (!bound)
    bound = m_socket->bind(QHostAddress::AnyIPv4, 0, share);
  if (!bound) {
    m_socket->deleteLater();
    m_socket = nullptr;
    return false;
  }

  if (joinMdnsGroups(m_socket) == 0)
    m_socket->joinMulticastGroup(QHostAddress(QStringLiteral("224.0.0.251")));

  connect(m_socket, &QUdpSocket::readyRead, this, &DiscoveryService::onReadyRead);

  m_pruneTimer.setInterval(5000);
  connect(&m_pruneTimer, &QTimer::timeout, this, &DiscoveryService::pruneStale, Qt::UniqueConnection);
  m_pruneTimer.start();

  sendBrowseQuery();
  return true;
}

void DiscoveryService::stop()
{
  m_pruneTimer.stop();
  if (m_socket) {
    m_socket->close();
    m_socket->deleteLater();
    m_socket = nullptr;
  }
}

void DiscoveryService::sendBrowseQuery()
{
  if (!m_socket)
    return;
  sendMdnsDatagram(m_socket, buildBrowseQuery(), 5353);
}

void DiscoveryService::onReadyRead()
{
  if (!m_socket || !m_self)
    return;
  while (m_socket->hasPendingDatagrams()) {
    QByteArray dgram;
    dgram.resize(int(m_socket->pendingDatagramSize()));
    QHostAddress sender;
    m_socket->readDatagram(dgram.data(), dgram.size(), &sender);

    // Our announcements are DNS responses carrying "_litekvm" TXT records.
    if (!dgram.contains("_litekvm"))
      continue;

    const QString host = normalizeHost(sender.toString());
    if (host.isEmpty())
      continue;

    QMap<QString, QString> txt;
    quint16 port = 25901;

    auto grab = [&](const QByteArray &key) -> QString {
      const int idx = dgram.indexOf(key);
      if (idx < 0)
        return {};
      const int end = dgram.indexOf(char(0), idx);
      const int stop = end < 0 ? dgram.size() : qMin(end, idx + 200);
      return QString::fromUtf8(dgram.mid(idx + key.size(), stop - idx - key.size()));
    };
    txt["device-id"] = grab("device-id=");
    txt["name"] = grab("name=");
    txt["platform"] = grab("platform=");
    txt["fp"] = grab("fp=");
    txt["state"] = grab("state=");
    const QString portStr = grab("port=");
    if (!portStr.isEmpty())
      port = portStr.toUShort();

    if (!txt["device-id"].isEmpty() && txt["device-id"] != m_self->deviceId())
      ingestTxtRecord(txt, host, port, true);
  }
}

void DiscoveryService::ingestTxtRecord(const QMap<QString, QString> &txt, const QString &host,
                                       uint16_t port, bool notify)
{
  DiscoveredPeer p;
  p.deviceId = txt.value("device-id");
  p.name = txt.value("name");
  p.platform = txt.value("platform");
  p.fingerprint = txt.value("fp");
  p.host = host;
  p.port = port ? port : 25901;
  p.state = DiscoveredPeer::stateFromString(txt.value("state", QStringLiteral("pairable")));

  if (p.deviceId.isEmpty() || p.name.isEmpty() || p.deviceId == m_self->deviceId())
    return;

  if (m_trust && m_trust->contains(p.deviceId))
    p.state = DiscoveredPeer::State::Paired;

  auto it = m_peers.find(p.deviceId);
  const bool isNew = it == m_peers.end();
  m_peers[p.deviceId] = p;
  m_lastSeenMs[p.deviceId] = QDateTime::currentMSecsSinceEpoch();

  if (isNew)
    Q_EMIT peerDiscovered(p);
  else if (notify)
    Q_EMIT peerUpdated(p);
}

void DiscoveryService::ingestTxtForTest(const QMap<QString, QString> &txt, const QString &host,
                                        quint16 port)
{
  ingestTxtRecord(txt, host, port, true);
}

void DiscoveryService::pruneStale()
{
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  std::vector<QString> dead;
  for (const auto &[id, seen] : m_lastSeenMs)
    if (now - seen > kStaleAfterMs)
      dead.push_back(id);
  for (const auto &id : dead) {
    m_peers.erase(id);
    m_lastSeenMs.erase(id);
    Q_EMIT peerOffline(id);
  }
}

std::vector<DiscoveredPeer> DiscoveryService::peers() const
{
  std::vector<DiscoveredPeer> out;
  out.reserve(m_peers.size());
  for (const auto &[id, p] : m_peers)
    out.push_back(p);
  return out;
}

std::optional<DiscoveredPeer> DiscoveryService::peer(const QString &deviceId) const
{
  auto it = m_peers.find(deviceId);
  if (it == m_peers.end())
    return std::nullopt;
  return it->second;
}

} // namespace litekvm
