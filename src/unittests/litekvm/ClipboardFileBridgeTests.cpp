// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// ClipboardFileBridge unit tests: what counts as a "copied local files" mime,
// URL <-> path conversion (Chinese names, spaces, percent-encoding, Windows
// separators) and the self-write loop guard that stops two machines from
// bouncing the same files forever.
//
// The OS clipboard itself is only used by liveClipboardWriteIsNotReEmitted(),
// which skips itself when no GUI clipboard exists.
#include "../../lib/litekvm/ClipboardFileBridge.h"

#include <QClipboard>
#include <QDir>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <memory>

using namespace litekvm;

namespace {

/// Native-separator path, i.e. what Explorer shows: C:\...\报告.docx on
/// Windows, /tmp/报告.docx elsewhere.
QString nativePath(const QString &dir, const QString &name)
{
  return QDir::toNativeSeparators(dir + QLatin1Char('/') + name);
}

/// Names that exercise the cases a naive path<->URL conversion breaks on:
/// Chinese (UTF-8 percent-encoding), spaces, and characters that are reserved
/// or special inside a URL.
QStringList trickyNames()
{
  return {
      QStringLiteral("跨机 报告 1.docx"),
      QStringLiteral("a#b&c+d%e.bin"),
      QStringLiteral("100% 已确认 文件夹"),
  };
}

QStringList trickyPaths(const QString &dir)
{
  QStringList out;
  for (const QString &name : trickyNames())
    out.append(nativePath(dir, name));
  return out;
}

} // namespace

class ClipboardFileBridgeTests : public QObject {
  Q_OBJECT

private Q_SLOTS:

  // ------------------------------------------------------------ detection

  void whatCountsAsALocalFileCopy()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList files = trickyPaths(dir.path());

    // Explorer's Ctrl+C: a set of file:/// URLs, all of them local files
    {
      std::unique_ptr<QMimeData> mime(ClipboardFileBridge::makeFileMimeData(files));
      QVERIFY(mime != nullptr);
      QVERIFY(ClipboardFileBridge::isLocalFileList(mime.get()));
      QCOMPARE(ClipboardFileBridge::localPathsFromMime(mime.get()), files);
    }

    // one file is just as valid as three
    {
      std::unique_ptr<QMimeData> mime(ClipboardFileBridge::makeFileMimeData({files.at(0)}));
      QVERIFY(ClipboardFileBridge::isLocalFileList(mime.get()));
      QCOMPARE(ClipboardFileBridge::localPathsFromMime(mime.get()), QStringList{files.at(0)});
    }

    // a browser link is not a file copy
    {
      QMimeData mime;
      mime.setUrls({QUrl(QStringLiteral("https://example.com/%E6%8A%A5%E5%91%8A.docx"))});
      QVERIFY(!ClipboardFileBridge::isLocalFileList(&mime));
      QVERIFY(ClipboardFileBridge::localPathsFromMime(&mime).isEmpty());
    }

    // mixed set: NOT a file copy, but the local member is still recoverable
    {
      QMimeData mime;
      mime.setUrls({QUrl::fromLocalFile(files.at(0)), QUrl(QStringLiteral("https://example.com/x"))});
      QVERIFY(!ClipboardFileBridge::isLocalFileList(&mime));
      QCOMPARE(ClipboardFileBridge::localPathsFromMime(&mime), QStringList{files.at(0)});
    }

    // a UNC share (file://server/share) is remote, not a local file
    {
      QMimeData mime;
      mime.setUrls({QUrl(QStringLiteral("file://fileserver/%E5%85%B1%E4%BA%AB/a.docx"))});
      QVERIFY(!ClipboardFileBridge::isLocalFileList(&mime));
      QVERIFY(ClipboardFileBridge::localPathsFromMime(&mime).isEmpty());
    }

