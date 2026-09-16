/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2024 - 2026 Chris Rizzitello <sithord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QProcess>
#include <QRegularExpression>
#include <QSystemTrayIcon>
#include <QUrl>

#include "VersionChecker.h"
#include "config/ServerConfig.h"
#include "gui/core/CoreProcess.h"
#include "gui/core/NetworkMonitor.h"
#include "net/Fingerprint.h"

#ifdef Q_OS_MACOS
#include "gui/OSXHelpers.h"
#endif

class QAction;
class QMenu;
class QLocalServer;

class DeskflowApplication;
class LogDock;
class StatusBar;

namespace Ui {
class MainWindow;
}

namespace deskflow::gui {
class ClipTransferDialog;
class CollapsiblePanel;
class LiteKvmController;
class NearbyPanel;
namespace ipc {
class DaemonIpcClient;
}
} // namespace deskflow::gui

class MainWindow : public QMainWindow
{
  using ConnectionState = deskflow::core::ConnectionState;
  using CoreMode = Settings::CoreMode;
  using CoreProcess = deskflow::gui::CoreProcess;
  using NetworkMonitor = deskflow::gui::NetworkMonitor;
  using ProcessState = deskflow::core::ProcessState;

  Q_OBJECT

public:
  enum class LogLevel
  {
    Error,
    Info
  };

public:
  explicit MainWindow();
  ~MainWindow() override;

  [[nodiscard]] CoreMode coreMode() const
  {
    return m_coreProcess.mode();
  }
  void open();
  ServerConfig &serverConfig()
  {
    return m_serverConfig;
  }

  void hide();

protected:
  void changeEvent(QEvent *e) override;
  bool eventFilter(QObject *obj, QEvent *event) override;

private:
  /**
   * @brief updateText Update all text not in the UI
   */
  void updateText();
  void toggleLogVisible(bool visible);

  void settingsChanged(const QString &key = QString());
  void serverConfigSaving();
  void coreProcessError(CoreProcess::Error error);
  void coreConnectionStateChanged(ConnectionState state);
  void coreProcessStateChanged(ProcessState state);

  void trayIconActivated(QSystemTrayIcon::ActivationReason reason);
  void serverConnectionConfigureClient(const QString &clientName);

  void clearSettings();
  void openAboutDialog();
  void openGetNewVersionUrl() const;
  void openSettings();
  void openProSettings();
  void startCore();
  void stopCore();
  bool saveServerConfig();
  void resetCore();

  void showMyFingerprint();
  void updateSecurityIcon(bool visible);
  void updateNetworkInfo();

  void coreModeToggled(bool checked);
  void updateModeControls();
  void updateModeControlLabels();
  std::unique_ptr<Ui::MainWindow> ui;

  void createMenuBar();
  void setupTrayIcon();
  void applyConfig();
  void setTrayIcon();
  void handleUnrecognisedClient(const QString &clientName);
  void handleConnectionRefused(deskflow::core::ConnectionRefusal reason);
  void handlePeerFingerprint(const QString &fingerprint);
  void handleMissingKeyboardLayouts(const QString &layouts);
  void closeEvent(QCloseEvent *event) override;
  bool maybeHideToTray();
  void secureSocket(bool secureSocket);
  void connectSlots();
  void handleLogLine(const QString &line);
  void updateFingerprintButton();
  void updateScreenName();
  void reworkPanels();
  void saveSettings() const;
  void showConfigureServer(const QString &message);
  void showConfigureClient();
  void restoreWindow();
  void setupControls();
  void showFirstConnectedMessage();
  void updateStatus();
  void showAndActivate();
  void showHostNameEditor();
  void setHostName();
  void daemonIpcClientConnectionFailed();
  void toggleCanRunCore(bool enableButtons);
  void remoteHostChanged(const QString &newRemoteHost);
  void updateIpLabel(const QStringList &addresses);
  void updateTimeoutDelay(int newDelay);
  void setHelpFilePath();
  void showHelpViewer() const;

  bool canRunCore() const;

