// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// ClipFileTransfer unit tests: Qt-free framing/chunk plan/path guards, the
// SHA-1 cross-checked against QCryptographicHash, plus a real loopback
// TCP transfer between two ClipFileTransfer instances.
#include "../../lib/litekvm/ClipFileChunk.h"
#include "../../lib/litekvm/ClipFileTransfer.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace litekvm;

namespace {

QByteArray pseudoRandom(int size, unsigned seed)
{
  QByteArray out(size, '\0');
  unsigned x = seed ? seed : 1u;
  for (int i = 0; i < size; ++i) {
    x = x * 1664525u + 1013904223u;
    out[i] = char((x >> 24) & 0xFF);
  }
  return out;
}

QString writeTempFile(const QString &path, const QByteArray &content)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return QString();
  if (file.write(content) != qint64(content.size()))
    return QString();
  file.close();
  return path;
}

/** Sends a control frame straight into a receiver (no socket needed). */
QByteArray controlFrame(const QJsonObject &msg)
{
  const QByteArray json = QJsonDocument(msg).toJson(QJsonDocument::Compact);
  const std::vector<std::uint8_t> frame =
      encodeControlFrame(std::string_view(json.constData(), std::size_t(json.size())));
  return QByteArray(reinterpret_cast<const char *>(frame.data()), qsizetype(frame.size()));
}

} // namespace

class ClipFileTransferTests : public QObject {
  Q_OBJECT

private Q_SLOTS:

  // ------------------------------------------------------------- protocol core

  void sha1MatchesQcryptographicHash()
  {
    // our portable SHA-1 must agree with Qt's implementation byte for byte
    const QList<int> sizes = {0, 1, 55, 56, 63, 64, 65, 1000, 65536, 300 * 1024 + 7};
    for (const int size : sizes) {
      const QByteArray data = pseudoRandom(size, unsigned(size) + 7);
      const std::string ours =
          clipsha1::hex(clipsha1::digest(data.constData(), std::size_t(data.size())));
      const QByteArray qt = QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex();
      QCOMPARE(QString::fromStdString(ours), QString::fromLatin1(qt));
    }

    // known vectors
    QCOMPARE(QString::fromStdString(clipsha1::hex(clipsha1::digest("abc", 3))),
             QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d"));
    QCOMPARE(QString::fromStdString(clipsha1::hex(clipsha1::digest("", 0))),
             QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709"));

    // incremental hashing must equal one-shot hashing
    clipsha1::Sha1 streamed;
    const QByteArray big = pseudoRandom(200 * 1024, 11);
    streamed.update(big.constData(), 12345);
    streamed.update(big.constData() + 12345, std::size_t(big.size()) - 12345);
    QCOMPARE(QString::fromStdString(streamed.hex()),
             QString::fromLatin1(QCryptographicHash::hash(big, QCryptographicHash::Sha1).toHex()));
  }

  void frameRoundtripAndDecoderGuards()
  {
    const QByteArray json = R"({"type":"CLIP_OFFER","file_count":2})";
    const std::vector<std::uint8_t> frame =
        encodeControlFrame(std::string_view(json.constData(), std::size_t(json.size())));
    QCOMPARE(int(frame[0]), int(ClipFrameKind::Control));
    // control frames keep PairingService's 2-byte big-endian length prefix
    QCOMPARE(int(be16Read(frame.data() + 1)), int(json.size()));
    QCOMPARE(int(frame.size()), int(json.size()) + 3);

    // byte-by-byte reassembly
    FrameDecoder decoder;
    ClipFrame out;
    int seen = 0;
    for (std::size_t i = 0; i < frame.size(); ++i) {
      decoder.append(frame.data() + i, 1);
      QVERIFY(!decoder.failed());
      while (decoder.next(&out)) {
        ++seen;
        QCOMPARE(int(out.kind), int(ClipFrameKind::Control));
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(out.payload.data()),
                            qsizetype(out.payload.size())),
                 json);
      }
    }
    QCOMPARE(seen, 1);
    QCOMPARE(int(decoder.buffered()), 0);

    // unknown kind byte, zero length, oversized declared data length
    FrameDecoder bad0;
    const std::uint8_t junk[] = {0x7F, 0x00, 0x02, 0x41};
    bad0.append(junk, sizeof(junk));
    QVERIFY(!bad0.next(&out));
    QVERIFY(bad0.failed());
    QVERIFY(!bad0.failureReason().empty());

    FrameDecoder bad1;
    const std::uint8_t zero[] = {static_cast<std::uint8_t>(ClipFrameKind::Control), 0x00, 0x00};
    bad1.append(zero, sizeof(zero));
    QVERIFY(!bad1.next(&out));
    QVERIFY(bad1.failed());

    FrameDecoder bad2;
    const std::uint8_t oversized[] = {static_cast<std::uint8_t>(ClipFrameKind::Data), 0x00, 0x50,
                                      0x00, 0x01}; // 5 MiB + 1
    bad2.append(oversized, sizeof(oversized));
    QVERIFY(!bad2.next(&out));
    QVERIFY(bad2.failed());
  }

