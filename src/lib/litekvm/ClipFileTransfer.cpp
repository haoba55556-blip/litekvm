// SPDX-License-Identifier: GPL-2.0
// LiteKVM cross-machine file clipboard — chunked file transfer over an
// already paired (encrypted) data channel.
//
// Spec: docs/plans/2026-08-25-litekvm-mvp.md Phase 4 Task 4.1 / 4.2
// Wire format: ClipFileChunk.h
#include "ClipFileTransfer.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace litekvm {

namespace {
constexpr int kProgressThrottleMs = 50;
constexpr int kMinHighWaterMark = 64 * 1024;
} // namespace

// ---------------------------------------------------------------- helpers

QString ClipFileTransfer::toString(Error error)
{
  switch (error) {
  case Error::None:
    return QStringLiteral("NONE");
  case Error::Io:
    return QStringLiteral("IO");
  case Error::Checksum:
    return QStringLiteral("CHECKSUM");
  case Error::Protocol:
    return QStringLiteral("PROTO");
  case Error::Rejected:
    return QStringLiteral("REJECTED");
  case Error::Cancelled:
    return QStringLiteral("CANCELLED");
  case Error::TooLarge:
    return QStringLiteral("TOO_LARGE");
  case Error::Timeout:
    return QStringLiteral("TIMEOUT");
  case Error::PeerError:
    return QStringLiteral("PEER_ERROR");
  }
  return QStringLiteral("UNKNOWN");
}

QString ClipFileTransfer::toString(State state)
{
  switch (state) {
  case State::Idle:
    return QStringLiteral("IDLE");
  case State::Offering:
    return QStringLiteral("OFFERING");
  case State::OfferReceived:
    return QStringLiteral("OFFER_RECEIVED");
  case State::Sending:
    return QStringLiteral("SENDING");
  case State::AwaitingAck:
    return QStringLiteral("AWAITING_ACK");
  case State::Receiving:
    return QStringLiteral("RECEIVING");
  case State::Completed:
    return QStringLiteral("COMPLETED");
  case State::Cancelled:
    return QStringLiteral("CANCELLED");
  case State::Failed:
    return QStringLiteral("FAILED");
  }
  return QStringLiteral("UNKNOWN");
}

int ClipFileTransfer::defaultChunkSize()
{
  return int(kClipDefaultChunkSize);
}

QString ClipFileTransfer::defaultIncomingDirectory()
{
  QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  if (base.isEmpty())
    base = QDir::tempPath();
  return base + QStringLiteral("/LiteKVM/clipboard");
}

QString ClipFileTransfer::fileSha1(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return QString();
  clipsha1::Sha1 hasher;
  char buffer[256 * 1024];
  while (!file.atEnd()) {
    const qint64 n = file.read(buffer, qint64(sizeof(buffer)));
    if (n < 0)
      return QString();
    if (n == 0)
      break;
    hasher.update(buffer, std::size_t(n));
  }
  if (file.error() != QFileDevice::NoError)
    return QString();
  return toQString(hasher.hex());
}

QByteArray ClipFileTransfer::toQByteArray(const std::vector<std::uint8_t> &data)
{
  return QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()));
}

QString ClipFileTransfer::toQString(const std::string &utf8)
{
  return QString::fromUtf8(utf8.data(), qsizetype(utf8.size()));
}

std::string ClipFileTransfer::toStdString(const QByteArray &utf8)
{
  return std::string(utf8.constData(), std::size_t(utf8.size()));
}

QString ClipFileTransfer::newTransferId()
{
  // 32 hex chars, same shape as DeviceIdentity::deviceId()
  return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
}

QJsonObject ClipFileTransfer::baseMessage(const QString &type, const QString &transferId)
{
  QJsonObject msg;
  msg.insert(QStringLiteral("type"), type);
  msg.insert(QStringLiteral("proto"), QStringLiteral("v1"));
  if (!transferId.isEmpty())
    msg.insert(QStringLiteral("transfer_id"), transferId);
  return msg;
}

bool ClipFileTransfer::hashPrefix(const QString &path, std::uint64_t count, clipsha1::Sha1 *hasher)
{
  if (!hasher || count == 0)
    return true;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;

  std::vector<char> buffer(256 * 1024);
  std::uint64_t left = count;
  while (left > 0) {
    const qint64 want = qint64(left < std::uint64_t(buffer.size()) ? left : buffer.size());
    const qint64 n = file.read(buffer.data(), want);
    if (n <= 0)
      return false;
    hasher->update(buffer.data(), std::size_t(n));
    left -= std::uint64_t(n);
  }
  return true;
}

QString ClipFileTransfer::uniqueDestinationPath(const QString &desired)
{
  if (!QFileInfo::exists(desired))
    return desired;

  const QFileInfo info(desired);
  const QString dir = info.absolutePath();
  const QString base = info.completeBaseName();
  const QString suffix = info.suffix();
  for (int i = 1; i < 1000; ++i) {
    const QString candidate =
        suffix.isEmpty()
            ? QStringLiteral("%1/%2 (%3)").arg(dir, base).arg(i)
            : QStringLiteral("%1/%2 (%3).%4").arg(dir, base).arg(i).arg(suffix);
    if (!QFileInfo::exists(candidate))
      return candidate;
  }
  return desired;
}

// ------------------------------------------------------------- lifecycle

