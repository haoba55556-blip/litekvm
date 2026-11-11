// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// LiteKvmController — glue between MainWindow and the litekvm zero-config
// layer (identity, discovery, advertiser, pairing, file clipboard).
#include "LiteKvmController.h"

#include "common/Settings.h"
#include "litekvm/ClipFileService.h"
#include "litekvm/ClipFileTransfer.h"
#include "litekvm/ClipRoute.h"
#include "litekvm/ClipboardFileBridge.h"
#include "litekvm/MdnsAdvertiser.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

namespace {

// 25903 lives here, not on the Qt GUI thread. Explorer's modal drag loop
// dispatches some posted events but starves QTcpSocket notifiers, so
// readyRead / bytesWritten / ClipFileTransfer::pump() run in this thread.
class ClipFileThread : public QThread
{
public:
  // QThread's parent constructor is not inherited. The controller owns this
  // thread, so the parent pointer has to be forwarded explicitly.
  explicit ClipFileThread(QObject *parent = nullptr) : QThread(parent)
  {
  }

  litekvm::ClipFileService *service = nullptr;
  QSemaphore ready;

protected:
  void run() override
  {
    QObject ping;
    // Fire only after exec() is pumping, so the GUI's BlockingQueued calls
    // cannot arrive before the loop exists.
    QTimer::singleShot(0, &ping, [this] {
      litekvm::ClipboardFileBridge::traceStage(
          QStringLiteral("clip-io"), QStringLiteral("25903 io thread=%1").arg(QThread::currentThread()->objectName())
      );
      ready.release();
    });
    exec();
    delete service;
    service = nullptr;
  }
};

} // namespace

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

  connect(m_discovery, &litekvm::DiscoveryService::peerDiscovered, this, &LiteKvmController::peerDiscovered);
  connect(m_discovery, &litekvm::DiscoveryService::peerUpdated, this, &LiteKvmController::peerUpdated);
  connect(m_discovery, &litekvm::DiscoveryService::peerOffline, this, &LiteKvmController::peerOffline);

  connect(m_pairing, &litekvm::PairingService::pairChallenge, this, &LiteKvmController::pairChallenge);
  connect(m_pairing, &litekvm::PairingService::pairingSucceeded, this, [this](const QString &id, const QString &name) {
    m_advertiser->setState(litekvm::MdnsAdvertiser::PeerState::Pairable);
    Q_EMIT pairingSucceeded(id, name);
  });
  connect(m_pairing, &litekvm::PairingService::pairingFailed, this, &LiteKvmController::pairingFailed);

  // Listen before the first announce. The advertiser also repeats every
  // couple of seconds, so a peer that boots later still gets found.
  m_discovery->start(*m_identity, m_trust.get());
  m_advertiser->start(*m_identity, pairingPort);

  m_pairing->listen(pairingPort);

  // AutoConnect: paired peer online -> autoSessionRequested.
  // The enabled flag is restored by MainWindow AFTER it connects the signal.
  // Enabling here would mark the peer "attempted" with nobody listening.
  m_autoConnect = new litekvm::AutoConnect(*m_identity, *m_trust, *m_discovery, this);
  connect(m_autoConnect, &litekvm::AutoConnect::autoConnectRequested, this, &LiteKvmController::autoSessionRequested);

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

void LiteKvmController::noteAutoSessionUp(const QString &deviceId)
{
  if (m_autoConnect)
    m_autoConnect->noteSessionUp(deviceId);
}

void LiteKvmController::noteAutoSessionDown(const QString &deviceId)
{
  if (m_autoConnect)
    m_autoConnect->noteSessionDown(deviceId);
}

