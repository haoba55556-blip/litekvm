// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#include "ElevatedCore.h"

#include "common/Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#if defined(Q_OS_WIN)
#include <shellapi.h>
#include <windows.h>
#endif

namespace litekvm {

namespace {

QString scriptPath()
{
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return dir + QStringLiteral("/litekvm-core-launch.cmd");
}

int runSchtasks(const QStringList &args, QString *error)
{
  QProcess process;
  process.start(QStringLiteral("schtasks"), args);
  if (!process.waitForStarted(5000)) {
    if (error)
      *error = QStringLiteral("schtasks did not start");
    return -1;
  }
  process.waitForFinished(20000);
  if (error)
    *error = QString::fromLocal8Bit(process.readAllStandardError());
  return process.exitCode();
}

#if defined(Q_OS_WIN)
bool runSchtasksElevated(const QString &parametersText, QString *error)
{
  const std::wstring parameters = parametersText.toStdWString();
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = L"schtasks.exe";
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) {
    if (error)
      *error = QStringLiteral("UAC consent was not granted");
    return false;
  }
  WaitForSingleObject(info.hProcess, 30000);
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  CloseHandle(info.hProcess);
  return code == 0;
}
#endif

} // namespace

bool ElevatedCore::supported()
{
#if defined(Q_OS_WIN)
  return true;
#else
  return false;
#endif
}

bool ElevatedCore::isEnabled()
{
  return Settings::value(QStringLiteral("litekvm/elevateCore")).toBool();
}

void ElevatedCore::setEnabled(bool enabled)
{
  Settings::setValue(QStringLiteral("litekvm/elevateCore"), enabled);
  if (!enabled && supported())
    runSchtasks(deleteTaskArguments(QLatin1String(kTaskName)), nullptr);
}

QString ElevatedCore::launcherScript(const QString &exe, const QStringList &args)
{
  QString body = QStringLiteral("@echo off\r\n\"%1\"").arg(exe);
  for (const QString &arg : args) {
    body += QLatin1Char(' ');
    if (arg.contains(QLatin1Char(' ')) || arg.contains(QLatin1Char('"')))
      body += QLatin1Char('"') + QString(arg).replace(QLatin1Char('"'), QStringLiteral("\\\"")) + QLatin1Char('"');
    else
      body += arg;
  }
  body += QStringLiteral("\r\n");
  return body;
}

QStringList ElevatedCore::createTaskArguments(const QString &taskName, const QString &scriptPath)
{
  // /IT: only while the user is logged on, so the core shares the interactive
  // desktop. /RL HIGHEST: elevated. The secure desktop is still out of reach.
  const QString action = QStringLiteral("cmd.exe /c \"%1\"").arg(scriptPath);
  return {QStringLiteral("/Create"), QStringLiteral("/F"), QStringLiteral("/TN"), taskName,
          QStringLiteral("/SC"),      QStringLiteral("ONCE"), QStringLiteral("/ST"), QStringLiteral("00:00"),
          QStringLiteral("/RL"),      QStringLiteral("HIGHEST"), QStringLiteral("/IT"), QStringLiteral("/TR"),
          action};
}

QStringList ElevatedCore::runTaskArguments(const QString &taskName)
{
  return {QStringLiteral("/Run"), QStringLiteral("/TN"), taskName};
}

QStringList ElevatedCore::endTaskArguments(const QString &taskName)
{
  return {QStringLiteral("/End"), QStringLiteral("/TN"), taskName};
}

QStringList ElevatedCore::deleteTaskArguments(const QString &taskName)
{
  return {QStringLiteral("/Delete"), QStringLiteral("/F"), QStringLiteral("/TN"), taskName};
}

bool ElevatedCore::launch(const QString &exe, const QStringList &args, QString *error)
{
  if (!supported()) {
    if (error)
      *error = QStringLiteral("elevated core launch is only available on Windows");
    return false;
  }

  const QString path = scriptPath();
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (error)
      *error = QStringLiteral("cannot write %1").arg(path);
    return false;
  }
  file.write(launcherScript(exe, args).toUtf8());
  file.close();

  // The task already points at this script, so a later start only rewrites
  // the script and /Run. /Create (which needs consent) runs when /Run fails
  // because the task is not registered yet.
  if (runSchtasks(runTaskArguments(QLatin1String(kTaskName)), nullptr) == 0)
    return true;

  QString createError;
  if (runSchtasks(createTaskArguments(QLatin1String(kTaskName), path), &createError) != 0) {
#if defined(Q_OS_WIN)
    // Registering /RL HIGHEST from a normal process is denied. One UAC prompt
    // creates the task; later /Run calls do not ask again.
    const QString elevated = QStringLiteral(
        "/Create /F /TN %1 /SC ONCE /ST 00:00 /RL HIGHEST /IT /TR \"cmd.exe /c \\\"%2\\\"\"")
                                 .arg(QLatin1String(kTaskName), path);
    if (!runSchtasksElevated(elevated, &createError)) {
      if (error)
        *error = createError.isEmpty() ? QStringLiteral("schtasks /Create failed") : createError;
      return false;
    }
#else
    if (error)
      *error = createError;
    return false;
#endif
  }
  if (runSchtasks(runTaskArguments(QLatin1String(kTaskName)), error) != 0)
    return false;
  return true;
}

bool ElevatedCore::endRunningTask()
{
  if (!supported())
    return false;
  return runSchtasks(endTaskArguments(QLatin1String(kTaskName)), nullptr) == 0;
}

} // namespace litekvm