ClipFileTransfer::ClipFileTransfer(QIODevice *channel, QObject *parent)
  : QObject(parent), m_channel(channel)
{
  if (m_channel) {
    connect(m_channel, &QIODevice::readyRead, this, &ClipFileTransfer::onChannelReadyRead);
    connect(m_channel, &QIODevice::bytesWritten, this, [this](qint64) { pump(); });
  }

  m_offerTimer = new QTimer(this);
  m_offerTimer->setSingleShot(true);
  connect(m_offerTimer, &QTimer::timeout, this, &ClipFileTransfer::onOfferTimeout);

  m_elapsed.start();
}

ClipFileTransfer::~ClipFileTransfer() = default;

bool ClipFileTransfer::isActive() const
{
  switch (m_state) {
  case State::Offering:
  case State::OfferReceived:
  case State::Sending:
  case State::AwaitingAck:
  case State::Receiving:
    return true;
  default:
    return false;
  }
}

void ClipFileTransfer::setChunkSize(int bytes)
{
  if (m_state != State::Idle)
    return; // both sides must agree before the first chunk
  if (bytes < int(kClipMinChunkSize))
    bytes = int(kClipMinChunkSize);
  if (bytes > int(kClipMaxChunkSize))
    bytes = int(kClipMaxChunkSize);
  m_chunkSize = bytes;
}

void ClipFileTransfer::setIncomingDirectory(const QString &dir)
{
  m_incomingDir = QDir::cleanPath(dir);
}

void ClipFileTransfer::setMaxTransferBytes(qint64 bytes)
{
  m_maxTransferBytes = bytes > 0 ? bytes : 0;
}

void ClipFileTransfer::setOfferTimeout(int ms)
{
  m_offerTimeoutMs = ms > 0 ? ms : 0;
}

void ClipFileTransfer::setHighWaterMark(int bytes)
{
  if (bytes < kMinHighWaterMark)
    bytes = kMinHighWaterMark;
  m_highWaterMark = bytes;
}

void ClipFileTransfer::resetForNewTransfer()
{
  m_transferId.clear();
  m_peerName.clear();
  m_entries.clear();
  m_entryLocalPaths.clear();
  m_destPaths.clear();
  m_completedPaths.clear();
  m_totalBytes = 0;
  m_entryIndex = 0;
  m_entryOffset = 0;
  m_entryBytesDone = 0;
  m_bytesDone = 0;
  m_nextSeq = 0;
  m_resumeFrom = 0;
  m_entryOpen = false;
  m_plan = ChunkPlan();
  m_fileHasher = clipsha1::Sha1{};
  m_inFile.reset();
  m_outFile.reset();
  m_lastError = Error::None;
  m_progress = TransferProgress{};
  m_lastProgressMs = -1000;
}

void ClipFileTransfer::cleanupPartial(bool removePartFile)
{
  if (m_offerTimer)
    m_offerTimer->stop();

  if (m_inFile) {
    m_inFile->close();
    m_inFile.reset();
  }

  if (m_outFile) {
    m_outFile->close();
    m_outFile.reset();
    if (removePartFile && !m_destPaths.isEmpty() && m_entryIndex < m_destPaths.size()) {
      const QString part = m_destPaths.at(m_entryIndex).partPath;
      if (!part.isEmpty())
        QFile::remove(part);
    }
  }
  m_entryOpen = false;
}

void ClipFileTransfer::failed(Error error, const QString &detail)
{
  if (m_state == State::Failed || m_state == State::Cancelled)
    return; // first failure wins
  m_lastError = error;
  cleanupPartial(true);
  m_state = State::Failed;
  Q_EMIT transferFailed(error, detail);
}

void ClipFileTransfer::sendJson(const QJsonObject &msg)
{
  if (!m_channel)
    return;
  const QByteArray json = QJsonDocument(msg).toJson(QJsonDocument::Compact);
  const std::vector<std::uint8_t> frame =
      encodeControlFrame(std::string_view(json.constData(), std::size_t(json.size())));
  if (frame.empty()) {
    failed(Error::Protocol, QStringLiteral("control frame too large (%1 bytes)").arg(json.size()));
    return;
  }
  m_channel->write(toQByteArray(frame));
}

void ClipFileTransfer::sendError(const QString &code, const QString &detail)
{
  // best effort: report a peer-visible failure code without recursing into
  // failed() (the caller decides what to surface locally)
  QJsonObject msg = baseMessage(QStringLiteral("CLIP_ERROR"), m_transferId);
  msg.insert(QStringLiteral("code"), code);
  if (!detail.isEmpty())
    msg.insert(QStringLiteral("detail"), detail);
  if (m_channel) {
    const QByteArray json = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    const std::vector<std::uint8_t> frame =
        encodeControlFrame(std::string_view(json.constData(), std::size_t(json.size())));
    if (!frame.empty())
      m_channel->write(toQByteArray(frame));
  }
}

void ClipFileTransfer::sendReject(const QString &transferId, const QString &reason)
{
  QJsonObject msg = baseMessage(QStringLiteral("CLIP_REJECT"), transferId);
  msg.insert(QStringLiteral("reason"), reason);
  sendJson(msg);
}

