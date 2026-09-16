// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#pragma once

#include <QDialog>

class QCheckBox;

namespace deskflow::gui {

class LiteKvmController;

/**
 * @brief LiteKVM Pro 设置对话框（纯代码构建 UI，无 .ui 文件）。
 * 包含：开机自动启动（litekvm::AutoStart）、自动连接已配对设备
 * （controller 持久化键 litekvm/autoConnect）。构造时从当前值初始化，
 * accept 时写回。
 */
class ProSettingsDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ProSettingsDialog(LiteKvmController *controller, QWidget *parent = nullptr);

public Q_SLOTS:
  void accept() override;

private:
  LiteKvmController *m_controller = nullptr;
  QCheckBox *m_chkAutoStart = nullptr;
  QCheckBox *m_chkAutoConnect = nullptr;
};

} // namespace deskflow::gui
