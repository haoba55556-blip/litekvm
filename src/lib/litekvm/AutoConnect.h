// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "DeviceIdentity.h"
#include "DiscoveryService.h"
#include "TrustStore.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

class QTimer;

namespace litekvm {

/**
 * @brief When enabled, every paired peer that appears on the LAN triggers
 * autoConnectRequested(peer). The controller turns that into a session.
 *
 * A peer is retried with backoff until noteSessionUp() (the core process is
 * running against it) or the peer goes offline. noteSessionDown() — process
 * exit, not a transient TCP blip the core already retries itself — schedules
 * another attempt. A host change (DHCP) while "up" requests a fresh connect.
 */
class AutoConnect : public QObject
{
  Q_OBJECT

public:
  AutoConnect(const DeviceIdentity &self, TrustStore &trust, DiscoveryService &discovery, QObject *parent = nullptr);

  void setEnabled(bool on);
  bool enabled() const
  {
    return m_enabled;
  }

  /// Core is up for this peer (or this machine is serving). Stops the watchdog.
  void noteSessionUp(const QString &deviceId);
  /// Core stopped. Schedules a backoff retry while the peer is still visible.
  void noteSessionDown(const QString &deviceId);

  /// Test seam. Empty restores the default 1s, 2s, 4s, 8s, 10s schedule.
  void setBackoffScheduleForTest(const QList<int> &milliseconds);

Q_SIGNALS:
  void autoConnectRequested(const litekvm::DiscoveredPeer &peer);

private Q_SLOTS:
  void onPeerSeen(const DiscoveredPeer &peer);
  void onPeerOffline(const QString &deviceId);

private:
  bool eligible(const DiscoveredPeer &peer) const;
  void requestConnect(const DiscoveredPeer &peer);
  void scheduleRetry(const QString &deviceId);
  void onRetry(const QString &deviceId);
  int backoffMs(const QString &deviceId) const;
  void stopTimer(const QString &deviceId);

  const DeviceIdentity &m_self;
  TrustStore &m_trust;
  DiscoveryService &m_discovery;
  bool m_enabled = false;
  QHash<QString, DiscoveredPeer> m_seen;
  QSet<QString> m_inflight;
  QSet<QString> m_up;
  QHash<QString, int> m_failCount;
  QHash<QString, QTimer *> m_retryTimers;
  QList<int> m_backoffMs;
};

} // namespace litekvm