void ClipFileTransfer::emitProgress(bool force)
{
  m_progress.bytesDone = m_bytesDone;
  m_progress.bytesTotal = m_totalBytes > 0 ? std::uint64_t(m_totalBytes) : 0;

  const qint64 now = m_elapsed.elapsed();
  if (!force && now - m_lastProgressMs < kProgressThrottleMs && m_bytesDone != m_progress.bytesTotal)
    return;
  m_lastProgressMs = now;
  Q_EMIT progressChanged(m_progress);
}

void ClipFileTransfer::onOfferTimeout()
{
  if (m_state == State::Offering)
    failed(Error::Timeout, QStringLiteral("peer did not answer CLIP_OFFER"));
}

// ----------------------------------------------------------------- sender

void ClipFileTransfer::sendLocalPaths(const QStringList &paths)
{
  if (m_state != State::Idle) {
    failed(Error::Protocol,
           QStringLiteral("sendLocalPaths() while %1").arg(toString(m_state)));
    return;
  }
  if (!m_channel) {
    failed(Error::Io, QStringLiteral("no channel"));
    return;
  }
  if (paths.isEmpty()) {
    failed(Error::Protocol, QStringLiteral("no paths to send"));
    return;
  }

  resetForNewTransfer();
  if (!stagePaths(paths))
    return;

  if (m_entries.isEmpty()) {
    failed(Error::Protocol, QStringLiteral("nothing to send"));
    return;
  }

  // sum the staged entries (directories contribute 0)
  m_totalBytes = 0;
  for (const ClipFileEntry &entry : std::as_const(m_entries))
    m_totalBytes += entry.size;

  m_transferId = newTransferId();
  m_sourcePaths = paths;
  m_progress = TransferProgress{0, std::uint64_t(m_totalBytes)};
  m_elapsed.restart();
  m_lastProgressMs = -1000;

  QJsonArray entries;
  for (const ClipFileEntry &entry : std::as_const(m_entries)) {
    QJsonObject obj;
    obj.insert(QStringLiteral("rel_path"), entry.relPath);
    obj.insert(QStringLiteral("size"), double(entry.size));
    obj.insert(QStringLiteral("is_dir"), entry.isDirectory);
    entries.append(obj);
  }

  QJsonObject offer = baseMessage(QStringLiteral("CLIP_OFFER"), m_transferId);
  offer.insert(QStringLiteral("chunk_size"), m_chunkSize);
  offer.insert(QStringLiteral("total_bytes"), double(m_totalBytes));
  offer.insert(QStringLiteral("file_count"), entries.size());
  offer.insert(QStringLiteral("entries"), entries);

  m_state = State::Offering;
  sendJson(offer);
  if (m_state == State::Failed)
    return;
  if (m_offerTimeoutMs > 0)
    m_offerTimer->start(m_offerTimeoutMs);
}

bool ClipFileTransfer::appendEntry(const QString &relPath, qint64 size, bool isDirectory,
                                   const QString &localPath, qint64 *totalBytes)
{
  const std::string normalized = normalizeRelativePath(toStdString(relPath.toUtf8()));
  if (!isSafeRelativePath(normalized)) {
    failed(Error::Protocol, QStringLiteral("unsafe path: %1").arg(relPath));
    return false;
  }

  const std::string name = sanitizeFileName(relPath.section(QLatin1Char('/'), -1).toUtf8().toStdString());
  if (name.empty()) {
    failed(Error::Protocol, QStringLiteral("unusable name: %1").arg(relPath));
    return false;
  }

  ClipFileEntry entry;
  entry.relPath = toQString(normalized);
  entry.isDirectory = isDirectory;
  entry.size = isDirectory ? 0 : size;

  if (!isDirectory) {
    if (entry.size < 0 || std::uint64_t(entry.size) > kClipMaxFileBytes) {
      failed(Error::TooLarge, QStringLiteral("%1 is too large").arg(relPath));
      return false;
    }
    if (totalBytes) {
      *totalBytes += entry.size;
      if (*totalBytes > m_maxTransferBytes) {
        failed(Error::TooLarge,
               QStringLiteral("transfer exceeds %1 bytes").arg(m_maxTransferBytes));
        return false;
      }
    }
  }

  m_entries.append(entry);
  m_entryLocalPaths.append(localPath);
  return true;
}

bool ClipFileTransfer::stagePaths(const QStringList &paths)
{
  qint64 total = 0;

  for (const QString &path : paths) {
    const QFileInfo info(path);
    if (!info.exists()) {
      failed(Error::Io, QStringLiteral("no such path: %1").arg(path));
      return false;
    }

    const QString baseName = info.fileName();
    if (baseName.isEmpty()) {
      failed(Error::Protocol, QStringLiteral("cannot mirror %1").arg(path));
      return false;
    }

    if (info.isFile()) {
      if (!appendEntry(baseName, info.size(), false, info.absoluteFilePath(), &total))
        return false;
      continue;
    }

    if (!info.isDir()) {
      failed(Error::Io, QStringLiteral("not a regular file: %1").arg(path));
      return false;
    }

    // directories: keep the folder itself (so empty folders survive) plus the
    // whole tree. Symlinked directories are not followed.
    const QString root = info.absoluteFilePath();
    if (!appendEntry(baseName, 0, true, root, &total))
      return false;

    const QDir rootDir(root);
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      const QFileInfo childInfo = it.fileInfo();
      const QString childName = childInfo.fileName();
      if (childName.isEmpty())
        continue;
      const QString rel = baseName + QLatin1Char('/') + rootDir.relativeFilePath(it.filePath());
      if (childInfo.isDir()) {
        if (!appendEntry(rel, 0, true, childInfo.absoluteFilePath(), &total))
          return false;
      } else if (childInfo.isFile()) {
        if (!appendEntry(rel, childInfo.size(), false, childInfo.absoluteFilePath(), &total))
          return false;
      }
      // sockets/devices/symlinks are skipped on purpose
    }
  }
  return true;
}