    // text, an image, an empty clipboard, a null one, a bare url
    {
      QMimeData text;
      text.setText(QStringLiteral("C:\\只是文字\\x.txt"));
      QVERIFY(!ClipboardFileBridge::isLocalFileList(&text));
      QVERIFY(ClipboardFileBridge::localPathsFromMime(&text).isEmpty());

      QMimeData image;
      image.setData(QStringLiteral("image/png"), QByteArray("\x89PNG\r\n\x1a\n", 8));
      QVERIFY(!ClipboardFileBridge::isLocalFileList(&image));

      QMimeData empty;
      QVERIFY(!ClipboardFileBridge::isLocalFileList(&empty));
      empty.setUrls({});
      QVERIFY(!ClipboardFileBridge::isLocalFileList(&empty));

      QMimeData blank;
      blank.setUrls({QUrl()}); // an invalid/empty url is not a local file
      QVERIFY(!ClipboardFileBridge::isLocalFileList(&blank));
      QVERIFY(ClipboardFileBridge::localPathsFromMime(&blank).isEmpty());

      QVERIFY(!ClipboardFileBridge::isLocalFileList(nullptr));
      QVERIFY(ClipboardFileBridge::localPathsFromMime(nullptr).isEmpty());
    }
  }

  // ------------------------------------------------------- path conversion

  void pathUrlRoundTripKeepsEveryCharacter()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList files = trickyPaths(dir.path());

    std::unique_ptr<QMimeData> mime(ClipboardFileBridge::makeFileMimeData(files));
    QVERIFY(mime != nullptr);
    QCOMPARE(mime->urls().size(), files.size());

    // nothing lost, nothing reordered
    QCOMPARE(ClipboardFileBridge::localPathsFromMime(mime.get()), files);

    // the conversion really goes through URL encoding: Chinese must be
    // percent-encoded in the URL itself and decoded again on the way back
    // toString() 默认返回美化解码形式（中文原样显示），要看百分号编码必须显式
    // 要 QUrl::FullyEncoded。
    const QString encoded = mime->urls().at(0).toString(QUrl::FullyEncoded);
    QVERIFY(encoded.startsWith(QStringLiteral("file:")));
    QVERIFY(encoded.contains(QLatin1Char('%')));
    QVERIFY(!encoded.contains(QStringLiteral("跨机")));

    // every pair round-trips on its own, including the already-URL form
    for (const QString &path : files) {
      const QUrl url = ClipboardFileBridge::toFileUrl(path);
      QVERIFY(url.isLocalFile());
      QCOMPARE(ClipboardFileBridge::toLocalFilePath(url), path);
      // feeding the URL's string form back in must not double-encode
      QCOMPARE(ClipboardFileBridge::toFileUrl(url.toString()), url);
      QCOMPARE(ClipboardFileBridge::toLocalFilePath(ClipboardFileBridge::toFileUrl(url.toString())), path);
    }

#ifdef Q_OS_WIN
    // Windows paths keep their backslashes through the round trip
    for (const QString &path : files) {
      QVERIFY(path.contains(QLatin1Char('\\')));
      QVERIFY(ClipboardFileBridge::toLocalFilePath(ClipboardFileBridge::toFileUrl(path)).contains(QLatin1Char('\\')));
    }
#else
    QVERIFY(!files.at(0).contains(QLatin1Char('\\')));
