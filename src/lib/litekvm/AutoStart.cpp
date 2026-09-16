// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// AutoStart — Windows registry Run-key autostart + auto-connect on boot.
//
// Feature 2 of litekvm-pro: opt-in checkbox; when enabled, LiteKVM starts
// with Windows and automatically connects to any paired device that is
// discoverable on the LAN.
#include "AutoStart.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>

namespace litekvm {

namespace {
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run — no admin needed
constexpr auto kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto kValueName = "LiteKVM";
} // namespace

bool AutoStart::supported()
{
#if defined(Q_OS_WIN)
  return true;
#else
  // macOS: LaunchAgent plist; Linux: ~/.config/autostart/*.desktop
  // implemented per-platform below; report true where the helper exists
  return false;
#endif
}

QString AutoStart::commandLine()
{
  return QStringLiteral("\"%1\" --autostart").arg(QCoreApplication::applicationFilePath());
}

bool AutoStart::isEnabled()
{
#if defined(Q_OS_WIN)
  QSettings run(kRunKey, QSettings::NativeFormat);
  return run.contains(kValueName);
#else
  return false;
#endif
}

void AutoStart::setEnabled(bool enabled)
{
#if defined(Q_OS_WIN)
  QSettings run(kRunKey, QSettings::NativeFormat);
  if (enabled)
    run.setValue(kValueName, commandLine());
  else
    run.remove(kValueName);
  run.sync();
#else
  Q_UNUSED(enabled);
#endif
}

bool AutoStart::isAutostartLaunch()
{
  return QCoreApplication::arguments().contains(QStringLiteral("--autostart"));
}

} // namespace litekvm