bool ClipFileTransfer::openOutgoingEntry(int index, qint64 resumeFrom)
{
  if (index < 0 || index >= m_entries.size())
    return false;

  const ClipFileEntry &entry = m_entries.at(index);
  m_entryIndex = index;
  m_entryOffset = 0;
  m_entryBytesDone = 0;
  m_nextSeq = 0;
  m_fileHasher = clipsha1::Sha1{};
  m_plan = ChunkPlan(std::uint64_t(entry.size), std::size_t(m_chunkSize));

  if (!m_plan.valid()) {
    failed(Error::Protocol, QStringLiteral("cannot plan chunks for %1").arg(entry.relPath));
    return false;
  }

  if (resumeFrom > 0) {
    if (!m_plan.seqForOffset(std::uint64_t(resumeFrom), &m_nextSeq)) {
      failed(Error::Protocol, QStringLiteral("resume offset is not chunk aligned"));
      return false;
    }
    m_entryOffset = std::uint64_t(resumeFrom);
    m_entryBytesDone = std::uint64_t(resumeFrom);
  }

  m_entryOpen = true;
  if (entry.isDirectory)
    return true;

  const QString localPath = m_entryLocalPaths.value(index);
  m_inFile.reset(new QFile(localPath));
  if (!m_inFile->open(QIODevice::ReadOnly)) {
    failed(Error::Io, QStringLiteral("cannot read %1: %2").arg(localPath, m_inFile->errorString()));
    return false;
  }

  if (m_entryOffset > 0) {
    // the whole-file digest must still cover the skipped prefix
    if (!hashPrefix(localPath, m_entryOffset, &m_fileHasher)) {
      failed(Error::Io, QStringLiteral("cannot re-hash %1").arg(localPath));
      return false;
    }
    if (!m_inFile->seek(qint64(m_entryOffset))) {
      failed(Error::Io, QStringLiteral("cannot seek %1").arg(localPath));
      return false;
    }
  }
  return true;
}

bool ClipFileTransfer::finishOutgoingEntry()
{
  const ClipFileEntry entry = m_entries.value(m_entryIndex);
  if (m_inFile) {
    m_inFile->close();
    m_inFile.reset();
  }
  m_entryOpen = false;

  QJsonObject msg = baseMessage(QStringLiteral("CLIP_COMPLETE"), m_transferId);
  msg.insert(QStringLiteral("entry_index"), m_entryIndex);
  msg.insert(QStringLiteral("size"), double(entry.size));
  msg.insert(QStringLiteral("sha1"), toQString(m_fileHasher.hex()));
  sendJson(msg);

  if (m_state != State::Failed)
    m_state = State::AwaitingAck;
  return m_state != State::Failed;
}

void ClipFileTransfer::pump()
{
  if (m_state != State::Sending || !m_channel)
    return;

  while (m_channel->bytesToWrite() < qint64(m_highWaterMark)) {
    if (m_entryIndex >= m_entries.size()) {
      failed(Error::Protocol, QStringLiteral("entry index out of range"));
      return;
    }

    if (!m_entryOpen && !openOutgoingEntry(m_entryIndex, m_resumeFrom))
      return;

    const ClipFileEntry entry = m_entries.at(m_entryIndex);

    if (entry.isDirectory) {
      finishOutgoingEntry();
      return; // wait for CLIP_DONE
    }

    if (std::uint64_t(m_nextSeq) >= m_plan.count()) {
      finishOutgoingEntry();
      return; // wait for CLIP_DONE
    }

    const std::size_t want = m_plan.length(m_nextSeq);
    QByteArray buffer;
    if (want > 0) {
      buffer = m_inFile->read(qint64(want));
      while (std::size_t(buffer.size()) < want && !m_inFile->atEnd()) {
        const QByteArray more = m_inFile->read(qint64(want) - buffer.size());
        if (more.isEmpty())
          break;
        buffer.append(more);
      }
      if (std::size_t(buffer.size()) != want) {
        failed(Error::Io, QStringLiteral("short read on %1").arg(entry.relPath));
        return;
      }
    }

    ChunkHeader header;
    if (!makeChunkHeader(m_nextSeq, m_entryOffset,
                         reinterpret_cast<const std::uint8_t *>(buffer.constData()),
                         std::size_t(buffer.size()), &header)) {
      failed(Error::Protocol, QStringLiteral("chunk header rejected"));
      return;
    }

    const std::vector<std::uint8_t> headerBytes = encodeChunkHeader(header);
    const std::vector<std::uint8_t> frame =
        encodeDataFrame(headerBytes.data(), headerBytes.size(),
                        reinterpret_cast<const std::uint8_t *>(buffer.constData()),
                        std::size_t(buffer.size()));
    if (frame.empty()) {
      failed(Error::Protocol, QStringLiteral("chunk frame rejected"));
      return;
    }

    m_fileHasher.update(buffer.constData(), std::size_t(buffer.size()));
    m_channel->write(toQByteArray(frame));

    m_entryOffset += std::uint64_t(buffer.size());
    m_entryBytesDone += std::uint64_t(buffer.size());
    m_bytesDone += std::uint64_t(buffer.size());
    ++m_nextSeq;
    emitProgress();

    if (std::uint64_t(m_nextSeq) >= m_plan.count()) {
      finishOutgoingEntry();
      return; // wait for CLIP_DONE
    }
  }
}

