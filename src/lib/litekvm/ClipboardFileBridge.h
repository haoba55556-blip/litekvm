// SPDX-License-Identifier: GPL-2.0
// LiteKVM cross-machine file clipboard — the local clipboard end.
//
// Spec: docs/plans/2026-08-25-litekvm-mvp.md Phase 4 Task 4.3
//   * Ctrl+C on files/folders in Explorer -> detect it, offer to send
//   * files arriving from the peer -> land back on the clipboard so the user
//     can Ctrl+V them straight into a folder
//   * never re-send what we just pasted ourselves: both machines would bounce
//     the same files back and forth forever
//
// Usage — watching the local clipboard:
//   auto *bridge = new ClipboardFileBridge(this);
//   connect(bridge, &ClipboardFileBridge::localFilesCopied, this, &Foo::sendFiles);
//
// Usage — handing the peer's files to the user:
//   bridge->putFilesOnClipboard(transfer->completedPaths()); // user hits Ctrl+V
//
// Windows notes: Explorer puts "file:///C:/Users/..." URLs on the clipboard for
// a file copy, so QMimeData::hasUrls() plus QUrl::isLocalFile() is the whole
// test. The clipboard is exclusively owned — on Windows only one process can
// have it open — so a null QMimeData (someone else holds it right now) is
// treated as "no change" instead of an error.
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QClipboard;
class QMimeData;
class QTimer;
class QUrl;

namespace litekvm {

/**
 * @brief Bridges the OS clipboard and the cross-machine file transfer.
 *
 * Two directions, one object:
 *  - local -> peer: onClipboardDataChanged() looks at QClipboard::dataChanged,
 *    classifies the content, and emits localFilesCopied() when the user copied
 *    a set of local files/folders. The caller decides whether to send it.
 *  - peer -> local: putFilesOnClipboard() puts the received paths on the
 *    clipboard as file URLs, which is exactly the form Explorer pastes from.
 *
 * Loop suppression (the important part): putFilesOnClipboard() records what it
 * wrote and opens a short "self-write window" (default 3 s). Any clipboard
 * change arriving inside that window whose content equals one of the recorded
 * writes is our own echo coming back through dataChanged() and is dropped.
 * A change with different content is never dropped, so a genuine copy made
 * right after a paste still goes through.
 *
 * Everything is nullable-safe: with no QClipboard (no QGuiApplication, or
 * --no-gui) the object still classifies mime data and just never touches the
 * OS clipboard.
 */
class ClipboardFileBridge : public QObject {
  Q_OBJECT

public:
  /// How long after a self-write the guard keeps matching echoes (ms).
  static constexpr int kDefaultSelfWriteGuardMs = 3000;
  /// Floor/ceiling for setSelfWriteGuardMs().
  static constexpr int kMinSelfWriteGuardMs = 250;
  static constexpr int kMaxSelfWriteGuardMs = 60'000;
  /// How many of our own recent writes the guard still matches.
  static constexpr int kRecentWriteHistory = 4;

  /**
   * Watches the process clipboard (QGuiApplication::clipboard()). Nothing is
   * watched when there is no GUI application — headless tests, --no-gui.
   */
  explicit ClipboardFileBridge(QObject *parent = nullptr);

  /// QGuiApplication::clipboard(), or nullptr when there is no GUI application
  /// (unit tests, --no-gui). Safe to call at any point.
  static QClipboard *defaultClipboard();

  /**
   * Switches the watched clipboard. Null detaches the bridge from the OS
   * clipboard — classification and the loop guard keep working — which is how
   * tests stay off the real clipboard.
   *
   * A setter rather than a constructor overload on purpose: QClipboard derives
   * from QObject, so a `ClipboardFileBridge(QClipboard *)` overload would
   * silently win over `(QObject *parent)` for anyone passing a clipboard.
   */
  void setClipboard(QClipboard *clipboard);
  QClipboard *clipboard() const
  {
    return m_clipboard;
  }

  // --------------------------------------------------------- pure helpers

  /// True when @p mime is a copy of local files: it has URLs, at least one of
  /// them, and *every* URL is a local file (an https link dragged along with
  /// the files makes the set something else, not a file copy). Null -> false.
  static bool isLocalFileList(const QMimeData *mime);

  /// Local file paths carried by @p mime, native separators, in order, deduped.
  /// Non-local URLs (http, UNC file://server/...) and text-only mime data yield
  /// an empty list. Not to be confused with isLocalFileList(): this filters,
  /// that one requires the whole set to be local.
  static QStringList localPathsFromMime(const QMimeData *mime);

