// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// LiteKVM trust store — persisted list of paired devices.
//
// Spec: docs/discovery-pairing-spec.md §6 (litekvm-design repo)
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QMutex>
#include <QString>

#include <algorithm>
#include <optional>
#include <vector>

namespace litekvm {

struct TrustEntry
{
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
 *
 * Methods take an internal lock. The 25903 worker thread reads the store while
 * the GUI thread updates it, so find() and entries() return copies.
 */
class TrustStore
{
public:
  explicit TrustStore(const QString &filePath = {});

  bool load();
  bool save() const;

  bool contains(const QString &deviceId) const;
  /// Engaged copy when @p deviceId is paired. Empty otherwise.
  std::optional<TrustEntry> find(const QString &deviceId) const;
  void upsert(const TrustEntry &entry);
  /// Write lastAddr when it actually changed. Returns true if the file was updated.
  bool updateLastAddr(const QString &deviceId, const QString &addr);
  bool remove(const QString &deviceId);
  /// Snapshot. Safe to iterate while another thread calls upsert().
  std::vector<TrustEntry> entries() const;

private:
  bool saveUnlocked() const;

  QString m_path;
  std::vector<TrustEntry> m_entries;
  mutable QMutex m_mutex;
};

} // namespace litekvm
