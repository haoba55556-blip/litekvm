// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#pragma once

#include "litekvm/DeviceIdentity.h"
#include "litekvm/DiscoveryService.h"
#include "litekvm/MdnsAdvertiser.h"
#include "litekvm/PairingService.h"
#include "litekvm/TrustStore.h"

#include <QObject>
#include <QSet>

namespace deskflow::gui {

/**
 * @brief Owns the litekvm zero-config stack and exposes Qt signals for the UI.
 * Instantiated once by MainWindow; all signals are cross-thread safe (queued).
 */
class LiteKvmController : public QObject {
  Q_OBJECT

public:
  explicit LiteKvmController(QObject *parent = nullptr);

  QString deviceName() const;
  void setDeviceName(const QString &name);
  bool ready() const
  {
    return m_identity.has_value();
  }

public Q_SLOTS:
  void startPairing(const QString &deviceId);
  void submitPairCode(const QString &code);
  void cancelPairing();

Q_SIGNALS:
  void peerDiscovered(const litekvm::DiscoveredPeer &peer);
  void peerUpdated(const litekvm::DiscoveredPeer &peer);
  void peerOffline(const QString &deviceId);
  void pairChallenge(const QString &peerName, const QString &peerFingerprint, const QString &expectedCode);
  void pairingSucceeded(const QString &deviceId, const QString &name);
  void pairingFailed(litekvm::PairingService::Error error);

private:
  std::optional<litekvm::DeviceIdentity> m_identity;
  std::unique_ptr<litekvm::TrustStore> m_trust;
  litekvm::DiscoveryService *m_discovery = nullptr;
  litekvm::MdnsAdvertiser *m_advertiser = nullptr;
  litekvm::PairingService *m_pairing = nullptr;
};

} // namespace deskflow::gui