// --------------------------------------------------------------- receiver

void ClipFileTransfer::rejectOffer(const QString &reason)
{
  if (m_state != State::OfferReceived)
    return;
  QJsonObject msg = baseMessage(QStringLiteral("CLIP_REJECT"), m_transferId);
  msg.insert(QStringLiteral("reason"), reason);
  sendJson(msg);
  cleanupPartial(false);
  m_state = State::Cancelled;
  Q_EMIT transferCancelled(m_transferId);
}

bool ClipFileTransfer::prepareDestinations()
{
  const QString root = QDir::cleanPath(m_incomingDir);
  if (root.isEmpty() || !QDir().mkpath(root)) {
    failed(Error::Io, QStringLiteral("cannot create %1").arg(root));
    return false;
  }

  m_destPaths.clear();
  for (const ClipFileEntry &entry : std::as_const(m_entries)) {
    const QString candidate = QDir::cleanPath(root + QLatin1Char('/') + entry.relPath);
    // relPath is validated, but never trust a joined path blindly
    if (!candidate.startsWith(root + QLatin1Char('/'))) {
      failed(Error::Protocol, QStringLiteral("path escapes the incoming dir: %1").arg(entry.relPath));
      return false;
    }

    DestPath dest;
    if (entry.isDirectory) {
      dest.finalPath = candidate;
      if (!QDir().mkpath(candidate)) {
        failed(Error::Io, QStringLiteral("cannot create %1").arg(candidate));
        return false;
      }
    } else {
      const QString parent = QFileInfo(candidate).absolutePath();
      if (!QDir().mkpath(parent)) {
        failed(Error::Io, QStringLiteral("cannot create %1").arg(parent));
        return false;
      }
      dest.finalPath = uniqueDestinationPath(candidate);
      dest.partPath = QDir::cleanPath(dest.finalPath + QString::fromLatin1(kClipPartSuffix));
    }
    m_destPaths.append(dest);
  }
  return true;
}

bool ClipFileTransfer::openIncomingEntry(int index, qint64 resumeFrom)
{
  if (index < 0 || index >= m_entries.size())
    return false;

  const ClipFileEntry &entry = m_entries.at(index);
  m_entryIndex = index;
  m_entryOffset = 0;
  m_entryBytesDone = 0;
  m_nextSeq = 0;
  m_fileHasher = clipsha1::Sha1{};
  m_plan = ChunkPlan(std::uint64_t(entry.size), std::size_t(m_chunkSize));

  if (!m_plan.valid()) {
    failed(Error::Protocol, QStringLiteral("cannot plan chunks for %1").arg(entry.relPath));
    return false;
  }

  m_entryOpen = true;
  if (entry.isDirectory)
    return true;

  const DestPath dest = m_destPaths.value(index);
  m_outFile.reset(new QFile(dest.partPath));

  qint64 keep = 0;
  if (resumeFrom > 0 && QFileInfo::exists(dest.partPath) &&
      QFileInfo(dest.partPath).size() == resumeFrom) {
    keep = resumeFrom;
  }

  QIODevice::OpenMode mode = QIODevice::WriteOnly;
  mode |= keep > 0 ? QIODevice::Append : QIODevice::Truncate;
  if (!m_outFile->open(mode)) {
    failed(Error::Io, QStringLiteral("cannot write %1: %2").arg(dest.partPath, m_outFile->errorString()));
    return false;
  }

  if (keep > 0) {
    if (!hashPrefix(dest.partPath, std::uint64_t(keep), &m_fileHasher) ||
        !m_plan.seqForOffset(std::uint64_t(keep), &m_nextSeq)) {
      failed(Error::Protocol, QStringLiteral("cannot resume %1").arg(entry.relPath));
      return false;
    }
    m_entryOffset = std::uint64_t(keep);
    m_entryBytesDone = std::uint64_t(keep);
  }
  return true;
}

void ClipFileTransfer::acceptOffer(qint64 resumeFrom)
{
  if (m_state != State::OfferReceived) {
    failed(Error::Protocol, QStringLiteral("acceptOffer() while %1").arg(toString(m_state)));
    return;
  }
  if (resumeFrom < 0)
    resumeFrom = 0;
  if (resumeFrom > 0 && !ChunkPlan::isChunkAligned(std::uint64_t(resumeFrom), std::size_t(m_chunkSize))) {
    failed(Error::Protocol, QStringLiteral("resume offset is not chunk aligned"));
    return;
  }
  if (!prepareDestinations())
    return;

  m_completedPaths.clear();
  m_bytesDone = std::uint64_t(resumeFrom);
  m_resumeFrom = resumeFrom;
  m_entryIndex = 0;
  m_elapsed.restart();
  m_lastProgressMs = -1000;

  if (!openIncomingEntry(0, resumeFrom))
    return;

  QJsonObject reply = baseMessage(QStringLiteral("CLIP_ACCEPT"), m_transferId);
  reply.insert(QStringLiteral("chunk_size"), m_chunkSize);
  reply.insert(QStringLiteral("resume_from"), double(resumeFrom));
  m_state = State::Receiving;
  sendJson(reply);
}

