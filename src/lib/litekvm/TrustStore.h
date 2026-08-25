// SPDX-License-Identifier: GPL-2.0
// LiteKVM trust store — persisted list of paired devices.
//
// Spec: docs/discovery-pairing-spec.md §6 (litekvm-design repo)
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <algorithm>
#include <optional>
#include <vector>

namespace litekvm {

struct TrustEntry {
  QString deviceId;
  QByteArray pubkey; // raw Ed25519 public key (32 bytes)
  QString name;
  QString platform;
  QDateTime pairedAt;
  QString lastAddr;
  QDateTime lastSeen;
};

/**
 * @brief JSON-persisted list of paired devices. Reconnect trusts device_id,
 * never IP addresses.
 */
class TrustStore {
public:
  explicit TrustStore(const QString &filePath = {});

  bool load();
  bool save() const;

  bool contains(const QString &deviceId) const;
  const TrustEntry *find(const QString &deviceId) const;
  void upsert(const TrustEntry &entry);
  bool remove(const QString &deviceId);
  const std::vector<TrustEntry> &entries() const;

private:
  QString m_path;
  std::vector<TrustEntry> m_entries;
};

} // namespace litekvm
