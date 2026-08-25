// SPDX-License-Identifier: GPL-2.0
// LiteKVM device identity — Ed25519 keypair, persistent device_id, fingerprint.
//
// Spec: docs/discovery-pairing-spec.md §2 (litekvm-design repo)
#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>

namespace litekvm {

/**
 * @brief Persistent per-machine identity: Ed25519 keypair + derived device_id.
 *
 * - device_id = hex(SHA256(pubkey)[0..16]) — 32 chars, stable for life.
 * - fingerprint = hex(SHA256(pubkey)[0..8]) — shown to user during pairing.
 * - Stored as JSON (pub/priv base64) under the app data dir.
 */
class DeviceIdentity {
public:
  static std::optional<DeviceIdentity> loadOrCreate(const QString &storageFileOverride = {});

  const std::array<uint8_t, 32> &publicKey() const;
  QString deviceId() const;
  QString fingerprint() const;
  QString name() const;
  void setName(const QString &name);

  static QString deviceIdFromPubkey(const uint8_t *pub, size_t len);
  static QString fingerprintFromPubkey(const uint8_t *pub, size_t len);

private:
  std::array<uint8_t, 32> m_pub{};
  std::array<uint8_t, 64> m_priv{}; // seed in first half
  QString m_deviceId;
  QString m_name = QStringLiteral("this-device");
};

} // namespace litekvm