// ------------------------------------------------------------------ cancel

void ClipFileTransfer::cancel()
{
  if (!isActive())
    return;

  if (!m_transferId.isEmpty()) {
    QJsonObject msg = baseMessage(QStringLiteral("CLIP_CANCEL"), m_transferId);
    msg.insert(QStringLiteral("reason"), QStringLiteral("USER"));
    sendJson(msg);
  }

  cleanupPartial(true);
  m_state = State::Cancelled;
  Q_EMIT transferCancelled(m_transferId);
}

// ------------------------------------------------------------ frame intake

void ClipFileTransfer::onChannelReadyRead()
{
  if (!m_channel)
    return;

  const QByteArray incoming = m_channel->readAll();
  if (incoming.isEmpty())
    return;

  m_decoder.append(reinterpret_cast<const std::uint8_t *>(incoming.constData()),
                   std::size_t(incoming.size()));

  ClipFrame frame;
  while (m_decoder.next(&frame)) {
    if (frame.kind == ClipFrameKind::Control)
      handleControl(frame.payload);
    else
      handleData(frame.payload);

    if (m_state == State::Failed || m_state == State::Cancelled)
      return;
  }

  if (m_decoder.failed())
    failed(Error::Protocol, QString::fromStdString(m_decoder.failureReason()));
}

void ClipFileTransfer::handleControl(const std::vector<std::uint8_t> &payload)
{
  QJsonParseError err{};
  const QByteArray raw(reinterpret_cast<const char *>(payload.data()), qsizetype(payload.size()));
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    failed(Error::Protocol, QStringLiteral("bad control frame: %1").arg(err.errorString()));
    return;
  }

  const QJsonObject msg = doc.object();
  const QString type = msg.value(QStringLiteral("type")).toString();

  if (type == QLatin1String("CLIP_OFFER")) {
    handleOffer(msg);
  } else if (type == QLatin1String("CLIP_ACCEPT")) {
    handleAccept(msg);
  } else if (type == QLatin1String("CLIP_REJECT")) {
    handleReject(msg);
  } else if (type == QLatin1String("CLIP_COMPLETE")) {
    handleComplete(msg);
  } else if (type == QLatin1String("CLIP_DONE")) {
    handleDone(msg);
  } else if (type == QLatin1String("CLIP_ERROR")) {
    handleRemoteError(msg);
  } else if (type == QLatin1String("CLIP_CANCEL")) {
    handleRemoteCancel(msg);
  } else if (type == QLatin1String("CLIP_HELLO")) {
    // capability greeting: nothing to negotiate yet
  } else {
    failed(Error::Protocol, QStringLiteral("unknown control type: %1").arg(type));
  }
}

void ClipFileTransfer::handleOffer(const QJsonObject &msg)
{
  if (m_state != State::Idle) {
    failed(Error::Protocol, QStringLiteral("busy: already %1").arg(toString(m_state)));
    return;
  }

  const QString transferId = msg.value(QStringLiteral("transfer_id")).toString();
  if (transferId.isEmpty()) {
    failed(Error::Protocol, QStringLiteral("offer without transfer_id"));
    return;
  }

  QList<ClipFileEntry> entries;
  qint64 total = 0;
  const QJsonArray array = msg.value(QStringLiteral("entries")).toArray();
  for (const QJsonValue &value : array) {
    const QJsonObject obj = value.toObject();
    const bool isDirectory = obj.value(QStringLiteral("is_dir")).toBool(false);
    const qint64 size = qint64(obj.value(QStringLiteral("size")).toDouble(0));
    const std::string normalized =
        normalizeRelativePath(toStdString(obj.value(QStringLiteral("rel_path")).toString().toUtf8()));

    if (!isSafeRelativePath(normalized)) {
      // never write outside the incoming dir; tell the peer and stop
      sendReject(transferId, QStringLiteral("PROTO"));
      failed(Error::Protocol, QStringLiteral("peer offered an unsafe path"));
      return;
    }

    ClipFileEntry entry;
    entry.relPath = toQString(normalized);
    entry.isDirectory = isDirectory;
    entry.size = isDirectory ? 0 : size;
    if (!isDirectory) {
      if (entry.size < 0 || std::uint64_t(entry.size) > kClipMaxFileBytes) {
        sendReject(transferId, QStringLiteral("SIZE"));
        failed(Error::TooLarge, QStringLiteral("peer offered %1 (%2 bytes)").arg(entry.relPath).arg(size));
        return;
      }
      total += entry.size;
    }
    entries.append(entry);
  }

  if (entries.isEmpty()) {
    failed(Error::Protocol, QStringLiteral("offer without entries"));
    return;
  }
  if (total > m_maxTransferBytes) {
    sendReject(transferId, QStringLiteral("SIZE"));
    failed(Error::TooLarge, QStringLiteral("offer exceeds max transfer size"));
    return;
  }
  if (m_incomingDir.isEmpty()) {
    sendReject(transferId, QStringLiteral("NO_DIR"));
    failed(Error::Io, QStringLiteral("no incoming directory configured"));
    return;
  }

  resetForNewTransfer();
  m_transferId = transferId;
  m_entries = entries;
  m_totalBytes = total;
  m_peerName = msg.value(QStringLiteral("device_name")).toString();

  const int peerChunk = msg.value(QStringLiteral("chunk_size")).toInt(0);
  if (peerChunk > 0)
    setChunkSize(peerChunk); // the receiver announces the agreed size back

  ClipFileOffer offer;
  offer.transferId = m_transferId;
  offer.peerName = m_peerName;
  offer.entries = m_entries;
  offer.totalBytes = m_totalBytes;
  offer.chunkSize = m_chunkSize;

  m_state = State::OfferReceived;
  m_progress = TransferProgress{0, std::uint64_t(m_totalBytes)};

  Q_EMIT offerReceived(offer);
  if (m_state == State::Failed || m_state == State::Cancelled)
    return;
  if (m_autoAccept)
    acceptOffer(0);
}

