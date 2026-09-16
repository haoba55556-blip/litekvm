// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#pragma once

#include "litekvm/AutoConnect.h"
#include "litekvm/DeviceIdentity.h"
#include "litekvm/DiscoveryService.h"
#include "litekvm/MdnsAdvertiser.h"
#include "litekvm/PairingService.h"
#include "litekvm/TrustStore.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <optional>

namespace litekvm {
class ClipFileService;
class ClipboardFileBridge;
} // namespace litekvm

namespace deskflow::gui {

/**
 * @brief Owns the litekvm zero-config stack and exposes Qt signals for the UI.
 * Instantiated once by MainWindow; all signals are cross-thread safe (queued).
 */
class LiteKvmController : public QObject
{
  Q_OBJECT

public:
  /// 可以作为文件剪贴板发送目标的对端（已配对）。
  struct ClipTarget
  {
    QString deviceId;
    QString name;
    QString platform;
    QString host;      ///< IP / 主机名，空表示当前不可达
    quint16 port = 0;  ///< 文件剪贴板端口
    bool online = false;
  };

  explicit LiteKvmController(QObject *parent = nullptr);

  QString deviceName() const;
  void setDeviceName(const QString &name);
  bool ready() const
  {
    return m_identity.has_value();
  }

  /// 文件剪贴板控制端口（litekvm::ClipFileService::defaultPort()，TCP 25903）
  static quint16 clipFilePort();

  // ---------------------------------------------------------- 文件剪贴板
  [[nodiscard]] bool fileClipboardEnabled() const
  {
    return m_fileClipboardEnabled;
  }
  /**
   * @brief 启用/禁用跨机文件剪贴板。
   * 禁用时既不向 UI 上报本地复制的文件，也会静默拒绝对端发来的 offer。
   */
  void setFileClipboardEnabled(bool enabled);

  /// 单次传输上限（字节），<= 0 表示不限制。
  void setMaxTransferBytes(qint64 bytes);
  [[nodiscard]] qint64 maxTransferBytes() const
  {
    return m_maxTransferBytes;
  }

  /// 已配对的对端（在线的排在前面）；没有已配对设备时返回空列表。
  [[nodiscard]] QList<ClipTarget> clipTargets() const;
  /// 当前是否有正在进行的传输。
  [[nodiscard]] bool transferActive() const
  {
    return m_transferActive;
  }
  [[nodiscard]] bool transferOutgoing() const
  {
    return m_transferOutgoing;
  }

public Q_SLOTS:
  void startPairing(const QString &deviceId);
  void submitPairCode(const QString &code);
  void cancelPairing();

  /// 把本地文件/目录发送到某个已配对对端。
  void sendFilesToPeer(const QString &deviceId, const QStringList &paths);
  /// 接受对端发来的 offer（收到 fileOfferReceived 之后调用）。
  void acceptIncomingFiles();
  /// 拒绝对端发来的 offer。
  void rejectIncomingFiles();
  /// 取消当前传输（收发都可以）。
  void cancelFileTransfer();
  /// 把收到的文件放到本机剪贴板上，方便用户直接 Ctrl+V。
  void copyFilesToClipboard(const QStringList &paths);

  /// 开关「自动连接已配对设备」并持久化到 QSettings（litekvm/autoConnect）。
  void setAutoConnectEnabled(bool enabled);
  [[nodiscard]] bool isAutoConnectEnabled() const
  {
    return m_autoConnect && m_autoConnect->enabled();
  }

Q_SIGNALS:
  void peerDiscovered(const litekvm::DiscoveredPeer &peer);
  void peerUpdated(const litekvm::DiscoveredPeer &peer);
  void peerOffline(const QString &deviceId);

  /// AutoConnect 发现可自动连接的已配对设备时触发（最小可用版：直接转发）。
  void autoSessionRequested(const litekvm::DiscoveredPeer &peer);
  void pairChallenge(const QString &peerName, const QString &peerFingerprint, const QString &expectedCode);
  void pairingSucceeded(const QString &deviceId, const QString &name);
  void pairingFailed(litekvm::PairingService::Error error);

  /// 本机剪贴板里出现了文件（用户复制了文件/文件夹）。
  void localFilesCopied(const QStringList &paths);
  /// 对端请求发文件过来（需要 UI 确认是否接受）。
  void fileOfferReceived(const QString &peerName, const QStringList &fileNames, qint64 totalBytes, int fileCount);
  /// 传输进度（outgoing = true 表示本机在发）。
  void fileProgressChanged(quint64 bytesDone, quint64 bytesTotal, bool outgoing);
  /// 传输完成；outgoing 为 false 时 localPaths 是落盘结果。
  void fileTransferCompleted(const QStringList &localPaths, bool outgoing, const QString &transferId);
  /// 传输失败（message 已拼好，可直接展示）。
  void fileTransferFailed(const QString &message, bool outgoing);
  /// 传输被取消。
  void fileTransferCancelled(bool outgoing);
  /// 文件剪贴板自身的错误（监听失败、协议错误等）。
  void fileClipboardError(const QString &message);

private:
  void setupFileClipboard();
  void applyFileClipboardSettings();
  [[nodiscard]] std::optional<ClipTarget> findTarget(const QString &deviceId) const;

  std::optional<litekvm::DeviceIdentity> m_identity;
  std::unique_ptr<litekvm::TrustStore> m_trust;
  litekvm::DiscoveryService *m_discovery = nullptr;
  litekvm::MdnsAdvertiser *m_advertiser = nullptr;
  litekvm::PairingService *m_pairing = nullptr;
  litekvm::AutoConnect *m_autoConnect = nullptr;

  litekvm::ClipFileService *m_clipFile = nullptr;
  litekvm::ClipboardFileBridge *m_clipBridge = nullptr;
  bool m_fileClipboardEnabled = false;
  bool m_transferActive = false;
  bool m_transferOutgoing = false;
  qint64 m_maxTransferBytes = -1;
};

} // namespace deskflow::gui
