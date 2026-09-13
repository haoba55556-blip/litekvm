// SPDX-License-Identifier: GPL-2.0
// LiteKVM cross-machine file clipboard — chunked file transfer over an
// already paired (encrypted) data channel.
//
// Spec: docs/plans/2026-08-25-litekvm-mvp.md Phase 4 Task 4.1 / 4.2
//   * clipboard file event -> metadata + 256 KiB chunks, sha1 per chunk
//   * receiver lands in a temp file, verifies, then renames into place
//   * >100 MB shows a progress bar and can be cancelled
//   * UTF-8 file names pass through unchanged (Phase 4.2)
//
// Wire format lives in ClipFileChunk.h (Qt-free, unit-tested on its own):
// one leading kind byte plus the same big-endian length prefix PairingService
// uses, 256 KiB chunks, SHA-1 per chunk and over the whole file.
//
// Usage — sender:
//   auto *tx = new ClipFileTransfer(deskflowDataStream, this);
//   connect(tx, &ClipFileTransfer::progressChanged, ui, &ProgressDialog::setProgress);
//   tx->sendLocalPaths({"/home/me/报告.docx"});
// Usage — receiver:
//   auto *rx = new ClipFileTransfer(deskflowDataStream, this);
//   rx->setIncomingDirectory(ClipFileTransfer::defaultIncomingDirectory());
//   connect(rx, &ClipFileTransfer::offerReceived, ui, &ClipPrompt::showOffer);
//   // ui confirms with rx->acceptOffer() or rx->rejectOffer()
#pragma once

#include "ClipFileChunk.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class QFile;
class QIODevice;
class QTimer;

namespace litekvm {

/// One file (or one directory) inside a transfer. Wire metadata only — the
/// sender's absolute source path is kept aside so it never hits the wire.
struct ClipFileEntry {
  QString relPath; ///< UTF-8 relative path, '/' separated ("sub/中文.txt")
  qint64 size = 0;
  bool isDirectory = false;
};

struct ClipFileOffer {
  QString transferId;
  QString peerName;
  QList<ClipFileEntry> entries;
  qint64 totalBytes = 0;
  int chunkSize = 0;
};

/**
 * @brief Chunked file sender/receiver for the cross-machine clipboard.
 *
 * One transfer at a time per instance. Both roles live here: the initiator
 * stages local paths and sends CLIP_OFFER, the peer answers CLIP_ACCEPT /
 * CLIP_REJECT, then files stream entry by entry. Every entry ends with
 * CLIP_COMPLETE (sender's whole-file sha1) followed by CLIP_DONE (receiver
 * verified it), so the sender only reports success once the bytes are verified
 * on disk at the other end.
 */
class ClipFileTransfer : public QObject {
  Q_OBJECT

public:
  enum class Error {
    None,
    Io,
    Checksum,
    Protocol,
    Rejected,
    Cancelled,
    TooLarge,
    Timeout,
    PeerError,
  };
  Q_ENUM(Error)
  static QString toString(Error error);

  enum class State {
    Idle,
    Offering,     ///< sender: CLIP_OFFER sent, waiting for CLIP_ACCEPT
    OfferReceived, ///< receiver: CLIP_OFFER parsed, waiting for the user
    Sending,      ///< sender: streaming chunks
    AwaitingAck,  ///< sender: CLIP_COMPLETE sent, waiting for CLIP_DONE
    Receiving,    ///< receiver: writing chunks
    Completed,
    Cancelled,
    Failed,
  };
  Q_ENUM(State)
  static QString toString(State state);

  explicit ClipFileTransfer(QIODevice *channel, QObject *parent = nullptr);
  ~ClipFileTransfer() override;

  /// Standalone control-plane port when not riding the deskflow data channel
  /// (PairingService uses 25901, LayoutSync 25902).
  static constexpr quint16 defaultPort()
  {
    return 25903;
  }
  static int defaultChunkSize();
  /// Temp directory files land in until the user accepts them.
  static QString defaultIncomingDirectory();
  /// Streaming SHA-1 of a local file, lowercase hex ("" on failure).
  static QString fileSha1(const QString &path);

  // ------------------------------------------------------------- settings
  void setChunkSize(int bytes);
  int chunkSize() const
  {
    return m_chunkSize;
  }
  void setIncomingDirectory(const QString &dir);
  QString incomingDirectory() const
  {
    return m_incomingDir;
  }
  void setMaxTransferBytes(qint64 bytes);
  qint64 maxTransferBytes() const
  {
    return m_maxTransferBytes;
  }
  /// Accept incoming offers without asking the UI (tests, trusted peers).
  void setAutoAccept(bool on)
  {
    m_autoAccept = on;
  }
  bool autoAccept() const
  {
    return m_autoAccept;
  }
  /// 0 disables the offer timeout.
  void setOfferTimeout(int ms);
  /// Stop queueing new chunks above this many bytes in flight.
  void setHighWaterMark(int bytes);

  // --------------------------------------------------------------- status
  State state() const
  {
    return m_state;
  }
  Error lastError() const
  {
    return m_lastError;
  }
  bool isActive() const;
  QString transferId() const
  {
    return m_transferId;
  }
  TransferProgress progress() const
  {
    return m_progress;
  }
  const QList<ClipFileEntry> &entries() const
  {
    return m_entries;
  }
  QStringList sourcePaths() const
  {
    return m_sourcePaths;
  }
  QStringList completedPaths() const
  {
    return m_completedPaths;
  }

