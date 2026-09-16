// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// RoleNegotiation — elects one server among N paired peers (feature 3:
// multi-machine sharing). Election rule per spec §2: lexicographically
// smallest online device_id wins; 5s hysteresis prevents flapping on
// transient disconnects.
#include "RoleNegotiation.h"

#include <QDateTime>

namespace litekvm {

namespace {
constexpr int kHysteresisMs = 5'000;
} // namespace

RoleNegotiation::RoleNegotiation(const DeviceIdentity &self, QObject *parent)
  : QObject(parent), m_self(self)
{
  m_graceTimer.setSingleShot(true);
  connect(&m_graceTimer, &QTimer::timeout, this, &RoleNegotiation::reelect);
}

void RoleNegotiation::setOnline(const QString &deviceId)
{
  if (deviceId == m_self.deviceId() || m_online.contains(deviceId))
    return;
  m_online.insert(deviceId);
  reelect();
}

void RoleNegotiation::setOffline(const QString &deviceId)
{
  if (!m_online.remove(deviceId))
    return;
  m_offlineSince.insert(deviceId, QDateTime::currentMSecsSinceEpoch());
  reelect();
}

void RoleNegotiation::reset(const QSet<QString> &onlineIds)
{
  m_online = onlineIds;
  m_online.remove(m_self.deviceId());
  m_offlineSince.clear();
  m_graceTimer.stop();
  reelect();
}

QString RoleNegotiation::electServer() const
{
  // smallest device_id among (self + online peers) becomes the server
  QString best = m_self.deviceId();
  for (const auto &id : m_online)
    if (id < best)
      best = id;
  return best;
}

void RoleNegotiation::reelect()
{
  const QString newServer = electServer();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();

  // hysteresis: don't switch away from the current server until it has been
  // offline for kHysteresisMs; schedule a re-election for when it expires
  if (!m_currentServer.isEmpty() && newServer != m_currentServer) {
    const auto since = m_offlineSince.value(m_currentServer, 0);
    if (since != 0) {
      const qint64 remaining = kHysteresisMs - (now - since);
      if (remaining > 0) {
        m_graceTimer.start(int(remaining));
        return; // keep the old server a bit longer (transient blip)
      }
      m_offlineSince.remove(m_currentServer);
    }
  }
  m_graceTimer.stop();

  if (newServer != m_currentServer) {
    m_currentServer = newServer;
    ++m_epoch;
    Q_EMIT roleChanged(newServer, m_epoch,
                       newServer == m_self.deviceId() ? Role::Server : Role::Client);
  }
}

} // namespace litekvm