void ClipFileTransfer::handleAccept(const QJsonObject &msg)
{
  if (m_state != State::Offering) {
    failed(Error::Protocol, QStringLiteral("unexpected CLIP_ACCEPT while %1").arg(toString(m_state)));
    return;
  }
  if (msg.value(QStringLiteral("transfer_id")).toString() != m_transferId) {
    failed(Error::Protocol, QStringLiteral("CLIP_ACCEPT for another transfer"));
    return;
  }

  m_offerTimer->stop();

  const int peerChunk = msg.value(QStringLiteral("chunk_size")).toInt(0);
  if (peerChunk > 0) {
    m_chunkSize = qBound(int(kClipMinChunkSize), peerChunk, int(kClipMaxChunkSize));
  }

  const qint64 resumeFrom = qint64(msg.value(QStringLiteral("resume_from")).toDouble(0));
  m_resumeFrom = resumeFrom > 0 ? resumeFrom : 0;
  m_entryIndex = 0;
  m_bytesDone = m_resumeFrom > 0 ? std::uint64_t(m_resumeFrom) : 0;
  m_elapsed.restart();
  m_lastProgressMs = -1000;

  if (!openOutgoingEntry(0, m_resumeFrom))
    return;

  m_state = State::Sending;
  pump();
}

void ClipFileTransfer::handleReject(const QJsonObject &msg)
{
  if (m_state != State::Offering)
    return;
  m_offerTimer->stop();
  QString reason = msg.value(QStringLiteral("reason")).toString();
  if (reason.isEmpty())
    reason = QStringLiteral("rejected by peer");
  cleanupPartial(false);
  m_state = State::Failed;
  m_lastError = Error::Rejected;
  Q_EMIT transferFailed(Error::Rejected, reason);
}

void ClipFileTransfer::handleDone(const QJsonObject &msg)
{
  if (m_state != State::AwaitingAck) {
    failed(Error::Protocol, QStringLiteral("unexpected CLIP_DONE while %1").arg(toString(m_state)));
    return;
  }
  if (msg.value(QStringLiteral("transfer_id")).toString() != m_transferId) {
    failed(Error::Protocol, QStringLiteral("CLIP_DONE for another transfer"));
    return;
  }

  const int index = msg.value(QStringLiteral("entry_index")).toInt(-1);
  if (index != m_entryIndex) {
    failed(Error::Protocol, QStringLiteral("CLIP_DONE for entry %1, expected %2").arg(index).arg(m_entryIndex));
    return;
  }
  if (!msg.value(QStringLiteral("ok")).toBool(true)) {
    failed(Error::PeerError, msg.value(QStringLiteral("detail")).toString());
    return;
  }

  const QString localPath = m_entryLocalPaths.value(m_entryIndex);
  Q_EMIT entryCompleted(m_entryIndex, localPath);

  m_entryIndex += 1;
  m_entryOpen = false;
  m_entryOffset = 0;
  m_entryBytesDone = 0;
  m_nextSeq = 0;
  m_resumeFrom = 0;

  if (m_entryIndex >= m_entries.size()) {
    m_state = State::Completed;
    emitProgress(true);
    Q_EMIT transferCompleted(m_sourcePaths, m_transferId);
    return;
  }

  m_state = State::Sending;
  pump();
}