  // --------------------------------------------------------------- sender
  /// Stages files and/or directories (directories are walked recursively) and
  /// sends CLIP_OFFER. Fails with Error::Protocol when called while not Idle.
  void sendLocalPaths(const QStringList &paths);

  // ------------------------------------------------------------- receiver
  /// Answer CLIP_OFFER. @p resumeFrom must be chunk-aligned and applies to the
  /// first file entry only (the receiver needs the matching .litekvm-part file
  /// on disk for the prefix to be kept).
  void acceptOffer(qint64 resumeFrom = 0);
  void rejectOffer(const QString &reason = QStringLiteral("USER"));

  // ----------------------------------------------------------------- both
  /// Cancels the transfer in either role, tells the peer, and removes the
  /// half-written temp file.
  void cancel();

Q_SIGNALS:
  void offerReceived(const litekvm::ClipFileOffer &offer);
  void progressChanged(const litekvm::TransferProgress &progress);
  void entryCompleted(int entryIndex, const QString &localPath);
  void transferCompleted(const QStringList &localPaths, const QString &transferId);
  void transferCancelled(const QString &transferId);
  void transferFailed(litekvm::ClipFileTransfer::Error error, const QString &detail);

private Q_SLOTS:
  void onChannelReadyRead();
  void onOfferTimeout();
  void pump();

private:
  struct DestPath {
    QString finalPath;
    QString partPath;
  };

  // ---- helpers
  void failed(Error error, const QString &detail);
  void resetForNewTransfer();
  void cleanupPartial(bool removePartFile);
  void sendJson(const QJsonObject &msg);
  void sendError(const QString &code, const QString &detail = QString());
  void sendReject(const QString &transferId, const QString &reason);
  void emitProgress(bool force = false);
  static QJsonObject baseMessage(const QString &type, const QString &transferId);

  static QString newTransferId();
  static QByteArray toQByteArray(const std::vector<std::uint8_t> &data);
  static QString toQString(const std::string &utf8);
  static std::string toStdString(const QByteArray &utf8);
  static bool hashPrefix(const QString &path, std::uint64_t count, clipsha1::Sha1 *hasher);
  static QString uniqueDestinationPath(const QString &desired);

  // ---- staging
  bool appendEntry(const QString &relPath, qint64 size, bool isDirectory, const QString &localPath,
                   qint64 *totalBytes);
  bool stagePaths(const QStringList &paths);

  // ---- sender
  bool openOutgoingEntry(int index, qint64 resumeFrom);
  bool finishOutgoingEntry();

  // ---- receiver
  bool prepareDestinations();
  bool openIncomingEntry(int index, qint64 resumeFrom);

  // ---- frame handling
  void handleControl(const std::vector<std::uint8_t> &payload);
  void handleData(const std::vector<std::uint8_t> &payload);
  void handleOffer(const QJsonObject &msg);
  void handleAccept(const QJsonObject &msg);
  void handleReject(const QJsonObject &msg);
  void handleComplete(const QJsonObject &msg);
  void handleDone(const QJsonObject &msg);
  void handleRemoteError(const QJsonObject &msg);
  void handleRemoteCancel(const QJsonObject &msg);

  QIODevice *m_channel = nullptr;
  FrameDecoder m_decoder;

  State m_state = State::Idle;
  Error m_lastError = Error::None;

  int m_chunkSize = defaultChunkSize();
  QString m_incomingDir;
  qint64 m_maxTransferBytes = qint64(kClipDefaultMaxTransferBytes);
  bool m_autoAccept = false;
  int m_offerTimeoutMs = 30'000;
  int m_highWaterMark = 4 * 1024 * 1024;
  QTimer *m_offerTimer = nullptr;

  QString m_transferId;
  QString m_peerName;
  QList<ClipFileEntry> m_entries;
  QStringList m_entryLocalPaths; ///< sender only, index-aligned with m_entries
  QList<DestPath> m_destPaths;   ///< receiver only, index-aligned with m_entries
  QStringList m_sourcePaths;     ///< sender only: what the user asked for
  QStringList m_completedPaths;  ///< receiver: verified local results
  qint64 m_totalBytes = 0;

  int m_entryIndex = 0;
  ChunkPlan m_plan;
  std::uint64_t m_entryOffset = 0;
  std::uint64_t m_entryBytesDone = 0;
  std::uint64_t m_bytesDone = 0;
  std::uint32_t m_nextSeq = 0;
  bool m_entryOpen = false;
  qint64 m_resumeFrom = 0; ///< applies to the first entry only
  clipsha1::Sha1 m_fileHasher;

  std::unique_ptr<QFile> m_inFile;
  std::unique_ptr<QFile> m_outFile;

  TransferProgress m_progress;
  QElapsedTimer m_elapsed;
  qint64 m_lastProgressMs = -1000; // force the first progressChanged
};

} // namespace litekvm

Q_DECLARE_METATYPE(litekvm::TransferProgress)
Q_DECLARE_METATYPE(litekvm::ClipFileOffer)
