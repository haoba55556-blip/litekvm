// SPDX-License-Identifier: GPL-2.0
// LiteKVM mDNS discovery — raw multicast-DNS listener for peer announcements.
//
// Qt 6.8 has no mDNS API (QDnsLookup is unicast-only), so discovery listens on
// UDP/5353 and parses our own announcement packets produced by MdnsAdvertiser.
//
// Spec: docs/discovery-pairing-spec.md §3 (litekvm-design repo)
#include "DiscoveryService.h"
#include "MdnsAdvertiser.h"

#include <QElapsedTimer>
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

bool DiscoveryService::start(const DeviceIdentity &self, const TrustStore *trust)
{
  m_self = &self;
  m_trust = trust;

  // Join the mDNS multicast group; announcements from peers arrive here.
  // The heavy lifting (socket + parse loop) lives in MdnsAdvertiser's socket;
  // for discovery we run an independent listener socket bound to 5353.
  auto *sock = new QUdpSocket(this);
  bool bound = false;
  for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
    if (!(iface.flags() & QNetworkInterface::CanMulticast))
      continue;
    for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
      if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol &&
          sock->bind(entry.ip(), 5353, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        bound = true;
        break;
      }
    }
    if (bound)
      break;
  }
  if (!bound)
    bound = sock->bind(QHostAddress::AnyIPv4, 5353,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
  if (!bound)
    return false;

  const QHostAddress group(QStringLiteral("224.0.0.251"));
  sock->joinMulticastGroup(group);

  connect(sock, &QUdpSocket::readyRead, this, [this, sock] {
    while (sock->hasPendingDatagrams()) {
      QByteArray dgram;
      dgram.resize(int(sock->pendingDatagramSize()));
      QHostAddress sender;
      sock->readDatagram(dgram.data(), dgram.size(), &sender);

      // Our announcements are DNS responses carrying "_litekvm" TXT records.
      // Parse pragmatically: locate each "key=value" TXT string in the packet.
      if (!dgram.contains("_litekvm"))
        continue;

      QMap<QString, QString> txt;
      QString host = sender.toString();
      quint16 port = 25901;

      // scan for known keys anywhere in the payload (TXT records are plain)
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
  });

  return true;
}

void DiscoveryService::stop()
{
  // sockets are child objects; nothing else to do
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
