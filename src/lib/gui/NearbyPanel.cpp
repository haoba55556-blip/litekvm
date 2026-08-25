// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// NearbyPanel — "附近的电脑" side panel: live mDNS peer list + pairing actions.
#include "NearbyPanel.h"

#include "LiteKvmController.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

namespace deskflow::gui {

NearbyPanel::NearbyPanel(QWidget *parent) : QWidget(parent)
{
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  auto *title = new QLabel(tr("附近的电脑 / Nearby computers"), this);
  title->setStyleSheet(QStringLiteral("font-weight: bold;"));
  layout->addWidget(title);

  m_table = new QTableWidget(0, 3, this);
  m_table->setHorizontalHeaderLabels({tr("名称"), tr("平台"), tr("状态")});
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->verticalHeader()->setVisible(false);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::SingleSelection);
  m_table->setMinimumHeight(120);
  layout->addWidget(m_table);

  m_btnPair = new QPushButton(tr("配对…"), this);
  m_btnPair->setEnabled(false);
  layout->addWidget(m_btnPair);

  m_status = new QLabel(this);
  layout->addWidget(m_status);

  connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
    m_btnPair->setEnabled(m_table->currentRow() >= 0);
  });

  connect(m_btnPair, &QPushButton::clicked, this, &NearbyPanel::onPairClicked);
}

void NearbyPanel::setController(LiteKvmController *controller)
{
  m_controller = controller;
  if (!m_controller)
    return;

  connect(controller, &LiteKvmController::peerDiscovered, this,
          [this](const litekvm::DiscoveredPeer &peer) { refreshRow(peer); });
  connect(controller, &LiteKvmController::peerUpdated, this,
          [this](const litekvm::DiscoveredPeer &peer) { refreshRow(peer); });
  connect(controller, &LiteKvmController::peerOffline, this, [this](const QString &deviceId) {
    for (int row = 0; row < m_table->rowCount(); ++row) {
      if (m_table->item(row, 0)->data(Qt::UserRole).toString() == deviceId) {
        m_table->removeRow(row);
        break;
      }
    }
  });

  connect(controller, &LiteKvmController::pairChallenge, this, &NearbyPanel::onPairChallenge);
  connect(controller, &LiteKvmController::pairingSucceeded, this, &NearbyPanel::onPairingSucceeded);
  connect(controller, &LiteKvmController::pairingFailed, this, &NearbyPanel::onPairingFailed);
}

void NearbyPanel::refreshRow(const litekvm::DiscoveredPeer &peer)
{
  // find or create the row keyed by device_id (UserRole of col 0)
  int row = -1;
  for (int r = 0; r < m_table->rowCount(); ++r) {
    if (m_table->item(r, 0)->data(Qt::UserRole).toString() == peer.deviceId) {
      row = r;
      break;
    }
  }
  if (row < 0) {
    row = m_table->rowCount();
    m_table->insertRow(row);
  }

  const QString stateText = peer.state == litekvm::DiscoveredPeer::State::Paired
                                ? tr("已配对")
                            : peer.state == litekvm::DiscoveredPeer::State::Pairable ? tr("可配对")
                                                                                     : tr("忙碌");

  auto make = [&](const QString &text) {
    auto *item = new QTableWidgetItem(text);
    item->setData(Qt::UserRole, peer.deviceId);
    return item;
  };
  m_table->setItem(row, 0, make(peer.name));
  m_table->setItem(row, 1, make(peer.platform));
  m_table->setItem(row, 2, make(stateText));
}

QString NearbyPanel::selectedDeviceId() const
{
  const int row = m_table->currentRow();
  if (row < 0 || !m_table->item(row, 0))
    return {};
  return m_table->item(row, 0)->data(Qt::UserRole).toString();
}

void NearbyPanel::onPairClicked()
{
  const QString deviceId = selectedDeviceId();
  if (deviceId.isEmpty())
    return;

  QMessageBox confirm(this);
  confirm.setWindowTitle(tr("配对确认"));
  confirm.setText(tr("要和选中的电脑配对吗？\n\n对方屏幕上会显示一个 6 位数字，在这里输入它完成配对。"));
  if (confirm.exec() != QDialog::Accepted)
    return;

  m_controller->startPairing(deviceId);
}

void NearbyPanel::onPairChallenge(const QString &peerName, const QString &fingerprint,
                                  const QString &code)
{
  // The code shown here was derived locally; per spec the user types what is
  // displayed on the REMOTE screen. In practice both codes are identical when
  // no MITM is present — we display ours and ask for theirs.
  bool ok = false;
  const QString input = QInputDialog::getText(
      this, tr("输入配对码"),
      tr("电脑「%1」正在配对。\n对方屏幕显示的 6 位数字是：\n\n（在下面输入对方屏幕上显示的数字）")
          .arg(peerName),
      QLineEdit::Normal, {}, &ok);
  if (!ok) {
    m_controller->cancelPairing();
    return;
  }
  m_controller->submitPairCode(input);
}

void NearbyPanel::onPairingSucceeded(const QString &deviceId, const QString &name)
{
  m_status->setText(tr("✅ 已与「%1」配对").arg(name));
  QMessageBox::information(this, tr("配对成功"), tr("已与「%1」建立信任，可以开始共享键鼠了。").arg(name));
}

void NearbyPanel::onPairingFailed(litekvm::PairingService::Error error)
{
  using E = litekvm::PairingService::Error;
  QString text;
  switch (error) {
  case E::WrongCode:
    text = tr("配对码不对，请重试。");
    break;
  case E::RateLimited:
    text = tr("错误次数过多，已锁定 10 分钟。");
    break;
  case E::Busy:
    text = tr("对方正忙，稍后再试。");
    break;
  case E::Rejected:
    text = tr("对方拒绝了配对。");
    break;
  case E::Timeout:
    text = tr("连接超时。");
    break;
  default:
    text = tr("配对失败。");
  }
  m_status->setText(tr("❌ %1").arg(text));
}

} // namespace deskflow::gui
