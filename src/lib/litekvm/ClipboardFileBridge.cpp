// SPDX-License-Identifier: GPL-2.0
// LiteKVM cross-machine file clipboard — the local clipboard end.
//
// Spec: docs/plans/2026-08-25-litekvm-mvp.md Phase 4 Task 4.3
//
// Detection: Windows Explorer puts the selected files on the clipboard as
// "file:///C:/..." URLs (plus a text list of names); Qt surfaces those through
// QMimeData::hasUrls(). Requiring every URL to be a local file is what keeps a
// browser link, a photo dragged from a mail client or a UNC share out of the
// file-transfer path.
//
// Loop guard: the clipboard is a shared object with no reliable "who wrote
// this" flag, so the guard is a short time window plus the content we wrote.
// putFilesOnClipboard() records the paths it wrote and arms a single-shot
// timer *before* calling setMimeData(), because the platform layer can deliver
// the matching dataChanged() synchronously from inside that call.
#include "ClipboardFileBridge.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QMimeData>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace litekvm {

namespace {

/// True for "file:..." (case-insensitive), used to accept file URLs wherever a
/// local path is expected.
bool looksLikeFileUrl(const QString &text)
{
  return text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive);
}

/// 真正落在本机的 file URL。QUrl::isLocalFile() 在 Windows 上对 file://host/share
/// （UNC 共享）也会返回 true，但那种路径跨机传给对端没有意义 —— 对端机器读不到
/// 同一个网络位置，所以这里显式排掉非空 host。
bool isTrulyLocalFile(const QUrl &url)
{
  if (!url.isLocalFile())
    return false;
  const QString host = url.host();
  return host.isEmpty() || host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0;
}

} // namespace

// ------------------------------------------------------------------ ctor

ClipboardFileBridge::ClipboardFileBridge(QObject *parent)
  : QObject(parent)
  , m_guardTimer(new QTimer(this))
{
  m_guardTimer->setSingleShot(true);
  connect(m_guardTimer, &QTimer::timeout, this, &ClipboardFileBridge::onSelfWriteGuardExpired);

  setClipboard(defaultClipboard()); // no-op when there is no GUI clipboard
}

QClipboard *ClipboardFileBridge::defaultClipboard()
{
  // QGuiApplication::clipboard() goes straight to the platform integration and
  // aborts without a QGuiApplication, so the instance is checked first.
  if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr)
    return nullptr;
  return QGuiApplication::clipboard();
}

void ClipboardFileBridge::setClipboard(QClipboard *clipboard)
{
  if (m_clipboard == clipboard)
    return;

  if (m_clipboard != nullptr)
    disconnect(m_clipboard, nullptr, this, nullptr);

  m_clipboard = clipboard;

  // A null clipboard is a supported state (no QGuiApplication: headless tests,
  // --no-gui): every clipboard use below is guarded, the object then only
  // classifies mime data handed to handleClipboardData().
  if (m_clipboard != nullptr)
    connect(m_clipboard, &QClipboard::dataChanged, this, &ClipboardFileBridge::onClipboardDataChanged);
}

// ----------------------------------------------------------- pure helpers

bool ClipboardFileBridge::isLocalFileList(const QMimeData *mime)
{
  if (mime == nullptr || !mime->hasUrls())
    return false;

  const QList<QUrl> urls = mime->urls();
  if (urls.isEmpty())
    return false; // hasUrls() is true for an empty "text/uri-list" too

  for (const QUrl &url : urls) {
    // file://host/share is a UNC path, not a local file — skipped on purpose.
    if (!isTrulyLocalFile(url))
      return false;
  }
  return true;
}

QString ClipboardFileBridge::toLocalFilePath(const QUrl &url)
{
  if (!isTrulyLocalFile(url))
    return QString();
  // toLocalFile() percent-decodes the URL (so 中文 and spaces survive) and
  // toNativeSeparators() gives back the C:\ form Explorer shows the user.
  // Both steps are idempotent, so a platform that already returns backslashes
  // is unaffected.
  return QDir::toNativeSeparators(url.toLocalFile());
}

QUrl ClipboardFileBridge::toFileUrl(const QString &localPath)
{
  const QString trimmed = localPath.trimmed();
  if (looksLikeFileUrl(trimmed)) {
    const QUrl asUrl(trimmed);
    if (asUrl.isLocalFile())
      return asUrl;
  }
  // fromLocalFile() handles native separators (backslashes on Windows) and
  // encodes everything that needs encoding.
  return QUrl::fromLocalFile(trimmed);
}

QStringList ClipboardFileBridge::localPathsFromMime(const QMimeData *mime)
{
  QStringList paths;
  if (mime == nullptr || !mime->hasUrls())
    return paths;

  const QList<QUrl> urls = mime->urls();
  paths.reserve(urls.size());
  for (const QUrl &url : urls) {
    const QString path = toLocalFilePath(url);
    if (!path.isEmpty())
      paths.append(path);
  }
  paths.removeDuplicates();
  return paths;
}

