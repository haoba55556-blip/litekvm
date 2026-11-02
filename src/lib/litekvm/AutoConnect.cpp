// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// AutoConnect — watches discovery; when a *paired* peer comes online,
// automatically establishes the session (no user interaction needed).
//
// Part of litekvm-pro feature 2 (boot autostart + auto-connect).
#include "AutoConnect.h"

#include <QTimer>

namespace litekvm {

namespace {
const QList<int> kDefaultBackoffMs = {1000, 2000, 4000, 8000, 10000};
}

AutoConnect::AutoConnect(const DeviceIdentity &self, TrustStore &trust, DiscoveryService &discovery,
                         QObject *parent)
  : QObject(parent), m_self(self), m_trust(trust), m_discovery(discovery), m_backoffMs(kDefaultBackoffMs)
{
  connect(&m_discovery, &DiscoveryService::peerDiscovered, this, &AutoConnect::onPeerSeen);
  connect(&m_discovery, &DiscoveryService::peerUpdated, this, &AutoConnect::onPeerSeen);
  connect(&m_discovery, &DiscoveryService::peerOffline, this, &AutoConnect::onPeerOffline);
}

void AutoConnect::setBackoffScheduleForTest(const QList<int> &milliseconds)
{
  m_backoffMs = milliseconds.isEmpty() ? kDefaultBackoffMs : milliseconds;
}

void AutoConnect::setEnabled(bool on)
{
  m_enabled = on;
  if (!on) {
    for (auto it = m_retryTimers.begin(); it != m_retryTimers.end(); ++it)
      it.value()->stop();
    m_inflight.clear();
    m_up.clear();
    return;
  }
  // peers discovered before enabling are already in the map — re-check them
  for (const auto &peer : m_discovery.peers())
    onPeerSeen(peer);
}

bool AutoConnect::eligible(const DiscoveredPeer &peer) const
{
  if (peer.deviceId.isEmpty() || peer.host.isEmpty())
    return false;
  if (!m_trust.contains(peer.deviceId))
    return false;
  if (peer.state != DiscoveredPeer::State::Paired)
    return false;
  return true;
}

void AutoConnect::onPeerSeen(const DiscoveredPeer &peer)
{
  if (!m_enabled || !eligible(peer))
    return;

  const QString previousHost = m_seen.value(peer.deviceId).host;
  const bool known = m_seen.contains(peer.deviceId);
  m_seen.insert(peer.deviceId, peer);

  if (m_up.contains(peer.deviceId)) {
    if (!known || previousHost == peer.host)
      return;
    // DHCP / interface change: the running session is pointed at a dead address.
    m_up.remove(peer.deviceId);
    m_inflight.remove(peer.deviceId);
    stopTimer(peer.deviceId);
  }

  if (m_inflight.contains(peer.deviceId))
    return;

  requestConnect(peer);
}

void AutoConnect::onPeerOffline(const QString &deviceId)
{
  m_seen.remove(deviceId);
  m_up.remove(deviceId);
  m_inflight.remove(deviceId);
  m_failCount.remove(deviceId);
  stopTimer(deviceId);
}

void AutoConnect::noteSessionUp(const QString &deviceId)
{
  if (deviceId.isEmpty())
    return;
  m_up.insert(deviceId);
  m_inflight.remove(deviceId);
  m_failCount.remove(deviceId);
  stopTimer(deviceId);
}

void AutoConnect::noteSessionDown(const QString &deviceId)
{
  m_up.remove(deviceId);
  m_inflight.remove(deviceId);
  stopTimer(deviceId);
  if (!m_enabled || !m_seen.contains(deviceId))
    return;
  scheduleRetry(deviceId);
}

void AutoConnect::requestConnect(const DiscoveredPeer &peer)
{
  if (m_inflight.contains(peer.deviceId) || m_up.contains(peer.deviceId))
    return;
  m_inflight.insert(peer.deviceId);
  Q_EMIT autoConnectRequested(peer);
  // Watchdog: if the GUI never confirms the core is up, try again.
  scheduleRetry(peer.deviceId);
}

void AutoConnect::scheduleRetry(const QString &deviceId)
{
  QTimer *timer = m_retryTimers.value(deviceId);
  if (!timer) {
    timer = new QTimer(this);
    timer->setSingleShot(true);
    m_retryTimers.insert(deviceId, timer);
    connect(timer, &QTimer::timeout, this, [this, deviceId] { onRetry(deviceId); });
  }
  timer->start(backoffMs(deviceId));
}

void AutoConnect::onRetry(const QString &deviceId)
{
  m_inflight.remove(deviceId);
  if (!m_enabled || m_up.contains(deviceId) || !m_seen.contains(deviceId))
    return;
  m_failCount.insert(deviceId, m_failCount.value(deviceId, 0) + 1);
  requestConnect(m_seen.value(deviceId));
}

int AutoConnect::backoffMs(const QString &deviceId) const
{
  if (m_backoffMs.isEmpty())
    return 1000;
  const int index = qMin(m_failCount.value(deviceId, 0), m_backoffMs.size() - 1);
  return qMax(1, m_backoffMs.at(index));
}

void AutoConnect::stopTimer(const QString &deviceId)
{
  if (QTimer *timer = m_retryTimers.value(deviceId))
    timer->stop();
}

} // namespace litekvm