  void chunkFramesAndDigestTampering()
  {
    const QByteArray data = pseudoRandom(int(kClipDefaultChunkSize), 42);
    ChunkHeader header;
    QVERIFY(makeChunkHeader(7, 3 * kClipDefaultChunkSize,
                            reinterpret_cast<const std::uint8_t *>(data.constData()),
                            std::size_t(data.size()), &header));
    QCOMPARE(header.seq, std::uint32_t(7));
    QCOMPARE(header.dataLen, std::uint32_t(data.size()));
    QCOMPARE(QString::fromStdString(header.digestHex()),
             QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex()));

    const std::vector<std::uint8_t> payload =
        encodeDataPayload(header, reinterpret_cast<const std::uint8_t *>(data.constData()));
    QCOMPARE(int(payload.size()), int(kClipChunkHeaderBytes + std::size_t(data.size())));

    ChunkHeader back;
    const std::uint8_t *bytes = nullptr;
    std::size_t len = 0;
    QVERIFY(decodeDataPayload(payload, &back, &bytes, &len));
    QVERIFY(back == header);
    QVERIFY(chunkDigestMatches(back, bytes));

    // a 256 KiB chunk must survive one data frame round trip
    const std::vector<std::uint8_t> frame = encodeDataFrame(payload.data(), kClipChunkHeaderBytes,
                                                           bytes, len);
    QCOMPARE(int(frame[0]), int(ClipFrameKind::Data));
    QCOMPARE(int(be32Read(frame.data() + 1)), int(kClipChunkHeaderBytes + len));
    FrameDecoder decoder;
    decoder.append(frame);
    ClipFrame out;
    QVERIFY(decoder.next(&out));
    ChunkHeader parsed;
    const std::uint8_t *parsedBytes = nullptr;
    std::size_t parsedLen = 0;
    QVERIFY(decodeDataPayload(out.payload, &parsed, &parsedBytes, &parsedLen));
    QVERIFY(parsed == header);
    QVERIFY(chunkDigestMatches(parsed, parsedBytes));

    // one flipped data byte must be caught
    std::vector<std::uint8_t> tampered(payload);
    tampered[kClipChunkHeaderBytes] = std::uint8_t(tampered[kClipChunkHeaderBytes] ^ 0x01);
    ChunkHeader tamperedHeader;
    const std::uint8_t *tamperedBytes = nullptr;
    std::size_t tamperedLen = 0;
    QVERIFY(decodeDataPayload(tampered, &tamperedHeader, &tamperedBytes, &tamperedLen));
    QVERIFY(!chunkDigestMatches(tamperedHeader, tamperedBytes));