void ClipFileTransfer::handleComplete(const QJsonObject &msg)
{
  if (m_state != State::Receiving) {
    failed(Error::Protocol, QStringLiteral("unexpected CLIP_COMPLETE while %1").arg(toString(m_state)));
    return;
  }

  const int index = msg.value(QStringLiteral("entry_index")).toInt(-1);
  if (index != m_entryIndex) {
    failed(Error::Protocol,
           QStringLiteral("CLIP_COMPLETE for entry %1, expected %2").arg(index).arg(m_entryIndex));
    return;
  }
  if (msg.value(QStringLiteral("transfer_id")).toString() != m_transferId) {
    failed(Error::Protocol, QStringLiteral("CLIP_COMPLETE for another transfer"));
    return;
  }

  const ClipFileEntry entry = m_entries.value(m_entryIndex);
  const QString peerSha1 = msg.value(QStringLiteral("sha1")).toString();
  const QString ourSha1 = toQString(m_fileHasher.hex());

  if (entry.isDirectory) {
    Q_EMIT entryCompleted(m_entryIndex, m_destPaths.value(m_entryIndex).finalPath);
    m_completedPaths.append(m_destPaths.value(m_entryIndex).finalPath);
  } else {
    if (m_entryOffset != std::uint64_t(entry.size)) {
      sendError(QStringLiteral("SIZE"));
      failed(Error::Protocol,
             QStringLiteral("%1 expected %2 bytes, got %3")
                 .arg(entry.relPath)
                 .arg(entry.size)
                 .arg(m_entryOffset));
      return;
    }
    if (ourSha1.compare(peerSha1, Qt::CaseInsensitive) != 0) {
      sendError(QStringLiteral("CHECKSUM"));
      failed(Error::Checksum,
             QStringLiteral("%1 sha1 mismatch: peer %2, local %3").arg(entry.relPath, peerSha1, ourSha1));
      return;
    }

    const DestPath dest = m_destPaths.value(m_entryIndex);
    if (m_outFile) {
      m_outFile->flush();
      m_outFile->close();
      m_outFile.reset();
    }

    QFile::remove(dest.finalPath);
    if (!QFile::rename(dest.partPath, dest.finalPath)) {
      failed(Error::Io, QStringLiteral("cannot move %1 into place").arg(dest.partPath));
      return;
    }
    m_completedPaths.append(dest.finalPath);
    Q_EMIT entryCompleted(m_entryIndex, dest.finalPath);
  }

  QJsonObject done = baseMessage(QStringLiteral("CLIP_DONE"), m_transferId);
  done.insert(QStringLiteral("entry_index"), m_entryIndex);
  done.insert(QStringLiteral("sha1"), peerSha1);
  done.insert(QStringLiteral("ok"), true);
  sendJson(done);

  m_entryIndex += 1;
  m_entryOpen = false;
  m_entryOffset = 0;
  m_entryBytesDone = 0;
  m_nextSeq = 0;

  if (m_entryIndex >= m_entries.size()) {
    m_state = State::Completed;
    emitProgress(true);
    Q_EMIT transferCompleted(m_completedPaths, m_transferId);
    return;
  }

  if (!openIncomingEntry(m_entryIndex, 0))
    return;
}

void ClipFileTransfer::handleData(const std::vector<std::uint8_t> &payload)
{
  if (m_state != State::Receiving) {
    failed(Error::Protocol, QStringLiteral("data frame while %1").arg(toString(m_state)));
    return;
  }

  ChunkHeader header;
  const std::uint8_t *data = nullptr;
  std::size_t dataLen = 0;
  if (!decodeDataPayload(payload, &header, &data, &dataLen)) {
    sendError(QStringLiteral("PROTO"));
    failed(Error::Protocol, QStringLiteral("malformed chunk frame"));
    return;
  }
  if (header.seq != m_nextSeq || header.offset != m_entryOffset) {
    sendError(QStringLiteral("PROTO"));
    failed(Error::Protocol, QStringLiteral("out-of-order chunk: seq %1 offset %2")
                                 .arg(header.seq)
                                 .arg(qulonglong(header.offset)));
    return;
  }
  if (!chunkDigestMatches(header, data)) {
    sendError(QStringLiteral("CHECKSUM"));
    failed(Error::Checksum, QStringLiteral("chunk %1 failed its sha1 check").arg(header.seq));
    return;
  }

  const ClipFileEntry entry = m_entries.value(m_entryIndex);
  if (entry.isDirectory) {
    if (dataLen != 0) {
      failed(Error::Protocol, QStringLiteral("peer sent %1 bytes for directory %2")
                                  .arg(dataLen)
                                  .arg(entry.relPath));
      return;
    }
  } else {
    if (!m_outFile || !m_outFile->isOpen()) {
      failed(Error::Io, QStringLiteral("destination is not open"));
      return;
    }
    const qint64 written = m_outFile->write(reinterpret_cast<const char *>(data), qint64(dataLen));
    if (written != qint64(dataLen)) {
      failed(Error::Io, QStringLiteral("write failed: %1").arg(m_outFile->errorString()));
      return;
    }
  }

  m_fileHasher.update(data, dataLen);
  m_entryOffset += std::uint64_t(dataLen);
  m_entryBytesDone += std::uint64_t(dataLen);
  m_bytesDone += std::uint64_t(dataLen);
  ++m_nextSeq;
  emitProgress();

  if (m_entryOffset > std::uint64_t(entry.size)) {
    sendError(QStringLiteral("PROTO"));
    failed(Error::Protocol, QStringLiteral("peer sent more bytes than offered for %1").arg(entry.relPath));
  }
}

void ClipFileTransfer::handleRemoteError(const QJsonObject &msg)
{
  const QString code = msg.value(QStringLiteral("code")).toString();
  const QString detail = msg.value(QStringLiteral("detail")).toString();
  Error error = Error::PeerError;
  if (code == QLatin1String("CHECKSUM"))
    error = Error::Checksum;
  else if (code == QLatin1String("IO"))
    error = Error::Io;
  else if (code == QLatin1String("PROTO"))
    error = Error::Protocol;
  failed(error, detail.isEmpty() ? QStringLiteral("peer error: %1").arg(code) : detail);
}

void ClipFileTransfer::handleRemoteCancel(const QJsonObject &msg)
{
  Q_UNUSED(msg);
  if (!isActive())
    return;
  cleanupPartial(true);
  m_state = State::Cancelled;
  Q_EMIT transferCancelled(m_transferId);
}

} // namespace litekvm