QStringList ClipboardFileBridge::normalizedPaths(const QStringList &localPaths)
{
  QStringList out;
  out.reserve(localPaths.size());
  for (const QString &path : localPaths) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
      continue;

    QString normalized;
    if (looksLikeFileUrl(trimmed)) {
      const QUrl url(trimmed);
      if (!url.isLocalFile())
        continue; // file://server/share is a UNC share, not something we paste
      normalized = toLocalFilePath(url);
    } else {
      // A URL to somewhere else (https://, smb://, ...) can never be a local
      // path. A single-letter scheme is a Windows drive letter ("C:/Users/...")
      // rather than a URL scheme, so that one is kept.
      const QUrl asUrl(trimmed);
      if (asUrl.scheme().size() > 1 && !asUrl.isLocalFile())
        continue;
      normalized = QDir::toNativeSeparators(trimmed);
    }
    if (!normalized.isEmpty())
      out.append(normalized);
  }
  out.removeDuplicates();
  return out;
}

QMimeData *ClipboardFileBridge::makeFileMimeData(const QStringList &localPaths)
{
  const QStringList paths = normalizedPaths(localPaths);
  if (paths.isEmpty())
    return nullptr;

  QList<QUrl> urls;
  urls.reserve(paths.size());
  for (const QString &path : paths)
    urls.append(toFileUrl(path));
  if (urls.isEmpty())
    return nullptr;

  // setUrls() is what Qt's Windows mime converter turns into CF_HDROP, which is
  // the format Explorer pastes from. Folders work the same way.
  auto *mime = new QMimeData;
  mime->setUrls(urls);
  return mime;
}

// ---------------------------------------------------------------- state

void ClipboardFileBridge::setFileClipboardState(bool isFileClipboard)
{
  if (m_isFileClipboard == isFileClipboard)
    return;
  m_isFileClipboard = isFileClipboard;
  Q_EMIT fileClipboardStateChanged(m_isFileClipboard);
}

// -------------------------------------------------------------- watching

void ClipboardFileBridge::onClipboardDataChanged()
{
  if (!m_clipboard)
    return;
  // mimeData() is null when the clipboard cannot be opened (on Windows one
  // process owns it at a time, so this happens routinely while another app is
  // writing) and when the clipboard really is empty. Both mean "nothing we can
  // read" — not a change, and definitely not a crash.
  handleClipboardData(m_clipboard->mimeData());
}

bool ClipboardFileBridge::handleClipboardData(const QMimeData *mime)
{
  if (!m_watchEnabled)
    return false;

  const QStringList paths = localPathsFromMime(mime);

  // Our own paste coming back through dataChanged(). Dropping it here is what
  // stops two machines from re-sending the same files to each other forever.
  if (isEchoOfOwnWrite(paths))
    return false;

  if (!isLocalFileList(mime)) {
    // Text, an image, a browser link, a mixed set: not our business.
    m_localFiles.clear();
    setFileClipboardState(false);
    return false;
  }

  m_localFiles = paths;
  setFileClipboardState(true);
  Q_EMIT localFilesCopied(paths);
  return true;
}

void ClipboardFileBridge::refresh()
{
  onClipboardDataChanged();
}

// --------------------------------------------------------------- writing

bool ClipboardFileBridge::putFilesOnClipboard(const QStringList &localPaths)
{
  if (!m_clipboard)
    return false; // no clipboard in this process

  const QStringList written = normalizedPaths(localPaths);
  if (written.isEmpty())
    return false;

  std::unique_ptr<QMimeData> mime(makeFileMimeData(written));
  if (!mime)
    return false;

  // Arm the guard *before* the write: setMimeData() can deliver the matching
  // dataChanged() synchronously here, and the echo must already be
  // recognisable when it arrives.
  armSelfWriteGuard(written);

  // setMimeData() takes ownership of the QMimeData.
  m_clipboard->setMimeData(mime.release(), QClipboard::Clipboard);

  // Optimistic state: the clipboard now holds what we wrote. If the write
  // silently failed (someone else owns the clipboard) the next dataChanged()
  // corrects this.
  m_localFiles = written;
  setFileClipboardState(true);
  return true;
}

// ------------------------------------------------------------ loop guard

void ClipboardFileBridge::setSelfWriteGuardMs(int ms)
{
  if (ms <= 0) {
    m_guardMs = 0; // disabled: the caller breaks the loop itself
    return;
  }
  m_guardMs = std::clamp(ms, kMinSelfWriteGuardMs, kMaxSelfWriteGuardMs);
}

void ClipboardFileBridge::armSelfWriteGuard(const QStringList &writtenPaths)
{
  if (m_guardMs <= 0)
    return;

  const QStringList written = normalizedPaths(writtenPaths);
  if (written.isEmpty())
    return;

  // Keep a few writes: a remote paste and a re-offer can land back to back and
  // both echoes still have to be recognised.
  m_recentWrites.prepend(written);
  while (m_recentWrites.size() > kRecentWriteHistory)
    m_recentWrites.removeLast();

  m_guardTimer->start(m_guardMs);
}

bool ClipboardFileBridge::selfWriteGuardActive() const
{
  return m_guardTimer != nullptr && m_guardTimer->isActive();
}

bool ClipboardFileBridge::isEchoOfOwnWrite(const QStringList &localPaths) const
{
  if (localPaths.isEmpty() || !selfWriteGuardActive())
    return false;
  return m_recentWrites.contains(localPaths);
}

void ClipboardFileBridge::onSelfWriteGuardExpired()
{
  // The window closed. Forgetting what we wrote is deliberate: a user copying
  // those same files again later is a real copy and must be reported.
  m_recentWrites.clear();
}

} // namespace litekvm
