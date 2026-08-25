// SPDX-License-Identifier: GPL-2.0
// LiteKVM mDNS advertiser — raw multicast-DNS announce on UDP/5353.
//
// Qt 6.8 has browse-only public API, so publishing is implemented here with a
// minimal DNS responder: we answer mDNS queries (PTR/SRV/TXT for
// _litekvm._tcp.local.) and send periodic unsolicited announcements.
//
// Spec: docs/discovery-pairing-spec.md §3 (litekvm-design repo)
#include "MdnsAdvertiser.h"

#include <QNetworkInterface>
#include <QUdpSocket>

namespace litekvm {

namespace {

const QHostAddress kMdnsGroupV4{QStringLiteral("224.0.0.251")};
const quint16 kMdnsPort = 5353;

void appendDnsName(QByteArray &out, const QString &name)
{
  // encode as sequence of length-prefixed labels; "." splits labels
  const QStringList labels = name.split(QLatin1Char('.'), Qt::SkipEmptyParts);
  for (const QString &label : labels) {
    const QByteArray utf8 = label.toUtf8();
    out.append(char(utf8.size()));
    out.append(utf8);
  }
  out.append(char(0));
}

void appendU16(QByteArray &out, quint16 v)
{
  out.append(char(v >> 8));
  out.append(char(v & 0xFF));
}

void appendU32(QByteArray &out, quint32 v)
{
  out.append(char((v >> 24) & 0xFF));
  out.append(char((v >> 16) & 0xFF));
  out.append(char((v >> 8) & 0xFF));
  out.append(char(v & 0xFF));
}

// build one answer record (single name, given type + rdata)
QByteArray buildAnswer(const QString &fullName, quint16 type, const QByteArray &rdata,
                       quint32 ttl = 4500)
{
  QByteArray a;
  appendDnsName(a, fullName);
  appendU16(a, type);
  appendU16(a, 1);           // class IN + cache-flush bit (0x8001) would be typical; use plain IN
  a[a.size() - 2] = char(0x80); // set cache-flush bit on class field
  appendU32(a, ttl);
  appendU16(a, quint16(rdata.size()));
  a.append(rdata);
  return a;
}

} // namespace

MdnsAdvertiser::MdnsAdvertiser(QObject *parent) : QObject(parent)
{
  connect(&m_socket, &QUdpSocket::readyRead, this, &MdnsAdvertiser::onReadyRead);
}

bool MdnsAdvertiser::start(const DeviceIdentity &self, uint16_t pairingPort, PeerState state)
{
  m_self = &self;
  m_pairingPort = pairingPort;
  m_state = state;

  // join multicast group on all IPv4 interfaces
  bool joined = false;
  for (const QHostAddress &addr : allLocalAddresses()) {
    if (m_socket.bind(addr, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
      joined = true;
      break;
    }
  }
  if (!joined && !m_socket.bind(QHostAddress::AnyIPv4, kMdnsPort,
                                QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
    return false;
  }
  m_socket.joinMulticastGroup(kMdnsGroupV4);

  announce();
  return true;
}

void MdnsAdvertiser::setState(PeerState state)
{
  m_state = state;
  announce(); // state change → re-announce so peers refresh TXT
}

void MdnsAdvertiser::stop()
{
  m_socket.close();
}

QStringList MdnsAdvertiser::txtFields() const
{
  return {QStringLiteral("device-id=%1").arg(m_self->deviceId()),
          QStringLiteral("name=%1").arg(m_self->name()),
          QStringLiteral("platform=")
#if defined(Q_OS_WIN)
              + QStringLiteral("win")
#elif defined(Q_OS_MAC)
              + QStringLiteral("mac")
#else
              + QStringLiteral("linux")
#endif
          ,
          QStringLiteral("fp=%1").arg(m_self->fingerprint()), QStringLiteral("proto=v1"),
          QStringLiteral("port=%1").arg(m_pairingPort),
          QStringLiteral("state=%1")
              .arg(m_state == PeerState::Pairable ? QStringLiteral("pairable") : QStringLiteral("busy"))};
}

QByteArray MdnsAdvertiser::buildAnnouncement() const
{
  const QString instance = QStringLiteral("%1.%2").arg(sanitizeInstanceName(m_self->name()),
                                                       DiscoveryService::kServiceType);
  const QString serviceEnum =
      DiscoveryService::kServiceType;                       // _litekvm._tcp.local.
  const QString host = QStringLiteral("%1.local.").arg(m_self->deviceId());

  // --- PTR: _litekvm._tcp.local. -> <instance> ---
  QByteArray ptrRdata;
  appendDnsName(ptrRdata, instance);

  // --- SRV: port + target host ---
  QByteArray srvRdata;
  appendU16(srvRdata, 0); // priority
  appendU16(srvRdata, 0); // weight
  appendU16(srvRdata, m_pairingPort);
  appendDnsName(srvRdata, host);

  // --- TXT: packed key=value strings ---
  QByteArray txtRdata;
  for (const QString &kv : txtFields()) {
    const QByteArray field = kv.toUtf8();
    txtRdata.append(char(field.size()));
    txtRdata.append(field);
  }

  QByteArray packet;
  // header: id=0 (mDNS), flags=0x8400 (response, authoritative), counts
  appendU16(packet, 0);
  appendU16(packet, 0x8400);
  appendU16(packet, 0); // qdcount
  appendU16(packet, 3); // ancount (PTR SRV TXT)
  appendU16(packet, 0); // nscount
  appendU16(packet, 0); // arcount

  packet.append(buildAnswer(serviceEnum, 12 /*PTR*/, ptrRdata));
  packet.append(buildAnswer(instance, 33 /*SRV*/, srvRdata));
  packet.append(buildAnswer(instance, 16 /*TXT*/, txtRdata));

  return packet;
}

void MdnsAdvertiser::announce()
{
  const QByteArray payload = buildAnnouncement();
  m_socket.writeDatagram(payload, kMdnsGroupV4, kMdnsPort);
}

void MdnsAdvertiser::onReadyRead()
{
  while (m_socket.hasPendingDatagrams()) {
    QByteArray datagram;
    datagram.resize(int(m_socket.pendingDatagramSize()));
    QHostAddress sender;
    m_socket.readDatagram(datagram.data(), datagram.size(), &sender);

    // minimal query parsing: find "_litekvm" in the question section and
    // answer unicast to the asker (legacy mDNS quirk) + multicast
    if (datagram.contains("_litekvm")) {
      if (!datagram.contains("_litekvm._tcp.local.") || hasQuestionTail(datagram)) {
        // reply directly to the querier too (helps some resolvers)
        m_socket.writeDatagram(buildAnnouncement(), sender, kMdnsPort);
      }
      announce();
    }
  }
}

bool MdnsAdvertiser::hasQuestionTail(const QByteArray &) const
{
  return false; // conservative: always also answer via multicast above
}

QString MdnsAdvertiser::sanitizeInstanceName(const QString &raw)
{
  QString s = raw;
  s.replace(QLatin1Char('.'), QLatin1Char(' ')); // dots would break DNS labels
  s.replace(QLatin1Char('\\'), QLatin1Char(' '));
  if (s.trimmed().isEmpty())
    s = QStringLiteral("litekvm-device");
  return s.trimmed();
}

std::vector<QHostAddress> MdnsAdvertiser::allLocalAddresses()
{
  std::vector<QHostAddress> out;
  for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
    if (!(iface.flags() & QNetworkInterface::CanMulticast))
      continue;
    for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
      const QHostAddress addr = entry.ip();
      if (addr.protocol() == QAbstractSocket::IPv4Protocol)
        out.push_back(addr);
    }
  }
  return out;
}

} // namespace litekvm