void LiteKvmController::noteLivePeer(const QString &screenName, const QString &host)
{
  const QString addr = litekvm::DiscoveryService::normalizeHost(host);
  if (addr.isEmpty() || !m_trust || litekvm::hostIsThisMachine(addr))
    return;
  m_livePeerHost = addr;
  m_livePeerName = screenName;
  const QString selfName = m_identity ? m_identity->name() : QString();
  const int paired = int(m_trust->entries().size());
  for (const auto &entry : m_trust->entries()) {
    litekvm::ClipRouteInput route;
    route.liveHost = addr;
    route.liveScreenName = screenName;
    route.deviceId = entry.deviceId;
    route.deviceName = entry.name;
    route.pairedCount = paired;
    if (!litekvm::liveHostApplies(route))
      continue;
    m_trust->updateLastAddr(entry.deviceId, addr);
    const QString label = litekvm::clipPeerLabel(entry.name, screenName, selfName);
    if (label != entry.name) {
      litekvm::TrustEntry renamed = entry;
      renamed.name = label;
      renamed.lastAddr = addr;
      m_trust->upsert(renamed);
    }
  }
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
LiteKvmController::~LiteKvmController()
{
  if (!m_clipThread || !m_clipFile)
    return;
  // Drop queued GUI slots before stop() emits. The service is deleted on the
  // worker after exec() returns; the GUI must not touch it again.
  QObject::disconnect(m_clipFile, nullptr, this, nullptr);
  if (m_clipThread->isRunning()) {
    QMetaObject::invokeMethod(
        m_clipFile,
        [svc = m_clipFile] {
          svc->stop();
          QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        },
        Qt::BlockingQueuedConnection
    );
    m_clipThread->quit();
    m_clipThread->wait();
  }
  m_clipFile = nullptr;
}

void LiteKvmController::setupFileClipboard()
{
  // No parent: moveToThread() refuses an object that has one. The worker
  // deletes the service after its event loop stops.
  m_clipFile = new litekvm::ClipFileService(*m_identity, *m_trust, nullptr);
  m_clipBridge = new litekvm::ClipboardFileBridge(this);

  auto *ioThread = new ClipFileThread(this);
  ioThread->setObjectName(QStringLiteral("clip-file-25903"));
  ioThread->service = m_clipFile;
  m_clipThread = ioThread;
  m_clipFile->moveToThread(ioThread);
  ioThread->start();
  ioThread->ready.acquire();

  // ---- ClipFileService -> UI ----
  connect(m_clipFile, &litekvm::ClipFileService::offerReceived, this, [this](const litekvm::ClipFileOffer &offer) {
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
    const QString intent = offer.intent.isEmpty() ? QStringLiteral("clipboard") : offer.intent;
    Q_EMIT fileOfferReceived(offer.peerName, names, offer.totalBytes, int(offer.entries.size()), intent);
  });

  connect(
      m_clipFile, &litekvm::ClipFileService::progressChanged, this,
      [this](const litekvm::TransferProgress &progress) {
        Q_EMIT fileProgressChanged(progress.bytesDone, progress.bytesTotal, m_transferOutgoing);
      }
  );

  connect(
      m_clipFile, &litekvm::ClipFileService::transferCompleted, this,
      [this](const QStringList &localPaths, const QString &transferId) {
        m_transferActive = false;
        Q_EMIT fileTransferCompleted(localPaths, m_transferOutgoing, transferId);
      }
  );

  connect(
      m_clipFile, &litekvm::ClipFileService::transferFailed, this,
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
      }
  );

  connect(m_clipFile, &litekvm::ClipFileService::transferCancelled, this, [this](const QString &transferId) {
    Q_UNUSED(transferId)
    m_transferActive = false;
    Q_EMIT fileTransferCancelled(m_transferOutgoing);
  });

  connect(m_clipFile, &litekvm::ClipFileService::errorOccurred, this, [this](const QString &message) {
    // A busy error must not clear the flag while the other transfer is still
    // the live session — the next drag would log "sending" and then drop the
    // CLIP_OFFER because the socket is still held.
    const bool live = m_clipFile && m_clipFile->isBusy();
    if (!live)
      m_transferActive = false;
    Q_EMIT fileClipboardError(message);
  });

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
  connect(
      m_clipBridge, &litekvm::ClipboardFileBridge::localFilesCopied, this,
      [this](const QStringList &paths, const QString &intent) {
        if (!m_fileClipboardEnabled || paths.isEmpty())
          return;
        Q_EMIT localFilesCopied(paths, intent.isEmpty() ? QStringLiteral("clipboard") : intent);
      }
  );
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

bool LiteKvmController::transferActive() const
{
  return m_clipFile && m_clipFile->isBusy();
}

void LiteKvmController::dropIdleFileSession()
{
  if (!m_clipFile)
    return;
  m_clipFile->dropIdleSession();
  if (!m_clipFile->hasSession())
    m_transferActive = false;
}

QList<LiteKvmController::ClipTarget> LiteKvmController::clipTargets() const
{
  QList<ClipTarget> targets;
  QSet<QString> seen;
  const int paired = m_trust ? int(m_trust->entries().size()) : 0;
  const QString selfId = m_identity ? m_identity->deviceId() : QString();
  const QString selfName = m_identity ? m_identity->name() : QString();

  auto routeHost = [&](const QString &deviceId, const QString &name, const QString &discovered,
                       const QString &trusted) {
    litekvm::ClipRouteInput route;
    route.liveHost = m_livePeerHost;
    route.liveScreenName = m_livePeerName;
    route.deviceId = deviceId;
    route.deviceName = name;
    route.pairedCount = paired;
    route.discoveredHost = discovered;
    route.trustedHost = trusted;
    return litekvm::clipRouteHost(route);
  };

  // 1) mDNS peers, but a live core socket address replaces a stale host.
  if (m_discovery) {
    for (const auto &peer : m_discovery->peers()) {
      if (peer.state != litekvm::DiscoveredPeer::State::Paired)
        continue;
      QString trusted;
      if (m_trust) {
        if (const auto entry = m_trust->find(peer.deviceId))
          trusted = entry->lastAddr;
      }
      if (!selfId.isEmpty() && peer.deviceId == selfId)
        continue;
      const QString host = routeHost(peer.deviceId, peer.name, peer.host, trusted);
      if (host.isEmpty() || litekvm::hostIsThisMachine(host))
        continue;
      const QString label = litekvm::liveHostApplies(
                                {m_livePeerHost, m_livePeerName, peer.deviceId, peer.name, paired, peer.host, trusted}
                            )
                                ? litekvm::clipPeerLabel(peer.name, m_livePeerName, selfName)
                                : peer.name;
      targets.append(ClipTarget{peer.deviceId, label, peer.platform, host, clipFilePort(), true});
      seen.insert(peer.deviceId);
    }
  }

  // 2) Paired devices discovery cannot see. Live socket address wins over trust.json.
  if (m_trust) {
    for (const auto &entry : m_trust->entries()) {
      if (seen.contains(entry.deviceId) || (!selfId.isEmpty() && entry.deviceId == selfId))
        continue;
      const QString host = routeHost(entry.deviceId, entry.name, QString(), entry.lastAddr);
      if (host.isEmpty() || litekvm::hostIsThisMachine(host))
        continue;
      const bool live = litekvm::liveHostApplies(
          {m_livePeerHost, m_livePeerName, entry.deviceId, entry.name, paired, {}, entry.lastAddr}
      );
      const QString label = live ? litekvm::clipPeerLabel(entry.name, m_livePeerName, selfName) : entry.name;
      targets.append(ClipTarget{entry.deviceId, label, entry.platform, host, clipFilePort(), live});
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

void LiteKvmController::sendFilesToPeer(const QString &deviceId, const QStringList &paths, const QString &intent)
{
  const QString wireIntent = intent.isEmpty() ? QStringLiteral("clipboard") : intent;
  if (!m_clipFile || !m_fileClipboardEnabled || paths.isEmpty()) {
    litekvm::ClipboardFileBridge::traceStage(
        QStringLiteral("sendFiles"), QStringLiteral("intent=%1 aborted peer-send clip=%2 enabled=%3 paths=%4")
                                         .arg(wireIntent)
                                         .arg(m_clipFile ? 1 : 0)
                                         .arg(m_fileClipboardEnabled ? 1 : 0)
                                         .arg(paths.size())
    );
    return;
  }

  const auto target = findTarget(deviceId);
  if (!target || target->host.isEmpty()) {
    litekvm::ClipboardFileBridge::traceStage(
        QStringLiteral("sendFiles"),
        QStringLiteral("intent=%1 aborted target-missing device=%2").arg(wireIntent, deviceId)
    );
    Q_EMIT fileClipboardError(tr("找不到已配对的对端「%1」，无法发送文件。").arg(deviceId));
    return;
  }

  m_transferActive = true;
  m_transferOutgoing = true;
  m_clipFile->sendFilesTo(target->host, target->port, paths, intent);
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