  /// Builds the "copied files" mime data for @p localPaths; the caller takes
  /// ownership. Null when nothing usable was passed. Accepts native paths as
  /// well as "file:" URLs, so it can be fed either end of the pipeline.
  static QMimeData *makeFileMimeData(const QStringList &localPaths);

  /// file:///... URL -> "C:\Users\好好\报告.docx" (percent-decoding included,
  /// nothing is lost). Empty for non-local URLs.
  static QString toLocalFilePath(const QUrl &url);

  /// "C:\...\报告.docx" (or an already-formed file: URL) -> the file URL to put
  /// on the clipboard. Backslashes are accepted, so Windows paths survive.
  static QUrl toFileUrl(const QString &localPath);

  /// Native-separator, deduped, junk-free form of @p localPaths: empties are
  /// dropped and anything that is a URL to somewhere else (http, smb,
  /// file://server/share) is rejected. Used for both sides of the loop guard so
  /// the comparison matches byte for byte.
  static QStringList normalizedPaths(const QStringList &localPaths);

  // ------------------------------------------------------------- watching

  /// True when the watched clipboard currently holds a set of local files.
  bool isFileClipboard() const
  {
    return m_isFileClipboard;
  }

  /// The set reported by the last localFilesCopied() (empty otherwise).
  QStringList currentLocalFiles() const
  {
    return m_localFiles;
  }

  /// Turn detection off (e.g. while the caller is replaying a remote paste and
  /// does not want the local copy reported). Writes still work.
  void setWatchEnabled(bool on)
  {
    m_watchEnabled = on;
  }
  bool watchEnabled() const
  {
    return m_watchEnabled;
  }

  // -------------------------------------------------------------- writing

  /**
   * Puts @p localPaths on the clipboard as file URLs, so Ctrl+V in Explorer
   * pastes them. Returns false when there is nothing to write (empty list) or
   * no clipboard is available; returns true once the write was handed to Qt,
   * which is as much as can be observed (a failed write due to another process
   * holding the clipboard is corrected by the next dataChanged()).
   *
   * Opens the loop guard, so the echo this write causes is not reported as a
   * local copy.
   */
  bool putFilesOnClipboard(const QStringList &localPaths);

  // ---------------------------------------------------------- loop guard

  /// Window length for matching our own echo. <= 0 disables the guard entirely
  /// (the caller must then break the loop itself).
  void setSelfWriteGuardMs(int ms);
  int selfWriteGuardMs() const
  {
    return m_guardMs;
  }
  /// True while the guard window opened by the last self-write is open.
  bool selfWriteGuardActive() const;
  /// True when @p localPaths is the echo of one of our own recent writes made
  /// inside the open guard window — i.e. this must not be sent.
  bool isEchoOfOwnWrite(const QStringList &localPaths) const;

  // ---------------------------------------------------------------- seams

  /**
   * The body of onClipboardDataChanged() with the clipboard passed in, so it
   * can be driven without a live clipboard (unit tests / foreign content).
   * Does not take ownership and never keeps @p mime; returns true when
   * localFilesCopied() was emitted.
   */
  bool handleClipboardData(const QMimeData *mime);

  /// Re-reads the clipboard right now (primes isFileClipboard() at start-up).
  void refresh();

public Q_SLOTS:
  /// Connected to QClipboard::dataChanged.
  void onClipboardDataChanged();

  /**
   * Opens the guard window as if @p writtenPaths had just been written to the
   * clipboard. Public because tests drive it directly; putFilesOnClipboard()
   * calls it too. No-op when the guard is disabled.
   */
  void armSelfWriteGuard(const QStringList &writtenPaths);

Q_SIGNALS:
  /// The user copied local files — the caller decides whether to send them.
  void localFilesCopied(const QStringList &localPaths);
  /// isFileClipboard() flipped (UI can enable/disable a "send files" action).
  void fileClipboardStateChanged(bool isFileClipboard);

private Q_SLOTS:
  void onSelfWriteGuardExpired();

private:
  void setFileClipboardState(bool isFileClipboard);

  QClipboard *m_clipboard = nullptr; ///< not owned; may be null
  QTimer *m_guardTimer = nullptr;    ///< single shot, opens/closes the window
  QList<QStringList> m_recentWrites; ///< newest first, cleared on expiry
  QStringList m_localFiles;
  bool m_isFileClipboard = false;
  bool m_watchEnabled = true;
  int m_guardMs = kDefaultSelfWriteGuardMs;
};

} // namespace litekvm