#endif

    // non-local urls yield no path at all
    QVERIFY(ClipboardFileBridge::toLocalFilePath(QUrl(QStringLiteral("https://example.com/a"))).isEmpty());
    QVERIFY(ClipboardFileBridge::toLocalFilePath(QUrl()).isEmpty());
  }

  void normalizedPathsAcceptsUrlsAndDropsJunk()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = nativePath(dir.path(), QStringLiteral("跨机 报告 1.docx"));

    const QStringList mixed{
        path,
        QUrl::fromLocalFile(path).toString(), // same file as a file: URL
        QString(),
        QStringLiteral("   "),
        QStringLiteral("https://example.com/remote.docx"),
        QDir::toNativeSeparators(path), // duplicate
    };
    QCOMPARE(ClipboardFileBridge::normalizedPaths(mixed), QStringList{path});

    // a Windows drive-letter path must not be mistaken for a URL scheme
    const QString drivePath = QDir::toNativeSeparators(QStringLiteral("C:/Users/未命名/报告.txt"));
    QCOMPARE(ClipboardFileBridge::normalizedPaths({QStringLiteral("C:/Users/未命名/报告.txt")}),
             QStringList{drivePath});

    QVERIFY(ClipboardFileBridge::normalizedPaths({}).isEmpty());
    QVERIFY(ClipboardFileBridge::normalizedPaths({QString(), QStringLiteral(" ")}).isEmpty());
    QVERIFY(ClipboardFileBridge::makeFileMimeData({}) == nullptr);
    QVERIFY(ClipboardFileBridge::makeFileMimeData({QString(), QStringLiteral("\t")}) == nullptr);
    // a file: URL that is not local cannot become mime data either
    QVERIFY(ClipboardFileBridge::makeFileMimeData({QStringLiteral("https://example.com/x")}) == nullptr);
  }

  // -------------------------------------------------------------- signals

  void fileCopyIsReportedAndNonFileCopyClearsState()
  {
    // a null clipboard is the supported headless mode: classification still
    // works, the OS clipboard is simply never touched
    ClipboardFileBridge bridge;
    bridge.setClipboard(nullptr); // keep the real OS clipboard out of this test
    QVERIFY(bridge.watchEnabled());
    QVERIFY(!bridge.isFileClipboard());
    QVERIFY(bridge.currentLocalFiles().isEmpty());

    QSignalSpy copied(&bridge, &ClipboardFileBridge::localFilesCopied);
    QSignalSpy stateChanged(&bridge, &ClipboardFileBridge::fileClipboardStateChanged);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList files = trickyPaths(dir.path());
    std::unique_ptr<QMimeData> mime(ClipboardFileBridge::makeFileMimeData(files));

    QVERIFY(bridge.handleClipboardData(mime.get()));
    QCOMPARE(copied.count(), 1);
    QCOMPARE(copied.at(0).at(0).value<QStringList>(), files);
    QVERIFY(bridge.isFileClipboard());
    QCOMPARE(bridge.currentLocalFiles(), files);
    QCOMPARE(stateChanged.count(), 1);
    QCOMPARE(stateChanged.at(0).at(0).toBool(), true);

    // the same files copied again is a new local copy (only our own write is
    // suppressed, and nothing wrote here)
    QVERIFY(bridge.handleClipboardData(mime.get()));
    QCOMPARE(copied.count(), 2);
    QCOMPARE(stateChanged.count(), 1); // no state transition

    // copying text replaces the file clipboard: no signal, state clears
    QMimeData text;
    text.setText(QStringLiteral("hello"));
    QVERIFY(!bridge.handleClipboardData(&text));
    QCOMPARE(copied.count(), 2);
    QVERIFY(!bridge.isFileClipboard());
    QVERIFY(bridge.currentLocalFiles().isEmpty());
    QCOMPARE(stateChanged.count(), 2);
    QCOMPARE(stateChanged.at(1).at(0).toBool(), false);

    // a null mime (clipboard held by another process, or empty) is silence,
    // never a crash, and never a state change
    QVERIFY(!bridge.handleClipboardData(nullptr));
    QCOMPARE(copied.count(), 2);
    QCOMPARE(stateChanged.count(), 2);

    // watching can be switched off (state already cleared above, so no signal)
    bridge.setWatchEnabled(false);
    QVERIFY(!bridge.watchEnabled());
    QVERIFY(!bridge.handleClipboardData(mime.get()));
    QCOMPARE(copied.count(), 2);
    bridge.setWatchEnabled(true);
    QVERIFY(bridge.handleClipboardData(mime.get()));
    QCOMPARE(copied.count(), 3);
  }

  // ------------------------------------------------------------ loop guard

  void ownWriteEchoIsNotSentBackAndOtherCopiesStillAre()
  {
    ClipboardFileBridge bridge;
    bridge.setClipboard(nullptr); // keep the real OS clipboard out of this test
    QSignalSpy copied(&bridge, &ClipboardFileBridge::localFilesCopied);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList mine{nativePath(dir.path(), QStringLiteral("收到的 报告.docx"))};
    const QStringList theirs{nativePath(dir.path(), QStringLiteral("用户新选的.bin"))};

    QVERIFY(!bridge.selfWriteGuardActive());
    bridge.armSelfWriteGuard(mine); // as putFilesOnClipboard() does after a paste
    QVERIFY(bridge.selfWriteGuardActive());
    QVERIFY(bridge.isEchoOfOwnWrite(mine));
    QVERIFY(!bridge.isEchoOfOwnWrite(theirs));

    // the echo of our own write must not be reported: this is the whole
    // anti-ping-pong guarantee
    std::unique_ptr<QMimeData> echo(ClipboardFileBridge::makeFileMimeData(mine));
    QVERIFY(!bridge.handleClipboardData(echo.get()));
    QCOMPARE(copied.count(), 0);

    // a genuine copy made right after the paste is NOT swallowed
    std::unique_ptr<QMimeData> other(ClipboardFileBridge::makeFileMimeData(theirs));
    QVERIFY(bridge.handleClipboardData(other.get()));
    QCOMPARE(copied.count(), 1);
    QCOMPARE(copied.at(0).at(0).value<QStringList>(), theirs);

    // Windows can deliver more than one dataChanged for a single write; the
    // duplicate echo is still recognised while the window is open
    QVERIFY(!bridge.handleClipboardData(echo.get()));
    QCOMPARE(copied.count(), 1);

    // non-file content inside the window is neither swallowed nor reported
    QMimeData text;
    text.setText(QStringLiteral("x"));
    QVERIFY(!bridge.handleClipboardData(&text));
    QCOMPARE(copied.count(), 1);

    // content that only *resembles* our write (extra file) is a real copy
    const QStringList mineAndMore{mine.at(0), theirs.at(0)};
    std::unique_ptr<QMimeData> combined(ClipboardFileBridge::makeFileMimeData(mineAndMore));
    QVERIFY(bridge.handleClipboardData(combined.get()));
    QCOMPARE(copied.count(), 2);
  }

  void guardIsBoundedAndCanBeDisabled()
  {
    ClipboardFileBridge bridge;
    bridge.setClipboard(nullptr); // keep the real OS clipboard out of this test
    QSignalSpy copied(&bridge, &ClipboardFileBridge::localFilesCopied);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList files = trickyPaths(dir.path());
    std::unique_ptr<QMimeData> echo(ClipboardFileBridge::makeFileMimeData(files));

    // default and clamping
    QCOMPARE(bridge.selfWriteGuardMs(), ClipboardFileBridge::kDefaultSelfWriteGuardMs);
    bridge.setSelfWriteGuardMs(10); // below the floor
    QCOMPARE(bridge.selfWriteGuardMs(), ClipboardFileBridge::kMinSelfWriteGuardMs);
    bridge.setSelfWriteGuardMs(10 * 60'000); // above the ceiling
    QCOMPARE(bridge.selfWriteGuardMs(), ClipboardFileBridge::kMaxSelfWriteGuardMs);
    bridge.setSelfWriteGuardMs(1000);
    QCOMPARE(bridge.selfWriteGuardMs(), 1000);

    // disabling the guard disables both the window and the echo matching
    bridge.setSelfWriteGuardMs(0);
    QCOMPARE(bridge.selfWriteGuardMs(), 0);
    bridge.armSelfWriteGuard(files);
    QVERIFY(!bridge.selfWriteGuardActive());
    QVERIFY(!bridge.isEchoOfOwnWrite(files));
    QVERIFY(bridge.handleClipboardData(echo.get()));
    QCOMPARE(copied.count(), 1);

    // arming with nothing (or with junk) is a no-op, never a crash
    bridge.setSelfWriteGuardMs(1000);
    bridge.armSelfWriteGuard({});
    QVERIFY(!bridge.selfWriteGuardActive());
    bridge.armSelfWriteGuard({QString(), QStringLiteral("   ")});
    QVERIFY(!bridge.selfWriteGuardActive());

    // no clipboard in this process: writes are refused and arm nothing, and a
    // refused write must not touch the recorded state either
    QVERIFY(bridge.clipboard() == nullptr);
    const bool wasFileClipboard = bridge.isFileClipboard();
    QCOMPARE(bridge.putFilesOnClipboard(files), false);
    QCOMPARE(bridge.putFilesOnClipboard({}), false);
    QVERIFY(!bridge.selfWriteGuardActive());
    QCOMPARE(bridge.isFileClipboard(), wasFileClipboard);

    // detaching is idempotent and never clears the guard by itself
    bridge.setClipboard(nullptr);
    QVERIFY(bridge.clipboard() == nullptr);
    QVERIFY(!bridge.selfWriteGuardActive());
  }

  void guardExpiresSoTheSameFilesCanBeSentAgain()
  {
    ClipboardFileBridge bridge;
    bridge.setClipboard(nullptr); // keep the real OS clipboard out of this test
    bridge.setSelfWriteGuardMs(300);
    QSignalSpy copied(&bridge, &ClipboardFileBridge::localFilesCopied);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList files = trickyPaths(dir.path());
    std::unique_ptr<QMimeData> echo(ClipboardFileBridge::makeFileMimeData(files));

    bridge.armSelfWriteGuard(files);
    QVERIFY(bridge.selfWriteGuardActive());
    QVERIFY(!bridge.handleClipboardData(echo.get())); // echo inside the window
    QCOMPARE(copied.count(), 0);

    // once the window closes the bridge must not be muted any more: the user
    // genuinely re-copying the same files has to be reported
    QVERIFY(QTest::qWaitFor([&bridge] { return !bridge.selfWriteGuardActive(); }, 5000));
    QVERIFY(!bridge.isEchoOfOwnWrite(files));
    QVERIFY(bridge.handleClipboardData(echo.get()));
    QCOMPARE(copied.count(), 1);
  }

  // -------------------------------------------------------- real clipboard

  void liveClipboardWriteIsNotReEmitted()
  {
    QClipboard *clip = ClipboardFileBridge::defaultClipboard();
    if (clip == nullptr)
      QSKIP("no GUI clipboard in this environment (QCoreApplication or no platform clipboard)");

    const QString savedText = clip->text(); // best-effort restore of the text form

    // the constructor already watches QGuiApplication's clipboard
    ClipboardFileBridge bridge;
    QVERIFY(bridge.clipboard() == clip);
    QSignalSpy copied(&bridge, &ClipboardFileBridge::localFilesCopied);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList files{nativePath(dir.path(), QStringLiteral("跨机 报告.txt"))};
    if (!bridge.putFilesOnClipboard(files))
      QSKIP("the clipboard is held by another process right now");

    QVERIFY(bridge.isFileClipboard());
    QCOMPARE(bridge.currentLocalFiles(), files);

    // Windows echoes any clipboard write (including our own) through
    // dataChanged: it must be recognised as ours and dropped
    QTest::qWait(300);
    QCOMPARE(copied.count(), 0);

    // sanity check that dataChanged does reach us at all: content written by
    // another code path (nothing armed) has to be reported
    const QStringList foreign{nativePath(dir.path(), QStringLiteral("外部 文件.bin"))};
    clip->setMimeData(ClipboardFileBridge::makeFileMimeData(foreign), QClipboard::Clipboard);
    if (!QTest::qWaitFor([&copied] { return copied.count() == 1; }, 2000))
      QSKIP("this platform did not report a clipboard write by this process through dataChanged()");

    QCOMPARE(copied.at(0).at(0).value<QStringList>(), foreign);

    // leave the developer's clipboard as close to how we found it as possible
    clip->setText(savedText);
  }
};

QTEST_MAIN(ClipboardFileBridgeTests)
#include "ClipboardFileBridgeTests.moc"
