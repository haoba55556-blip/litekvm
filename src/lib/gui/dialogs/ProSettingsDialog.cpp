// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// ProSettingsDialog — LiteKVM Pro 设置（纯代码 UI）：
// 开机自动启动（litekvm::AutoStart）、自动连接已配对设备（controller
// 持久化键 litekvm/autoConnect）、关闭窗口最小化到托盘（gui/closeToTray）。
// 构造时从当前值初始化，accept 时写回。
#include "ProSettingsDialog.h"

#include "LiteKvmController.h"

#include "common/Settings.h"
#include "litekvm/AutoStart.h"
#include "litekvm/ElevatedCore.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
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

  // 关窗最小化到托盘（复用 Settings::Gui::CloseToTray 键，maybeHideToTray
  // 直接读这个键；默认 true 符合键鼠共享类工具的常驻预期）
  m_chkCloseToTray = new QCheckBox(tr("关闭窗口时最小化到托盘（保持连接）"), this);
  m_chkCloseToTray->setChecked(Settings::value(Settings::Gui::CloseToTray).toBool());

  // Highest-privileges scheduled task. One UAC prompt when the task is
  // created. This reaches admin windows on the user desktop. The dimmed UAC
  // secure desktop is a different desktop and still needs service mode.
  m_chkElevateCore = new QCheckBox(tr("以最高权限运行核心（可控制管理员窗口）"), this);
  m_chkElevateCore->setEnabled(litekvm::ElevatedCore::supported());
  m_chkElevateCore->setChecked(litekvm::ElevatedCore::isEnabled());
  m_chkElevateCore->setToolTip(tr(
      "通过计划任务以最高权限启动核心，这样能把键鼠送进「以管理员身份运行」的窗口。"
      "第一次打开会弹出一次 UAC。"
      "变暗的 UAC 安全桌面不在这个权限里，那种画面需要用 Windows 服务模式。"));

  auto *formLayout = new QFormLayout;
  formLayout->addRow(m_chkAutoStart);
  formLayout->addRow(m_chkAutoConnect);
  formLayout->addRow(m_chkCloseToTray);
  formLayout->addRow(m_chkElevateCore);

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

  Settings::setValue(Settings::Gui::CloseToTray, m_chkCloseToTray->isChecked());
  litekvm::ElevatedCore::setEnabled(m_chkElevateCore->isChecked());

  QDialog::accept();
}

} // namespace deskflow::gui
