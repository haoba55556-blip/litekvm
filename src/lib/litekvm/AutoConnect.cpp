// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// AutoConnect — watches discovery; when a *paired* peer comes online,
// automatically establishes the session (no user interaction needed).
//
// Part of litekvm-pro feature 2 (boot autostart + auto-connect).
#include "AutoConnect.h"

namespace litekvm {

AutoConnect::AutoConnect(const DeviceIdentity &self, TrustStore &trust, DiscoveryService &discovery,
                         QObject *parent)
  : QObject(parent), m_self(self), m_trust(trust), m_discovery(discovery)
{
  connect(&m_discovery, &DiscoveryService::peerDiscovered, this, &AutoConnect::onPeerSeen);
  connect(&m_discovery, &DiscoveryService::peerUpdated, this, &AutoConnect::onPeerSeen);
}

void AutoConnect::setEnabled(bool on)
{
  m_enabled = on;
  if (on) {
    // peers discovered before enabling are already in the map — re-check them
    for (const auto &peer : m_discovery.peers())
      onPeerSeen(peer);
  }
}

void AutoConnect::onPeerSeen(const DiscoveredPeer &peer)
{
  if (!m_enabled)
    return;
  if (m_attempted.contains(peer.deviceId))
    return; // one attempt per device per session
  if (!m_trust.contains(peer.deviceId))
    return; // only auto-connect devices the user explicitly paired
  if (peer.state != DiscoveredPeer::State::Paired)
    return;

  m_attempted.insert(peer.deviceId);
  Q_EMIT autoConnectRequested(peer);
}

} // namespace litekvm
