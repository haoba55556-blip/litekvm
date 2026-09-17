// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// LiteKvmController — glue between MainWindow and the litekvm zero-config
// layer (identity, discovery, advertiser, pairing, file clipboard).
#include "LiteKvmController.h"

#include "common/Settings.h"
#include "litekvm/ClipFileService.h"
#include "litekvm/ClipFileTransfer.h"
#include "litekvm/ClipboardFileBridge.h"
#include "litekvm/MdnsAdvertiser.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace deskflow::gui {

LiteKvmController::LiteKvmController(QObject *parent) : QObject(parent)
{
  m_identity = litekvm::DeviceIdentity::loadOrCreate();
  if (!m_identity)
    return; // degraded mode: panel shows nothing, core features still work

  m_trust = std::make_unique<litekvm::TrustStore>();
  m_discovery = new litekvm::DiscoveryService(this);
  m_advertiser = new litekvm::MdnsAdvertiser(this);
  m_pairing = new litekvm::PairingService(*m_identity, *m_trust, this);

  const quint16 pairingPort = 25901;

  connect(m_discovery, &litekvm::DiscoveryService::peerDiscovered, this,
          &LiteKvmController::peerDiscovered);
  connect(m_discovery, &litekvm::DiscoveryService::peerUpdated, this,
          &LiteKvmController::peerUpdated);
  connect(m_discovery, &litekvm::DiscoveryService::peerOffline, this,
          &LiteKvmController::peerOffline);

  connect(m_pairing, &litekvm::PairingService::pairChallenge, this,
          &LiteKvmController::pairChallenge);
  connect(m_pairing, &litekvm::PairingService::pairingSucceeded, this,
          [this](const QString &id, const QString &name) {
            m_advertiser->setState(litekvm::MdnsAdvertiser::PeerState::Pairable);
            Q_EMIT pairingSucceeded(id, name);
          });
  connect(m_pairing, &litekvm::PairingService::pairingFailed, this,
          &LiteKvmController::pairingFailed);

  if (m_advertiser->start(*m_identity, pairingPort))
    m_discovery->start(*m_identity, m_trust.get());

  m_pairing->listen(pairingPort);

  // AutoConnect: paired peer online -> autoSessionRequested (minimal wiring)
  m_autoConnect = new litekvm::AutoConnect(*m_identity, *m_trust, *m_discovery, this);
  connect(m_autoConnect, &litekvm::AutoConnect::autoConnectRequested, this,
          &LiteKvmController::autoSessionRequested);
  // restore persisted toggle — without this the switch silently resets to
  // "off" on every launch and auto-connect never runs (bug found 2026-09-20)
  if (Settings::value(QStringLiteral("litekvm/autoConnect")).toBool())
    m_autoConnect->setEnabled(true);

  setupFileClipboard();
}

QString LiteKvmController::deviceName() const
{
  return m_identity ? m_identity->name() : QString();
}

void LiteKvmController::setDeviceName(const QString &name)
{
  // identity rename persists on next save; discovery TXT refreshes via announce
}

void LiteKvmController::setAutoConnectEnabled(bool enabled)
{
  if (m_autoConnect)
    m_autoConnect->setEnabled(enabled);
  Settings::setValue(QStringLiteral("litekvm/autoConnect"), enabled);
  Settings::save(false);
}

void LiteKvmController::startPairing(const QString &deviceId)
{
  if (!m_pairing)
    return;
  auto peer = m_discovery->peer(deviceId);
  if (!peer)
    return;
  m_advertiser->setState(litekvm::MdnsAdvertiser::PeerState::Busy);
  m_pairing->pairWith(*peer);
}

void LiteKvmController::submitPairCode(const QString &code)
{
  if (m_pairing)
    m_pairing->submitPairCode(code);
}

void LiteKvmController::cancelPairing()
{
  if (m_pairing)
    m_pairing->cancelPairing();
  m_advertiser->setState(litekvm::MdnsAdvertiser::PeerState::Pairable);
}

quint16 LiteKvmController::clipFilePort()
{
  return litekvm::ClipFileService::defaultPort();
}