    // an empty file still gets exactly one (empty) chunk
    ChunkHeader empty;
    QVERIFY(makeChunkHeader(0, 0, nullptr, 0, &empty));
    QCOMPARE(empty.dataLen, std::uint32_t(0));
    QVERIFY(chunkDigestMatches(empty, nullptr));
  }

  void chunkPlanCoversFileExactly()
  {
    const std::size_t cs = kClipDefaultChunkSize;
    struct Expect {
      std::uint64_t size;
      std::uint64_t count;
    };
    const QList<Expect> cases = {{0, 1},
                                 {1, 1},
                                 {std::uint64_t(cs) - 1, 1},
                                 {std::uint64_t(cs), 1},
                                 {std::uint64_t(cs) + 1, 2},
                                 {std::uint64_t(3 * cs), 3},
                                 {std::uint64_t(3 * cs) + 1, 4}};

    for (const Expect &expected : cases) {
      const ChunkPlan plan(expected.size, cs);
      QVERIFY(plan.valid());
      QCOMPARE(plan.count(), expected.count);
      QCOMPARE(plan.bytesCovered(), expected.size); // chunks tile the file exactly
      for (std::uint64_t i = 0; i < plan.count(); ++i) {
        QCOMPARE(plan.offset(i), i * std::uint64_t(cs));
        QVERIFY(plan.length(i) <= cs);
      }
      const std::uint64_t last = plan.count() - 1;
      QCOMPARE(plan.offset(last) + std::uint64_t(plan.length(last)), expected.size);
    }

    // the plan's own acceptance case: a 50 MB folder member -> 200 chunks
    QCOMPARE(ChunkPlan(50ull * 1024 * 1024, cs).count(), std::uint64_t(200));

    ChunkPlan plan(3 * std::uint64_t(cs) + 5, cs);
    std::uint32_t seq = 0;
    QVERIFY(plan.seqForOffset(cs, &seq) && seq == 1);
    QVERIFY(!plan.seqForOffset(cs + 1, &seq)); // resume point must be aligned
    QVERIFY(!ChunkPlan(1000, kClipMinChunkSize - 1).valid());
    QVERIFY(!ChunkPlan(1000, kClipMaxChunkSize + 1).valid());
  }

  void unsafePathsAreRejected()
  {
    QVERIFY(isSafeRelativePath("dir/sub/file.bin"));
    QVERIFY(isSafeRelativePath(QStringLiteral("中文 文件夹/报告.txt").toUtf8().toStdString()));

    QVERIFY(!isSafeRelativePath(""));
    QVERIFY(!isSafeRelativePath("/etc/passwd"));
    QVERIFY(!isSafeRelativePath("\\windows\\system32"));
    QVERIFY(!isSafeRelativePath("C:/windows/system32"));
    QVERIFY(!isSafeRelativePath("C:x"));
    QVERIFY(!isSafeRelativePath("../../secrets"));
    QVERIFY(!isSafeRelativePath("a/../../b"));
    QVERIFY(!isSafeRelativePath("a/"));
    QVERIFY(!isSafeRelativePath(std::string(4097, 'a')));

    QCOMPARE(QString::fromStdString(normalizeRelativePath("dir\\sub/file.txt")),
             QStringLiteral("dir/sub/file.txt"));
    QCOMPARE(QString::fromStdString(normalizeRelativePath("./a//b/./c/")), QStringLiteral("a/b/c"));
    // normalization keeps "..", the safety check is what rejects it
    QVERIFY(!isSafeRelativePath(normalizeRelativePath("..\\..\\etc")));

    QCOMPARE(QString::fromStdString(sanitizeFileName("a<b>?c:d")), QStringLiteral("a_b__c_d"));
    QCOMPARE(QString::fromStdString(sanitizeFileName("trailing. ")), QStringLiteral("trailing"));
    QCOMPARE(QString::fromStdString(partPathFor("a/b.bin")),
             QStringLiteral("a/b.bin.litekvm-part"));
    QCOMPARE(QString::fromStdString(parentDirOf("a/b.bin")), QStringLiteral("a"));
  }

  void progressMath()
  {
    TransferProgress progress;
    QCOMPARE(progress.ratio(), 1.0); // nothing to move
    QCOMPARE(progress.percent(), 100);

    progress.bytesTotal = 1024;
    QCOMPARE(progress.ratio(), 0.0);
    progress.bytesDone = 512;
    QCOMPARE(progress.percent(), 50);
    QCOMPARE(progress.bytesRemaining(), std::uint64_t(512));
    progress.bytesDone = 1024;
    QCOMPARE(progress.ratio(), 1.0);
    progress.bytesDone = 4096; // never overshoot
    QCOMPARE(progress.percent(), 100);
  }

  void fileSha1HelperMatchesQt()
  {
    QTemporaryDir dir;
    const QByteArray content = pseudoRandom(200 * 1024, 5);
    const QString path = writeTempFile(dir.path() + QStringLiteral("/中文.bin"), content);
    QVERIFY(!path.isEmpty());

    QCOMPARE(ClipFileTransfer::fileSha1(path),
             QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha1).toHex()));
    QVERIFY(ClipFileTransfer::fileSha1(dir.path() + QStringLiteral("/missing.bin")).isEmpty());
    QVERIFY(ClipFileTransfer::defaultIncomingDirectory().contains(QStringLiteral("LiteKVM")));
    QCOMPARE(ClipFileTransfer::defaultChunkSize(), 256 * 1024);
  }

  // ------------------------------------------------------- loopback transfers

  void loopbackSendsMultiChunkFile()
  {
    QTemporaryDir incoming;
    QVERIFY(incoming.isValid());
    QTemporaryDir source;
    QVERIFY(source.isValid());

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(server.waitForNewConnection(5000));
    QTcpSocket *accepted = server.nextPendingConnection();
    QVERIFY(accepted != nullptr);
    QVERIFY(client.waitForConnected(5000));

    ClipFileTransfer sender(&client);
    ClipFileTransfer receiver(accepted);
    sender.setChunkSize(64 * 1024);
    receiver.setChunkSize(64 * 1024);
    receiver.setAutoAccept(true);
    QVERIFY(receiver.incomingDirectory().isEmpty());
    receiver.setIncomingDirectory(incoming.path());
    QVERIFY(!receiver.incomingDirectory().isEmpty());

    const QByteArray content = pseudoRandom(300 * 1024 + 7, 99); // five-ish chunks
    const QString name = QStringLiteral("跨机 报告.bin");
    const QString path = writeTempFile(source.path() + QLatin1Char('/') + name, content);
    QVERIFY(!path.isEmpty());

    QSignalSpy doneSpy(&receiver, &ClipFileTransfer::transferCompleted);
    QSignalSpy senderFailureSpy(&sender, &ClipFileTransfer::transferFailed);
    QSignalSpy receiverFailureSpy(&receiver, &ClipFileTransfer::transferFailed);

    sender.sendLocalPaths({path});
    QCOMPARE(int(sender.state()), int(ClipFileTransfer::State::Offering));
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() == 1, 15000);

    QCOMPARE(senderFailureSpy.count(), 0);
    QCOMPARE(receiverFailureSpy.count(), 0);
    QCOMPARE(int(sender.state()), int(ClipFileTransfer::State::Completed));
    QCOMPARE(int(receiver.state()), int(ClipFileTransfer::State::Completed));
    QCOMPARE(sender.progress().percent(), 100);

    const QStringList landed = receiver.completedPaths();
    QCOMPARE(landed.size(), 1);
    QVERIFY(QFileInfo::exists(landed.at(0)));
    QFile received(landed.at(0));
    QVERIFY(received.open(QIODevice::ReadOnly));
    QCOMPARE(received.readAll(), content); // byte-for-byte
    QCOMPARE(ClipFileTransfer::fileSha1(landed.at(0)),
             QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha1).toHex()));
    // the temp file must not be left behind
    QVERIFY(!QFileInfo::exists(landed.at(0) + QString::fromLatin1(kClipPartSuffix)));
  }

  void loopbackSendsDirectoryTree()
  {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(server.waitForNewConnection(5000));
    QTcpSocket *accepted = server.nextPendingConnection();
    QVERIFY(accepted != nullptr);
    QVERIFY(client.waitForConnected(5000));

    ClipFileTransfer sender(&client);
    ClipFileTransfer receiver(accepted);
    QTemporaryDir incoming;
    QVERIFY(incoming.isValid());
    receiver.setIncomingDirectory(incoming.path());
    receiver.setAutoAccept(true);

    QTemporaryDir source;
    QVERIFY(source.isValid());
    const QString root = source.path() + QStringLiteral("/中文目录");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/子目录")));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/空的")));
    const QByteArray big = pseudoRandom(200 * 1024, 3);
    QVERIFY(!writeTempFile(root + QStringLiteral("/a.txt"), "hello\n").isEmpty());
    QVERIFY(!writeTempFile(root + QStringLiteral("/子目录/b.bin"), big).isEmpty());

    QSignalSpy doneSpy(&receiver, &ClipFileTransfer::transferCompleted);
    sender.sendLocalPaths({root});
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() == 1, 15000);

    const QString dest = incoming.path() + QStringLiteral("/中文目录");
    QCOMPARE(QFileInfo(dest + QStringLiteral("/a.txt")).size(), qint64(6));
    QCOMPARE(QFileInfo(dest + QStringLiteral("/子目录/b.bin")).size(), qint64(big.size()));
    QVERIFY(QFileInfo(dest + QStringLiteral("/空的")).isDir()); // empty dirs survive
    QCOMPARE(ClipFileTransfer::fileSha1(dest + QStringLiteral("/子目录/b.bin")),
             QString::fromLatin1(QCryptographicHash::hash(big, QCryptographicHash::Sha1).toHex()));
  }

  void receiverRejectionFailsTheSender()
  {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(server.waitForNewConnection(5000));
    QTcpSocket *accepted = server.nextPendingConnection();
    QVERIFY(accepted != nullptr);
    QVERIFY(client.waitForConnected(5000));

    ClipFileTransfer sender(&client);
    ClipFileTransfer receiver(accepted);
    QTemporaryDir incoming;
    QVERIFY(incoming.isValid());
    receiver.setIncomingDirectory(incoming.path());
    receiver.setAutoAccept(false);
    connect(&receiver, &ClipFileTransfer::offerReceived, &receiver,
            [&receiver](const ClipFileOffer &) { receiver.rejectOffer(QStringLiteral("USER")); });

    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(!writeTempFile(source.path() + QStringLiteral("/x.txt"), "nope").isEmpty());

    QSignalSpy senderFailureSpy(&sender, &ClipFileTransfer::transferFailed);
    QSignalSpy doneSpy(&sender, &ClipFileTransfer::transferCompleted);

    sender.sendLocalPaths({source.path() + QStringLiteral("/x.txt")});
    QTRY_VERIFY_WITH_TIMEOUT(senderFailureSpy.count() == 1, 10000);
    QCOMPARE(int(senderFailureSpy.at(0).at(0).value<ClipFileTransfer::Error>()),
             int(ClipFileTransfer::Error::Rejected));
    QCOMPARE(doneSpy.count(), 0);
    QCOMPARE(int(receiver.state()), int(ClipFileTransfer::State::Cancelled));
  }

  void cancelDuringOfferStopsTheTransfer()
  {
    QBuffer channel;
    QVERIFY(channel.open(QIODevice::WriteOnly));

    QTemporaryDir source;
    QVERIFY(source.isValid());
    const QString path = writeTempFile(source.path() + QStringLiteral("/big.bin"),
                                       pseudoRandom(8 * 1024 * 1024, 1));
    QVERIFY(!path.isEmpty());

    ClipFileTransfer sender(&channel);
    QSignalSpy cancelledSpy(&sender, &ClipFileTransfer::transferCancelled);
    QSignalSpy doneSpy(&sender, &ClipFileTransfer::transferCompleted);

    sender.sendLocalPaths({path});
    QVERIFY(sender.isActive());
    QVERIFY(channel.size() > 0); // CLIP_OFFER went out

    sender.cancel();
    QCOMPARE(cancelledSpy.count(), 1);
    QCOMPARE(int(sender.state()), int(ClipFileTransfer::State::Cancelled));
    QCOMPARE(doneSpy.count(), 0);
    QVERIFY(!sender.isActive());
    sender.cancel(); // idempotent
    QCOMPARE(cancelledSpy.count(), 1);
  }

  void oversizedTransferIsRejectedLocally()
  {
    QBuffer channel;
    QVERIFY(channel.open(QIODevice::WriteOnly));

    QTemporaryDir source;
    QVERIFY(source.isValid());
    const QByteArray blob(64 * 1024, 'x');
    QVERIFY(!writeTempFile(source.path() + QStringLiteral("/a.bin"), blob).isEmpty());

    ClipFileTransfer sender(&channel);
    sender.setMaxTransferBytes(1024);
    QSignalSpy failureSpy(&sender, &ClipFileTransfer::transferFailed);

    sender.sendLocalPaths({source.path() + QStringLiteral("/a.bin")});
    QCOMPARE(failureSpy.count(), 1);
    QCOMPARE(int(failureSpy.at(0).at(0).value<ClipFileTransfer::Error>()),
             int(ClipFileTransfer::Error::TooLarge));
    QCOMPARE(int(sender.state()), int(ClipFileTransfer::State::Failed));
  }

  void offerWithoutIncomingDirectoryIsRejected()
  {
    QBuffer channel;
    QVERIFY(channel.open(QIODevice::ReadWrite));

    ClipFileTransfer receiver(&channel);
    QSignalSpy failureSpy(&receiver, &ClipFileTransfer::transferFailed);

    const QByteArray frame = controlFrame(QJsonObject{
        {"type", "CLIP_OFFER"},
        {"proto", "v1"},
        {"transfer_id", "0123456789abcdef0123456789abcdef"},
        {"chunk_size", 256 * 1024},
        {"total_bytes", 4.0},
        {"entries", QJsonArray{QJsonObject{{"rel_path", "a.txt"}, {"size", 4.0}, {"is_dir", false}}}}});
    channel.write(frame);
    QVERIFY(channel.seek(0));
    // QBuffer signals readyRead with the cursor already at the end, so feed the
    // receiver explicitly instead of relying on the event loop
    QVERIFY(QMetaObject::invokeMethod(&receiver, "onChannelReadyRead", Qt::DirectConnection));

    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 1, 5000);
    QCOMPARE(int(failureSpy.at(0).at(0).value<ClipFileTransfer::Error>()),
             int(ClipFileTransfer::Error::Io));
    QCOMPARE(int(receiver.state()), int(ClipFileTransfer::State::Failed));
  }

  void offerWithEscapingPathIsRejected()
  {
    QBuffer channel;
    QVERIFY(channel.open(QIODevice::ReadWrite));

    ClipFileTransfer receiver(&channel);
    QTemporaryDir incoming;
    QVERIFY(incoming.isValid());
    receiver.setIncomingDirectory(incoming.path());
    QSignalSpy failureSpy(&receiver, &ClipFileTransfer::transferFailed);

    const QByteArray frame = controlFrame(QJsonObject{
        {"type", "CLIP_OFFER"},
        {"proto", "v1"},
        {"transfer_id", "0123456789abcdef0123456789abcdef"},
        {"chunk_size", 256 * 1024},
        {"total_bytes", 4.0},
        {"entries", QJsonArray{QJsonObject{{"rel_path", "../../evil.txt"},
                                           {"size", 4.0},
                                           {"is_dir", false}}}}});
    channel.write(frame);
    QVERIFY(channel.seek(0));
    // QBuffer signals readyRead with the cursor already at the end, so feed the
    // receiver explicitly instead of relying on the event loop
    QVERIFY(QMetaObject::invokeMethod(&receiver, "onChannelReadyRead", Qt::DirectConnection));

    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 1, 5000);
    QCOMPARE(int(failureSpy.at(0).at(0).value<ClipFileTransfer::Error>()),
             int(ClipFileTransfer::Error::Protocol));
    QVERIFY(!QFileInfo::exists(incoming.path() + QStringLiteral("/../../evil.txt")));
  }
};

QTEST_MAIN(ClipFileTransferTests)
#include "ClipFileTransferTests.moc"