  // ---- 跨机文件剪贴板 ----
  void setupFileClipboard();
  void applyFileClipboardSettings();
  void requestSendFiles();
  void sendFiles(const QStringList &paths, bool fromClipboard);
  void onLocalFilesCopied(const QStringList &paths);
  void onFileOfferReceived(const QString &peerName, const QStringList &fileNames, qint64 totalBytes, int fileCount);
  void onFileProgressChanged(quint64 bytesDone, quint64 bytesTotal, bool outgoing);
  void onFileTransferCompleted(const QStringList &localPaths, bool outgoing, const QString &transferId);
  void onFileTransferFailed(const QString &message, bool outgoing);
  void onFileTransferCancelled(bool outgoing);
  void onFileClipboardError(const QString &message);
  void beginTransferUi(bool outgoing, const QString &peerName, quint64 totalBytes);
  void finishTransferUi(const QString &message, bool failed);
  void cancelTransferUi(const QString &message);
  void notifyUser(const QString &message);
  static qint64 localFilesSize(const QStringList &paths);

  /**
   * @brief trustedFingerprintDatabase get the FingerprintDatabase for the trusted clients or trusted servers.
   * @return The path to the trusted fingerprint file
   */
  QString trustedFingerprintDatabase() const;

  /**
   * @brief generateCertificate Generate a new certificate
   * @return true when successful
   */
  bool generateCertificate();

  Fingerprint m_fingerprint;

  void serverClientsChanged(const QStringList &clients);

  inline static const auto m_guiSocketName = QStringLiteral("deskflow-gui");
  inline static const auto m_nameRegEx = QRegularExpression(QStringLiteral("^[\\w\\-_\\.]{0,255}$"));

  VersionChecker m_versionChecker;
  bool m_secureSocket = false;
  bool m_saveOnExit = true;
  bool m_clientErrorVisible = false;
  ServerConfig m_serverConfig;
  deskflow::gui::CoreProcess m_coreProcess;
  QSet<QString> m_ignoredClients;
  bool m_newClientPromptShowing = false;
  bool m_serverConfigDialogVisible = false;
  QSize m_expandedSize = QSize();
  QStringList m_checkedClients;
  QStringList m_checkedServers;
  QSystemTrayIcon *m_trayIcon = nullptr;
  QLocalServer *m_guiDupeChecker = nullptr;
  deskflow::gui::ipc::DaemonIpcClient *m_daemonIpcClient = nullptr;

  LogDock *m_logDock;
  deskflow::gui::CollapsiblePanel *m_nearbyCollapsible = nullptr;
  deskflow::gui::CollapsiblePanel *m_logCollapsible = nullptr;
  StatusBar *m_statusBar = nullptr;
  deskflow::gui::LiteKvmController *m_liteKvmController = nullptr;

  // 跨机文件剪贴板
  deskflow::gui::ClipTransferDialog *m_clipTransferDialog = nullptr;
  bool m_fileClipboardEnabled = false;
  bool m_filePromptShowing = false;
  bool m_clipTransferOutgoing = false;
  QString m_clipTransferPeer;
  qint64 m_clipTransferTotalBytes = 0;
  QString m_lastCopiedPaths;
  QElapsedTimer m_lastCopiedAt;

  // Window Menu
  QMenu *m_menuFile = nullptr;
  QMenu *m_menuEdit = nullptr;
  QMenu *m_menuView = nullptr;
  QMenu *m_menuHelp = nullptr;

  // Window Actions
  QAction *m_actionAbout = nullptr;
  QAction *m_actionMinimize = nullptr;
  QAction *m_actionQuit = nullptr;
  QAction *m_actionTrayQuit = nullptr;
  QAction *m_actionRestore = nullptr;
  QAction *m_actionSettings = nullptr;
  QAction *m_actionProSettings = nullptr;
  QAction *m_actionStartCore = nullptr;
  QAction *m_actionRestartCore = nullptr;
  QAction *m_actionStopCore = nullptr;
  QAction *m_actionShowHelp = nullptr;
  QAction *m_actionSendFiles = nullptr;

  // Network monitoring
  NetworkMonitor *m_networkMonitor = nullptr;
  QString m_currentIpAddress;

  // Server IP strategy optimization
  QStringList m_serverStartIPs;
  QString m_serverStartSuggestedIP;
  QUrl m_helpPath;
};
