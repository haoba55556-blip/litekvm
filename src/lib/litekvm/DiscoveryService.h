// SPDX-License-Identifier: GPL-2.0
// LiteKVM mDNS discovery — browse for nearby peers via mDNS.
//
// Qt 6.8 ships QDnsLookup (unicast DNS) but no mDNS browse either, so both
// directions (announce + browse) are implemented in this module over raw
// multicast DNS on UDP/5353:
//  - MdnsAdvertiser: announces our PTR/SRV/TXT records
//  - DiscoveryService: listens for queries from peers and their announcements,
//    maintaining the live peer list
//
// Spec: docs/discovery-pairing-spec.md §3 (litekvm-design repo)
#pragma once

#include "DeviceIdentity.h"
#include "TrustStore.h"

#include <QMap>
#include <QObject>

#include <map>
#include <optional>
#include <vector>

namespace litekvm {

struct DiscoveredPeer {
  enum class State { Pairable, Paired, Busy };

  QString deviceId;
  QString name;
  QString platform;
  QString fingerprint; // 16 hex chars
  QString host;        // IP or hostname
  uint16_t port = 0;   // pairing port (25901)
  State state = State::Pairable;

  static State stateFromString(const QString &s)
  {
    if (s == QLatin1String("pairable"))
      return State::Pairable;
    if (s == QLatin1String("paired"))
      return State::Paired;
    return State::Busy;
  }
};

/**
 * @brief Maintains the live list of nearby LiteKVM devices.
 *
 * Listens on multicast UDP/5353 for announcements and query responses.
 * Multi-NIC duplicates are deduped by device_id (last record wins).
 * TrustStore (when provided) upgrades display state of paired devices.
 */
class DiscoveryService : public QObject {
  Q_OBJECT

public:
  static const QString kServiceType; // "_litekvm._tcp.local."

  explicit DiscoveryService(QObject *parent = nullptr);
  ~DiscoveryService() override;

  /// begin listening for mDNS traffic
  bool start(const DeviceIdentity &self, const TrustStore *trust = nullptr);
  void stop();

  std::vector<DiscoveredPeer> peers() const;
  std::optional<DiscoveredPeer> peer(const QString &deviceId) const;

Q_SIGNALS:
  void peerDiscovered(const litekvm::DiscoveredPeer &peer);
  void peerUpdated(const litekvm::DiscoveredPeer &peer);
  void peerOffline(const QString &deviceId);

private:
  void ingestTxtRecord(const QMap<QString, QString> &txt, const QString &host, uint16_t port,
                       bool announce);
  void pruneStale();

  const DeviceIdentity *m_self = nullptr;
  const TrustStore *m_trust = nullptr;
  std::map<QString, DiscoveredPeer> m_peers;   // key: device_id
  std::map<QString, qint64> m_lastSeenMs;      // device_id -> monotonic ms
};

} // namespace litekvm