// ---------------------------------------------------------------- file clipboard
//
// 对底层两件套的调用集中在 setupFileClipboard() / setFileClipboardEnabled() /
// sendFilesToPeer() 三处，接口变动只需要改这里。
// 刻意不打开 ClipFileService::setAutoAccept()：接不接受由 GUI 决定（先建好进度窗口
// 再 acceptOffer()），而且 ClipFileTransfer::handleOffer() 是在 emit offerReceived()
// 之后才看 autoAccept 的，两边都开会变成重复 accept。
void LiteKvmController::setupFileClipboard()
{
  m_clipFile = new litekvm::ClipFileService(*m_identity, *m_trust, this);
  m_clipBridge = new litekvm::ClipboardFileBridge(this);

  // ---- ClipFileService -> UI ----
  connect(m_clipFile, &litekvm::ClipFileService::offerReceived, this,
          [this](const litekvm::ClipFileOffer &offer) {
            if (!m_fileClipboardEnabled) {
              // 关掉文件剪贴板时不打扰用户，静默拒绝
              m_clipFile->rejectOffer();
              return;
            }
            QStringList names;
            names.reserve(offer.entries.size());
            for (const auto &entry : offer.entries)
              names.append(entry.isDirectory ? entry.relPath + QLatin1Char('/') : entry.relPath);

            m_transferActive = true;
            m_transferOutgoing = false;
            Q_EMIT fileOfferReceived(offer.peerName, names, offer.totalBytes, int(offer.entries.size()));
          });

  connect(m_clipFile, &litekvm::ClipFileService::progressChanged, this,
          [this](const litekvm::TransferProgress &progress) {
            Q_EMIT fileProgressChanged(progress.bytesDone, progress.bytesTotal, m_transferOutgoing);
          });

  connect(m_clipFile, &litekvm::ClipFileService::transferCompleted, this,
          [this](const QStringList &localPaths, const QString &transferId) {
            m_transferActive = false;
            Q_EMIT fileTransferCompleted(localPaths, m_transferOutgoing, transferId);
          });

  connect(m_clipFile, &litekvm::ClipFileService::transferFailed, this,
          [this](litekvm::ClipFileTransfer::Error error, const QString &detail) {
            m_transferActive = false;

            using E = litekvm::ClipFileTransfer::Error;
            QString reason;
            switch (error) {
            case E::Io:
              reason = tr("读写文件失败");
              break;
            case E::Checksum:
              reason = tr("文件校验失败（sha1 不一致）");
              break;
            case E::Protocol:
              reason = tr("协议错误");
              break;
            case E::Rejected:
              reason = tr("对端拒绝了这次传输");
              break;
            case E::Cancelled:
              reason = tr("传输已取消");
              break;
            case E::TooLarge:
              reason = tr("文件超过单次传输上限");
              break;
            case E::Timeout:
              reason = tr("等待对端超时");
              break;
            case E::PeerError:
              reason = tr("对端出错");
              break;
            case E::None:
            default:
              reason = litekvm::ClipFileTransfer::toString(error);
              break;
            }
            if (!detail.isEmpty())
              reason += QStringLiteral("（%1）").arg(detail);
            Q_EMIT fileTransferFailed(reason, m_transferOutgoing);
          });

  connect(m_clipFile, &litekvm::ClipFileService::transferCancelled, this, [this](const QString &transferId) {
    Q_UNUSED(transferId)
    m_transferActive = false;
    Q_EMIT fileTransferCancelled(m_transferOutgoing);
  });

  connect(m_clipFile, &litekvm::ClipFileService::errorOccurred, this,
          [this](const QString &message) { Q_EMIT fileClipboardError(message); });

  // 会话被对端/网络拉断时 ClipFileTransfer 不一定能报出终态，这里兜一下，
  // 免得进度窗口一直挂在那儿。
  connect(m_clipFile, &litekvm::ClipFileService::peerDisconnected, this, [this] {
    if (!m_transferActive)
      return;
    m_transferActive = false;
    Q_EMIT fileTransferFailed(tr("与对端的连接断开了"), m_transferOutgoing);
  });

  // ---- ClipboardFileBridge -> UI ----
  // 构造后就开始监视本机剪贴板（没有 GUI 时它自己不去碰系统剪贴板）；关闭文件
  // 剪贴板时 setFileClipboardEnabled() 会用 setWatchEnabled(false) 停掉上报，
  // 这里的 enabled 判断是第二道保险。
  connect(m_clipBridge, &litekvm::ClipboardFileBridge::localFilesCopied, this,
          [this](const QStringList &paths) {
            if (!m_fileClipboardEnabled || paths.isEmpty())
              return;
            Q_EMIT localFilesCopied(paths);
          });
}

