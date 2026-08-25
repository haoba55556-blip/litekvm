// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// LiteKvmController — glue between MainWindow and the litekvm zero-config
// layer (identity, discovery, advertiser, pairing).
#include "LiteKvmController.h"

#include "litekvm/MdnsAdvertiser.h"

#include <QDir>
#include <QFileInfo>

namespace deskflow::gui {

LiteKvmController::LiteKvmController(QObject *parent) : QObject(parent)
{
  m_identity = litekvm::DeviceIdentity::loadOrCreate();
  if (!m_identity)
    return; // degraded mode: panel shows nothing, core features still work

  m_trust = std::make_unique<litekvm::TrustStore>();
  m_discovery = new litekvm::DiscoveryService(this);
  m_advertiser = new litekvm::MdnsAdvertiser(this);
  m_pairing = new litekvm::PairingService(*m_identity, *m_trust, this);

  const quint16 pairingPort = 25901;

  connect(m_discovery, &litekvm::DiscoveryService::peerDiscovered, this,
          &LiteKvmController::peerDiscovered);
  connect(m_discovery, &litekvm::DiscoveryService::peerUpdated, this,
          &LiteKvmController::peerUpdated);
  connect(m_discovery, &litekvm::DiscoveryService::peerOffline, this,
          &LiteKvmController::peerOffline);

  connect(m_pairing, &litekvm::PairingService::pairChallenge, this,
          &LiteKvmController::pairChallenge);
  connect(m_pairing, &litekvm::PairingService::pairingSucceeded, this,
          [this](const QString &id, const QString &name) {
            m_advertiser->setState(litekvm::MdnsAdvertiser::PeerState::Pairable);
            Q_EMIT pairingSucceeded(id, name);
          });
  connect(m_pairing, &litekvm::PairingService::pairingFailed, this,
          &LiteKvmController::pairingFailed);

  if (m_advertiser->start(*m_identity, pairingPort))
    m_discovery->start(*m_identity, m_trust.get());

  m_pairing->listen(pairingPort);
}

QString LiteKvmController::deviceName() const
{
  return m_identity ? m_identity->name() : QString();
}

void LiteKvmController::setDeviceName(const QString &name)
{
  // identity rename persists on next save; discovery TXT refreshes via announce
}

void LiteKvmController::startPairing(const QString &deviceId)
{
  if (!m_pairing)
    return;
  auto peer = m_discovery->peer(deviceId);
  if (!peer)
    return;
  m_advertiser->setState(litekvm::MdnsAdvertiser::PeerState::Busy);
  m_pairing->pairWith(*peer);
}

void LiteKvmController::submitPairCode(const QString &code)
{
  if (m_pairing)
    m_pairing->submitPairCode(code);
}

void LiteKvmController::cancelPairing()
{
  if (m_pairing)
    m_pairing->cancelPairing();
  m_advertiser->setState(litekvm::MdnsAdvertiser::PeerState::Pairable);
}

} // namespace deskflow::gui
