// SPDX-License-Identifier: GPL-2.0
// LiteKVM mesh layout sync — control-plane TCP/25902, LAYOUT_UPDATE flood,
// last-write-wins by monotonic rev.
//
// Spec: docs/mesh-layout-sync-spec.md §3-§4 (litekvm-design repo)
#include "LayoutSync.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace litekvm {

namespace {
constexpr int kFrameHeaderBytes = 2;
constexpr int kMaxFrameSize = 256 * 1024;
constexpr qint64 kHeartbeatMs = 10'000;
} // namespace

QByteArray LayoutSync::encodeFrame(const QJsonObject &obj)
{
  const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  QByteArray frame;
  frame.append(char((payload.size() >> 8) & 0xFF));
  frame.append(char(payload.size() & 0xFF));
  frame.append(payload);
  return frame;
}

std::optional<QJsonObject> LayoutSync::decodeFrame(const QByteArray &payload)
{
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return std::nullopt;
  return doc.object();
}

QJsonObject LayoutSync::layoutToJson(const ScreenLayout &layout)
{
  QJsonArray screens;
  for (const auto &s : layout.screens) {
    screens.append(QJsonObject{{"device_id", s.deviceId},
                               {"x", s.x},
                               {"y", s.y},
                               {"w", s.w},
                               {"h", s.h}});
  }
  return QJsonObject{{"type", "LAYOUT_UPDATE"},
                     {"rev", layout.rev},
                     {"updated_by", layout.updatedBy},
                     {"ts", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                     {"screens", screens}};
}

std::optional<ScreenLayout> LayoutSync::layoutFromJson(const QJsonObject &obj)
{
  if (obj.value("type").toString() != QLatin1String("LAYOUT_UPDATE"))
    return std::nullopt;

  ScreenLayout out;
  out.rev = qint64(obj.value("rev").toDouble());
  out.updatedBy = obj.value("updated_by").toString();

  const auto arr = obj.value("screens").toArray();
  for (const auto &v : arr) {
    const auto o = v.toObject();
    ScreenRect r;
    r.deviceId = o.value("device_id").toString();
    r.x = o.value("x").toInt();
    r.y = o.value("y").toInt();
    r.w = o.value("w").toInt();
    r.h = o.value("h").toInt();
    if (!r.deviceId.isEmpty())
      out.screens.push_back(r);
  }
  if (out.rev < 0 || out.screens.empty())
    return std::nullopt;
  return out;
}

LayoutSync::LayoutSync(const DeviceIdentity &self, QObject *parent)
  : QObject(parent), m_self(self)
{
}

bool LayoutSync::start(quint16 port)
{
  connect(&m_server, &QTcpServer::newConnection, this, [this] {
    while (m_server.hasPendingConnections()) {
      QTcpSocket *sock = m_server.nextPendingConnection();
      if (!sock)
        continue;
      connect(sock, &QTcpSocket::readyRead, this, &LayoutSync::onReadyRead);
      connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
      m_peers.insert(sock);
    }
  });
  return m_server.listen(QHostAddress::AnyIPv4, port);
}

void LayoutSync::stop()
{
  m_server.close();
  for (auto *s : m_peers)
    s->disconnectFromHost();
}

void LayoutSync::connectToPeer(const QString &host, quint16 port)
{
  auto *sock = new QTcpSocket(this);
  connect(sock, &QTcpSocket::connected, this, [this, sock] {
    m_peers.insert(sock);
    // bring the newcomer up to date immediately
    if (m_layout)
      sock->write(encodeFrame(layoutToJson(*m_layout)));
    startHeartbeat();
  });
  sock->connectToHost(host, port);
}

void LayoutSync::publish(ScreenLayout layout)
{
  // rev must be strictly greater than anything we've seen
  layout.rev = qMax(m_lastRev + 1, layout.rev);
  layout.updatedBy = m_self.deviceId();
  applyIncoming(layout);
  broadcast(layoutToJson(layout));
}

void LayoutSync::applyIncoming(const ScreenLayout &incoming)
{
  if (incoming.rev > m_lastRev) {
    m_layout = incoming;
    m_lastRev = incoming.rev;
    Q_EMIT layoutChanged(*m_layout);
  }
}

void LayoutSync::broadcast(const QJsonObject &msg)
{
  const QByteArray frame = encodeFrame(msg);
  for (auto *sock : std::as_const(m_peers)) {
    if (sock->state() == QAbstractSocket::ConnectedState)
      sock->write(frame);
  }
}

void LayoutSync::startHeartbeat()
{
  if (m_heartbeat)
    return;
  m_heartbeat = new QTimer(this);
  connect(m_heartbeat, &QTimer::timeout, this, [this] {
    static const QByteArray ping = [] {
      return encodeFrame(QJsonObject{{"type", "PING"}});
    }();
    for (auto *sock : std::as_const(m_peers)) {
      if (sock->state() == QAbstractSocket::ConnectedState)
        sock->write(ping);
    }
  });
  m_heartbeat->start(kHeartbeatMs);
}

void LayoutSync::onReadyRead()
{
  auto *sock = qobject_cast<QTcpSocket *>(sender());
  if (!sock)
    return;

  QByteArray &buf = m_buffers[sock];
  buf.append(sock->readAll());

  while (true) {
    if (buf.size() < kFrameHeaderBytes)
      return;
    const int len = (quint8(buf[0]) << 8) | quint8(buf[1]);
    if (len > kMaxFrameSize) {
      sock->disconnectFromHost();
      return;
    }
    if (buf.size() < kFrameHeaderBytes + len)
      return;

    const QByteArray payload = buf.mid(kFrameHeaderBytes, len);
    buf.remove(0, kFrameHeaderBytes + len);

    auto msg = decodeFrame(payload);
    if (!msg)
      continue;

    if (msg->value("type") == QLatin1String("LAYOUT_UPDATE")) {
      auto incoming = layoutFromJson(*msg);
      if (!incoming)
        continue;
      const bool newer = incoming->rev > m_lastRev;
      applyIncoming(*incoming);
      if (!newer && m_layout && m_lastRev > incoming->rev) {
        // we have a newer copy — feed it back so the sender converges
        sock->write(encodeFrame(layoutToJson(*m_layout)));
      } else if (newer) {
        // flood to other peers (dedup by rev: they ignore stale)
        broadcast(layoutToJson(*m_layout));
      }
    }
    // PING/PONG need no action; TCP keepalive is enough at this scale
  }
}

} // namespace litekvm
