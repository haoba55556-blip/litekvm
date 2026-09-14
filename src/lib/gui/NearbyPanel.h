// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#pragma once

#include "litekvm/DiscoveryService.h"
#include "litekvm/PairingService.h"

#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>

namespace litekvm {
class DiscoveryService;
class PairingService;
} // namespace litekvm

namespace deskflow::gui {

class LiteKvmController;

/**
 * @brief "附近的电脑" side panel — live peer list + pairing flow UI.
 * The .ui file promotes a plain QWidget to this class; the controller is
 * attached by MainWindow right after construction (setController).
 */
class NearbyPanel : public QWidget {
  Q_OBJECT

public:
  explicit NearbyPanel(QWidget *parent = nullptr);
  void setController(LiteKvmController *controller);

  void setShowTitle(bool show) { if (m_title) m_title->setVisible(show); }

Q_SIGNALS:
  /// 用户点击「连接」/双击已配对设备 → 请求 MainWindow 以该设备为主机启动客户端。
  void connectToPeer(const QString &deviceId);

private:
  void refreshRow(const litekvm::DiscoveredPeer &peer);
  QString selectedDeviceId() const;
  litekvm::DiscoveredPeer::State currentPeerState() const;

private Q_SLOTS:
  void onPairClicked();
  void onPairChallenge(const QString &peerName, const QString &fingerprint, const QString &code);
  void onPairingSucceeded(const QString &deviceId, const QString &name);
  void onPairingFailed(litekvm::PairingService::Error error);

private:
  LiteKvmController *m_controller = nullptr;
  QTableWidget *m_table = nullptr;
  QPushButton *m_btnPair = nullptr;
  QPushButton *m_btnConnect = nullptr;
  QLabel *m_title = nullptr;
  QLabel *m_status = nullptr;
  QHash<QString, litekvm::DiscoveredPeer::State> m_peerStates;
};

} // namespace deskflow::gui