void LiteKvmController::setFileClipboardEnabled(bool enabled)
{
  if (enabled == m_fileClipboardEnabled)
    return;
  m_fileClipboardEnabled = enabled;

  if (!m_clipFile)
    return;

  if (enabled) {
    // 打开文件剪贴板端口；监听失败的原因由服务的 errorOccurred() 报出来
    // （端口被占 / 已经有一个会话在跑）。
    if (!m_clipFile->listen(clipFilePort()))
      qWarning() << "file clipboard: listen on port" << clipFilePort() << "failed";
    if (m_clipBridge)
      m_clipBridge->setWatchEnabled(true);
    applyFileClipboardSettings();
  } else {
    // 关掉监听、掐掉进行中的会话（stop() 会取消传输并清掉半截的 .litekvm-part），
    // 同时停止上报本地剪贴板里的文件。
    if (m_clipBridge)
      m_clipBridge->setWatchEnabled(false);
    m_clipFile->stop();
    m_transferActive = false;
  }
}

void LiteKvmController::setMaxTransferBytes(qint64 bytes)
{
  m_maxTransferBytes = bytes;
  applyFileClipboardSettings();
}

void LiteKvmController::applyFileClipboardSettings()
{
  if (m_clipFile && m_maxTransferBytes > 0)
    m_clipFile->setMaxTransferBytes(m_maxTransferBytes);
}

QList<LiteKvmController::ClipTarget> LiteKvmController::clipTargets() const
{
  QList<ClipTarget> targets;
  QSet<QString> seen;

  // 1) 正在 mDNS 里可见、且已配对的设备（可以直接发）
  if (m_discovery) {
    for (const auto &peer : m_discovery->peers()) {
      if (peer.state != litekvm::DiscoveredPeer::State::Paired || peer.host.isEmpty())
        continue;
      targets.append(ClipTarget{peer.deviceId, peer.name, peer.platform, peer.host, clipFilePort(), true});
      seen.insert(peer.deviceId);
    }
  }

  // 2) 信任库里记过地址、但眼下不在 mDNS 列表里的设备
  if (m_trust) {
    for (const auto &entry : m_trust->entries()) {
      if (seen.contains(entry.deviceId) || entry.lastAddr.isEmpty())
        continue;
      targets.append(ClipTarget{entry.deviceId, entry.name, entry.platform, entry.lastAddr, clipFilePort(), false});
      seen.insert(entry.deviceId);
    }
  }

  return targets;
}

std::optional<LiteKvmController::ClipTarget> LiteKvmController::findTarget(const QString &deviceId) const
{
  for (const auto &target : clipTargets()) {
    if (target.deviceId == deviceId)
      return target;
  }
  return std::nullopt;
}

void LiteKvmController::sendFilesToPeer(const QString &deviceId, const QStringList &paths)
{
  if (!m_clipFile || !m_fileClipboardEnabled || paths.isEmpty())
    return;

  const auto target = findTarget(deviceId);
  if (!target || target->host.isEmpty()) {
    Q_EMIT fileClipboardError(tr("找不到已配对的对端「%1」，无法发送文件。").arg(deviceId));
    return;
  }

  m_transferActive = true;
  m_transferOutgoing = true;
  m_clipFile->sendFilesTo(target->host, target->port, paths);
}

void LiteKvmController::acceptIncomingFiles()
{
  if (!m_clipFile)
    return;
  m_transferActive = true;
  m_transferOutgoing = false;
  m_clipFile->acceptOffer();
}

void LiteKvmController::rejectIncomingFiles()
{
  if (!m_clipFile)
    return;
  m_transferActive = false;
  m_clipFile->rejectOffer();
}

void LiteKvmController::cancelFileTransfer()
{
  if (!m_clipFile)
    return;
  m_transferActive = false;
  m_clipFile->cancel();
}

void LiteKvmController::copyFilesToClipboard(const QStringList &paths)
{
  if (m_clipBridge && !paths.isEmpty())
    m_clipBridge->putFilesOnClipboard(paths);
}

} // namespace deskflow::gui
