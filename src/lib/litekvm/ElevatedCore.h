// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Optional "run the core at highest privileges" via a scheduled task.
//
// An elevated interactive process can inject input into windows that were
// started with "Run as administrator". It cannot inject into the UAC secure
// desktop (the dimmed consent screen). That desktop is isolated; only a
// service running as SYSTEM (LiteKVM daemon / service mode) can reach it.
#pragma once

#include <QString>
#include <QStringList>

namespace litekvm {

class ElevatedCore
{
public:
  static constexpr auto kTaskName = "LiteKVMCore";

  static bool supported();
  static bool isEnabled();
  static void setEnabled(bool enabled);

  /// cmd body the scheduled task runs. The GUI rewrites this before each
  /// /Run so server/client mode can change without recreating the task.
  static QString launcherScript(const QString &exe, const QStringList &args);

  /// schtasks /Create … /RL HIGHEST /IT. One UAC consent when the task is
  /// first registered; later starts only /Run it.
  static QStringList createTaskArguments(const QString &taskName, const QString &scriptPath);
  static QStringList runTaskArguments(const QString &taskName);
  static QStringList endTaskArguments(const QString &taskName);
  static QStringList deleteTaskArguments(const QString &taskName);

  /// Register (if needed), write the launcher, and start the task.
  /// False when the platform has no Task Scheduler or schtasks fails.
  static bool launch(const QString &exe, const QStringList &args, QString *error = nullptr);
  static bool endRunningTask();
};

} // namespace litekvm
