// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#pragma once

#include <QString>

namespace litekvm {

/**
 * @brief Opt-in launch-at-boot via HKCU Run key (no admin required).
 * When launched with --autostart the GUI hides to tray and auto-connects
 * to any paired peer discovered on the LAN.
 */
class AutoStart {
public:
  static bool supported();
  static bool isEnabled();
  static void setEnabled(bool enabled);
  static bool isAutostartLaunch();

private:
  static QString commandLine();
};

} // namespace litekvm
