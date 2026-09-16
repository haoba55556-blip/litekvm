// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// ProSettingsDialog — LiteKVM Pro 设置（纯代码 UI）：
// 开机自动启动（litekvm::AutoStart）、自动连接已配对设备（controller
// 持久化键 litekvm/autoConnect）、中继地址/房间令牌（litekvm/relayUrl、
// litekvm/relayRoom）。构造时从当前值初始化，accept 时写回。
#include "ProSettingsDialog.h"

#include "LiteKvmController.h"

#include "common/Settings.h"
#include "litekvm/AutoStart.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

namespace deskflow::gui {

ProSettingsDialog::ProSettingsDialog(LiteKvmController *controller, QWidget *parent)
  : QDialog(parent),
    m_controller(controller)
{
  setWindowTitle(tr("LiteKVM Pro 设置"));
  setModal(true);

  m_chkAutoStart = new QCheckBox(tr("开机自动启动（启动后隐藏到托盘）"), this);
  m_chkAutoStart->setEnabled(litekvm::AutoStart::supported());
  m_chkAutoStart->setChecked(litekvm::AutoStart::isEnabled());

  m_chkAutoConnect = new QCheckBox(tr("自动连接已配对设备"), this);
  if (m_controller)
    m_chkAutoConnect->setChecked(m_controller->isAutoConnectEnabled());

  m_editRelayUrl = new QLineEdit(Settings::value(QStringLiteral("litekvm/relayUrl")).toString(), this);
  m_editRelayUrl->setPlaceholderText(tr("中继服务器地址，如 wss://relay.example.com:443"));

  m_editRelayRoom = new QLineEdit(Settings::value(QStringLiteral("litekvm/relayRoom")).toString(), this);
  m_editRelayRoom->setPlaceholderText(tr("房间令牌（可选）"));

  auto *formLayout = new QFormLayout;
  formLayout->addRow(m_chkAutoStart);
  formLayout->addRow(m_chkAutoConnect);
  formLayout->addRow(tr("中继地址："), m_editRelayUrl);
  formLayout->addRow(tr("房间令牌："), m_editRelayRoom);

  auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttonBox, &QDialogButtonBox::accepted, this, &ProSettingsDialog::accept);
  connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->addLayout(formLayout);
  mainLayout->addWidget(buttonBox);
}

void ProSettingsDialog::accept()
{
  litekvm::AutoStart::setEnabled(m_chkAutoStart->isChecked());

  // controller 为空时（理论上不会发生）直接写键，保持行为一致
  if (m_controller)
    m_controller->setAutoConnectEnabled(m_chkAutoConnect->isChecked());
  else
    Settings::setValue(QStringLiteral("litekvm/autoConnect"), m_chkAutoConnect->isChecked());

  Settings::setValue(QStringLiteral("litekvm/relayUrl"), m_editRelayUrl->text().trimmed());
  Settings::setValue(QStringLiteral("litekvm/relayRoom"), m_editRelayRoom->text().trimmed());

  QDialog::accept();
}

} // namespace deskflow::gui
