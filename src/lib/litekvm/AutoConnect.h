// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#pragma once

#include "DeviceIdentity.h"
#include "DiscoveryService.h"
#include "TrustStore.h"

#include <QSet>
#include <QObject>

namespace litekvm {

/**
 * @brief When enabled, every paired peer that appears on the LAN triggers
 * autoConnectRequested(peer). The controller turns that into a session.
 * One attempt per device per session — no retry spam.
 */
class AutoConnect : public QObject {
  Q_OBJECT

public:
  AutoConnect(const DeviceIdentity &self, TrustStore &trust, DiscoveryService &discovery,
              QObject *parent = nullptr);

  void setEnabled(bool on);
  bool enabled() const
  {
    return m_enabled;
  }

Q_SIGNALS:
  void autoConnectRequested(const litekvm::DiscoveredPeer &peer);

private Q_SLOTS:
  void onPeerSeen(const DiscoveredPeer &peer);

private:
  const DeviceIdentity &m_self;
  TrustStore &m_trust;
  DiscoveryService &m_discovery;
  bool m_enabled = false;
  QSet<QString> m_attempted;
};

} // namespace litekvm
