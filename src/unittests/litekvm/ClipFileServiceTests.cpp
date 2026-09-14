// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// ClipFileService tests: two real services over loopback TCP — paired-device
// handshake, one real chunked file transfer, rejection / busy / handshake
// timeout / mid-transfer disconnect cleanup.
#include "../../lib/litekvm/ClipFileChunk.h"
#include "../../lib/litekvm/ClipFileService.h"
#include "../../lib/litekvm/ClipFileTransfer.h"
#include "../../lib/litekvm/DeviceIdentity.h"
#include "../../lib/litekvm/TrustStore.h"

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <optional>

using namespace litekvm;

namespace {

/// An isolated node: its own identity file and its own trust store.
struct Node {
  QTemporaryDir dir;
  std::optional<DeviceIdentity> identity;
  std::unique_ptr<TrustStore> store;

  bool init()
  {
    identity = DeviceIdentity::loadOrCreate(dir.path() + QStringLiteral("/id.json"));
    if (!identity)
      return false;
    store = std::make_unique<TrustStore>(dir.path() + QStringLiteral("/trust.json"));
    return true;
  }

  /// What the *other* side has to persist to trust this node.
  TrustEntry entry() const
  {
    TrustEntry e;
    e.deviceId = identity->deviceId();
    e.pubkey = QByteArray(reinterpret_cast<const char *>(identity->publicKey().data()),
                          qsizetype(identity->publicKey().size()));
    e.name = identity->name();
    e.platform = QStringLiteral("win");
    return e;
  }
};

/// Make two nodes trust each other, as PairingService would have.
void pairTrust(Node &a, Node &b)
{
  a.store->upsert(b.entry());
  b.store->upsert(a.entry());
}

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

/// Recursive count of half-written transfer temp files under @p root.
int countPartFiles(const QString &root)
{
  int count = 0;
  QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    if (it.fileName().endsWith(QString::fromLatin1(kClipPartSuffix)))
      ++count;
  }
  return count;
}

/// A loopback port nothing is listening on (bound then released).
quint16 unusedPort()
{
  QTcpServer probe;
  if (!probe.listen(QHostAddress::LocalHost, 0))
    return 0;
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

QByteArray sha1Hex(const QByteArray &data)
{
  return QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex();
}

} // namespace

class ClipFileServiceTests : public QObject {
  Q_OBJECT

private Q_SLOTS:

  // ------------------------------------------------------------ wire (no socket)

  void helloFrameCodecGuards()
  {
    QCOMPARE(ClipFileService::defaultPort(), quint16(25903));
    QCOMPARE(ClipFileService::protocolVersion(), QStringLiteral("v1"));

    const QJsonObject hello{{QStringLiteral("type"), QStringLiteral("CALP_HELLO")},
                            {QStringLiteral("proto"), QStringLiteral("v1")},
                            {QStringLiteral("device_id"), QStringLiteral("0123456789abcdef")}};
    const QByteArray frame = ClipFileService::encodeFrame(hello);
    QVERIFY(!frame.isEmpty());

    const QByteArray payload = QJsonDocument(hello).toJson(QJsonDocument::Compact);
    QCOMPARE(frame.size(), payload.size() + 2); // PairingService's 2-byte big-endian prefix
    QCOMPARE(quint8(frame.at(0)), quint8((payload.size() >> 8) & 0xFF));
    QCOMPARE(quint8(frame.at(1)), quint8(payload.size() & 0xFF));

    const auto back = ClipFileService::decodeFrame(frame.mid(2));
    QVERIFY(back.has_value());
    QCOMPARE(back->value(QStringLiteral("device_id")).toString(), QStringLiteral("0123456789abcdef"));

    // garbage must not decode
    QVERIFY(!ClipFileService::decodeFrame(QByteArrayLiteral("{not json")).has_value());
    QVERIFY(!ClipFileService::decodeFrame(QByteArrayLiteral("[1,2,3]")).has_value());
    QVERIFY(!ClipFileService::decodeFrame(QByteArray()).has_value());

    // a payload above the 64 KiB cap is refused, never truncated into a frame
    const QJsonObject oversized{{QStringLiteral("type"), QStringLiteral("CALP_HELLO")},
                                {QStringLiteral("blob"), QString(70000, QLatin1Char('a'))}};
    QVERIFY(ClipFileService::encodeFrame(oversized).isEmpty());
  }

  // ---------------------------------------------------------- real loopback

  void loopbackTransferBetweenPairedDevices()
  {
    Node serverNode, clientNode;
    QVERIFY(serverNode.init());
    QVERIFY(clientNode.init());
    pairTrust(serverNode, clientNode);

    QTemporaryDir incoming, source;
    QVERIFY(incoming.isValid());
    QVERIFY(source.isValid());

    const QByteArray content = pseudoRandom(300 * 1024 + 17, 0xC0FFEE); // two-ish chunks
    const QString path = writeTempFile(source.path() + QStringLiteral("/跨机 报告.bin"), content);
    QVERIFY(!path.isEmpty());

    // state captured by the lambdas; the services below are declared after it so
    // they are destroyed first and never emit into a dead capture
    QSemaphore serverUp, clientUp, serverBye, clientBye, serverDone, clientDone, serverFail, clientFail;
    QString serverPeer, clientPeer;
    QStringList landed, log;

    ClipFileService server(*serverNode.identity, *serverNode.store);
    ClipFileService client(*clientNode.identity, *clientNode.store);

    server.setIncomingDirectory(incoming.path());
    server.setAutoAccept(true);
    QCOMPARE(server.incomingDirectory(), QDir::cleanPath(incoming.path()));
    QVERIFY(server.autoAccept());
    QCOMPARE(int(server.role()), int(ClipFileService::Role::None));
    QVERIFY(!server.isListening());

    connect(&server, &ClipFileService::peerConnected, [&](const QString &id) {
      serverPeer = id;
      serverUp.release();
    });
    connect(&client, &ClipFileService::peerConnected, [&](const QString &id) {
      clientPeer = id;
      clientUp.release();
    });
    connect(&server, &ClipFileService::peerDisconnected, [&] { serverBye.release(); });
    connect(&client, &ClipFileService::peerDisconnected, [&] { clientBye.release(); });
    connect(&server, &ClipFileService::transferCompleted,
            [&](const QStringList &paths, const QString &) {
              landed = paths;
              serverDone.release();
            });
    connect(&client, &ClipFileService::transferCompleted,
            [&](const QStringList &, const QString &) { clientDone.release(); });
    connect(&server, &ClipFileService::transferFailed,
            [&](ClipFileTransfer::Error, const QString &detail) {
              log << QStringLiteral("server transferFailed: ") + detail;
              serverFail.release();
            });
    connect(&client, &ClipFileService::transferFailed,
            [&](ClipFileTransfer::Error, const QString &detail) {
              log << QStringLiteral("client transferFailed: ") + detail;
              clientFail.release();
            });
    connect(&server, &ClipFileService::errorOccurred, [&](const QString &message) {
      log << QStringLiteral("server errorOccurred: ") + message;
      serverFail.release();
    });
    connect(&client, &ClipFileService::errorOccurred, [&](const QString &message) {
      log << QStringLiteral("client errorOccurred: ") + message;
      clientFail.release();
    });

    // the product port first; an ephemeral one if 25903 is taken on this box
    if (!server.listen(ClipFileService::defaultPort()))
      QVERIFY(server.listen(0));
    QVERIFY(server.isListening());
    QVERIFY(server.serverPort() != 0);
    QCOMPARE(int(server.role()), int(ClipFileService::Role::Server));

    client.sendFilesTo(QStringLiteral("127.0.0.1"), server.serverPort(), {path});
    QCOMPARE(int(client.role()), int(ClipFileService::Role::Client));

    // --- handshake
    QVERIFY2(spin(serverUp, log), qPrintable(log.join(QStringLiteral(" | "))));
    QVERIFY2(spin(clientUp, log), qPrintable(log.join(QStringLiteral(" | "))));
    QCOMPARE(serverPeer, clientNode.identity->deviceId());
    QCOMPARE(clientPeer, serverNode.identity->deviceId());
    QCOMPARE(server.peerDeviceId(), clientNode.identity->deviceId());
    QCOMPARE(server.peerName(), clientNode.identity->name());
    QVERIFY(server.hasPeer());
    QVERIFY(client.hasPeer());

    // --- one real transfer
    QVERIFY2(spin(serverDone, log), qPrintable(log.join(QStringLiteral(" | "))));
    QVERIFY2(spin(clientDone, log), qPrintable(log.join(QStringLiteral(" | "))));
    QCOMPARE(serverFail.available(), 0);
    QCOMPARE(clientFail.available(), 0);
    QVERIFY2(log.isEmpty(), qPrintable(log.join(QStringLiteral(" | "))));

    QCOMPARE(landed.size(), 1);
    QVERIFY(QFileInfo::exists(landed.at(0)));
    QCOMPARE(QFileInfo(landed.at(0)).fileName(), QStringLiteral("跨机 报告.bin")); // UTF-8 survives
    QFile received(landed.at(0));
    QVERIFY(received.open(QIODevice::ReadOnly));
    QCOMPARE(received.readAll(), content); // byte for byte
    received.close();
    QCOMPARE(ClipFileTransfer::fileSha1(landed.at(0)), QString::fromLatin1(sha1Hex(content)));
    QCOMPARE(countPartFiles(incoming.path()), 0); // no half-written temp file left

    // --- one transfer per session: both ends tore the session down
    QVERIFY2(spin(serverBye, log), qPrintable(log.join(QStringLiteral(" | "))));
    QVERIFY2(spin(clientBye, log), qPrintable(log.join(QStringLiteral(" | "))));
    QVERIFY(!server.hasPeer());
    QVERIFY(!client.hasPeer());
    QVERIFY(!server.isBusy());
    QVERIFY(server.transfer() == nullptr);
    QVERIFY(client.transfer() == nullptr);

    server.stop();
    client.stop();
    QVERIFY(!server.isListening());
  }

  void unpairedPeerIsRejectedAndTheSessionStaysUsable()
  {
    Node serverNode, clientNode;
    QVERIFY(serverNode.init());
    QVERIFY(clientNode.init());
    // the client knows the server; the server has never heard of the client
    clientNode.store->upsert(serverNode.entry());

    QTemporaryDir incoming, source;
    QVERIFY(incoming.isValid());
    QVERIFY(source.isValid());
    const QByteArray content = pseudoRandom(8 * 1024 + 1, 7);
    const QString path = writeTempFile(source.path() + QStringLiteral("/secret.bin"), content);
    QVERIFY(!path.isEmpty());

    QSemaphore serverErr, clientErr, clientUp, serverUp, serverDone, clientDone;
    QStringList serverLog, clientLog, landed;

    ClipFileService server(*serverNode.identity, *serverNode.store);
    ClipFileService client(*clientNode.identity, *clientNode.store);
    server.setIncomingDirectory(incoming.path());
    server.setAutoAccept(true);

    connect(&server, &ClipFileService::errorOccurred, [&](const QString &message) {
      serverLog << message;
      serverErr.release();
    });
    connect(&client, &ClipFileService::errorOccurred, [&](const QString &message) {
      clientLog << message;
      clientErr.release();
    });
    connect(&client, &ClipFileService::peerConnected, [&](const QString &) { clientUp.release(); });
    connect(&server, &ClipFileService::peerConnected, [&](const QString &) { serverUp.release(); });
    connect(&server, &ClipFileService::transferCompleted,
            [&](const QStringList &paths, const QString &) {
              landed = paths;
              serverDone.release();
            });
    connect(&client, &ClipFileService::transferCompleted,
            [&](const QStringList &, const QString &) { clientDone.release(); });

    if (!server.listen(ClipFileService::defaultPort()))
      QVERIFY(server.listen(0));

    client.sendFilesTo(QStringLiteral("127.0.0.1"), server.serverPort(), {path});

    // both ends report the refusal: the server because we are not paired, the
    // client because the peer told it so
    QVERIFY2(spin(serverErr, serverLog), qPrintable(serverLog.join(QStringLiteral(" | "))));
    QVERIFY2(spin(clientErr, clientLog), qPrintable(clientLog.join(QStringLiteral(" | "))));
    QVERIFY(serverLog.join(QStringLiteral(" ")).contains(QStringLiteral("NOT_PAIRED")));
    QVERIFY(clientLog.join(QStringLiteral(" ")).contains(QStringLiteral("NOT_PAIRED")));
    QCOMPARE(clientUp.available(), 0); // never accepted as a peer
    QCOMPARE(serverUp.available(), 0);
    QVERIFY(!server.hasPeer());
    QVERIFY(!client.hasPeer());
    QVERIFY(server.transfer() == nullptr);
    QVERIFY(client.transfer() == nullptr);

    // the refused session left no state behind: pairing up lets the very next
    // connection through on the same two service instances
    pairTrust(serverNode, clientNode);
    client.sendFilesTo(QStringLiteral("127.0.0.1"), server.serverPort(), {path});
    QVERIFY2(spin(serverUp, serverLog), qPrintable(serverLog.join(QStringLiteral(" | "))));
    QVERIFY2(spin(serverDone, serverLog), qPrintable(serverLog.join(QStringLiteral(" | "))));
    QVERIFY2(spin(clientDone, clientLog), qPrintable(clientLog.join(QStringLiteral(" | "))));
    QCOMPARE(landed.size(), 1);
    QVERIFY(QFileInfo::exists(landed.at(0)));
    QFile received(landed.at(0));
    QVERIFY(received.open(QIODevice::ReadOnly));
    QCOMPARE(received.readAll(), content);

    server.stop();
    client.stop();
  }

  void busyServerRefusesASecondClient()
  {
    Node serverNode, clientNode, otherNode;
    QVERIFY(serverNode.init());
    QVERIFY(clientNode.init());
    QVERIFY(otherNode.init());
    pairTrust(serverNode, clientNode);
    pairTrust(serverNode, otherNode);

    QTemporaryDir incoming, source;
    QVERIFY(incoming.isValid());
    QVERIFY(source.isValid());
    const QByteArray content = pseudoRandom(64 * 1024 + 3, 11);
    const QString path = writeTempFile(source.path() + QStringLiteral("/one.bin"), content);
    const QString otherPath = writeTempFile(source.path() + QStringLiteral("/two.bin"), content);
    QVERIFY(!path.isEmpty() && !otherPath.isEmpty());

    QSemaphore offerSeen, serverDone, otherErr, otherUp;
    QStringList log, otherLog, landed;

    ClipFileService server(*serverNode.identity, *serverNode.store);
    ClipFileService client(*clientNode.identity, *clientNode.store);
    ClipFileService other(*otherNode.identity, *otherNode.store);

    server.setIncomingDirectory(incoming.path());
    server.setAutoAccept(false); // hold the offer: the session stays busy

    connect(&server, &ClipFileService::offerReceived,
            [&](const ClipFileOffer &) { offerSeen.release(); });
    connect(&server, &ClipFileService::transferCompleted,
            [&](const QStringList &paths, const QString &) {
              landed = paths;
              serverDone.release();
            });
    connect(&server, &ClipFileService::errorOccurred, [&](const QString &message) {
      log << QStringLiteral("server: ") + message;
    });
    connect(&server, &ClipFileService::transferFailed,
            [&](ClipFileTransfer::Error, const QString &detail) {
              log << QStringLiteral("server transferFailed: ") + detail;
            });
    connect(&other, &ClipFileService::errorOccurred, [&](const QString &message) {
      otherLog << message;
      otherErr.release();
    });
    connect(&other, &ClipFileService::peerConnected, [&](const QString &) { otherUp.release(); });

    if (!server.listen(ClipFileService::defaultPort()))
      QVERIFY(server.listen(0));

    client.sendFilesTo(QStringLiteral("127.0.0.1"), server.serverPort(), {path});
    QVERIFY2(spin(offerSeen, log), qPrintable(log.join(QStringLiteral(" | "))));
    QVERIFY(server.hasPeer());
    QVERIFY(server.isBusy());
    QVERIFY(server.transfer() != nullptr);
    QCOMPARE(server.peerDeviceId(), clientNode.identity->deviceId());

    // second connection while the first session is live -> BUSY, then closed
    other.sendFilesTo(QStringLiteral("127.0.0.1"), server.serverPort(), {otherPath});
    QVERIFY2(spin(otherErr, otherLog, 8000), qPrintable(otherLog.join(QStringLiteral(" | "))));
    QVERIFY(otherLog.join(QStringLiteral(" ")).contains(QStringLiteral("BUSY")));
    QCOMPARE(otherUp.available(), 0);
    QVERIFY(!other.hasPeer());
    QVERIFY(other.transfer() == nullptr);
    QVERIFY(other.serverPort() == 0); // never listened

    // ...and refusing it did not disturb the first session
    QVERIFY(log.isEmpty());
    QVERIFY(server.hasPeer());
    QCOMPARE(server.peerDeviceId(), clientNode.identity->deviceId());
    QVERIFY(server.isBusy());

    server.acceptOffer();
    QVERIFY2(spin(serverDone, log), qPrintable(log.join(QStringLiteral(" | "))));
    QCOMPARE(landed.size(), 1);
    QFile received(landed.at(0));
    QVERIFY(received.open(QIODevice::ReadOnly));
    QCOMPARE(received.readAll(), content);
    QCOMPARE(countPartFiles(incoming.path()), 0);

    server.stop();
    client.stop();
    other.stop();
  }

  void disconnectDuringTransferRemovesThePartialFile()
  {
    Node serverNode, clientNode;
    QVERIFY(serverNode.init());
    QVERIFY(clientNode.init());
    pairTrust(serverNode, clientNode);

    QTemporaryDir incoming, source;
    QVERIFY(incoming.isValid());
    QVERIFY(source.isValid());
    const QByteArray content = pseudoRandom(16 * 1024 * 1024, 0xBEEF);
    const QString path = writeTempFile(source.path() + QStringLiteral("/一半 大文件.bin"), content);
    QVERIFY(!path.isEmpty());

    bool sawPartFile = false;
    QSemaphore serverBye, serverDone, cancelled;
    QStringList log;

    ClipFileService server(*serverNode.identity, *serverNode.store);
    ClipFileService client(*clientNode.identity, *clientNode.store);
    server.setIncomingDirectory(incoming.path());
    server.setAutoAccept(true);

    connect(&server, &ClipFileService::progressChanged, [&](const TransferProgress &p) {
      if (sawPartFile || p.bytesDone == 0 || countPartFiles(incoming.path()) == 0)
        return;
      sawPartFile = true;
      // Abort from inside the first progress callback. The sender's high-water
      // mark (4 MiB) caps how much can be in flight, so 16 MiB cannot have
      // landed yet — the receiver is definitely left with a ".litekvm-part".
      client.stop();
    });
    connect(&server, &ClipFileService::transferCancelled,
            [&](const QString &) { cancelled.release(); });
    connect(&server, &ClipFileService::transferCompleted,
            [&](const QStringList &, const QString &) { serverDone.release(); });
    connect(&server, &ClipFileService::peerDisconnected, [&] { serverBye.release(); });
    connect(&server, &ClipFileService::errorOccurred, [&](const QString &message) {
      log << QStringLiteral("server errorOccurred: ") + message;
    });
    connect(&server, &ClipFileService::transferFailed,
            [&](ClipFileTransfer::Error, const QString &detail) {
              log << QStringLiteral("server transferFailed: ") + detail;
            });

    if (!server.listen(ClipFileService::defaultPort()))
      QVERIFY(server.listen(0));

    client.sendFilesTo(QStringLiteral("127.0.0.1"), server.serverPort(), {path});

    // the peer hung up mid-flight: the session must be torn down
    QVERIFY2(spin(serverBye, log), qPrintable(log.join(QStringLiteral(" | "))));
    QVERIFY(sawPartFile);                          // we caught it mid-write
    QVERIFY2(cancelled.available() >= 1, "the teardown must surface transferCancelled");
    QCOMPARE(serverDone.available(), 0);           // and it never completed
    QCOMPARE(countPartFiles(incoming.path()), 0);  // half-written temp file removed
    QVERIFY(QDir(incoming.path()).entryList(QDir::Files).isEmpty()); // and nothing else landed
    QVERIFY2(log.isEmpty(), qPrintable(log.join(QStringLiteral(" | ")))); // a hang-up is not an error
    QVERIFY(!server.hasPeer());
    QVERIFY(server.transfer() == nullptr);

    server.stop();
    client.stop();
  }

  void silentPeerIsDroppedByTheHandshakeTimeout()
  {
    Node serverNode;
    QVERIFY(serverNode.init());

    QSemaphore serverErr;
    QStringList log;

    ClipFileService server(*serverNode.identity, *serverNode.store);
    server.setHandshakeTimeout(300);
    QCOMPARE(server.handshakeTimeout(), 300);
    server.setHandshakeTimeout(0); // 0 restores the default
    QCOMPARE(server.handshakeTimeout(), 10000);
    server.setHandshakeTimeout(300);

    connect(&server, &ClipFileService::errorOccurred, [&](const QString &message) {
      log << message;
      serverErr.release();
    });
    connect(&server, &ClipFileService::peerConnected, [&](const QString &) {
      log << QStringLiteral("unexpected peerConnected");
    });

    if (!server.listen(ClipFileService::defaultPort()))
      QVERIFY(server.listen(0));

    QTcpSocket silent;
    silent.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(silent.waitForConnected(5000));

    QVERIFY2(spin(serverErr, log, 8000), qPrintable(log.join(QStringLiteral(" | "))));
    QVERIFY(log.join(QStringLiteral(" ")).contains(QStringLiteral("HANDSHAKE_TIMEOUT")));
    QVERIFY(!server.hasPeer());
    QVERIFY(server.transfer() == nullptr);

    // the server closed its end of the silent connection
    QTRY_VERIFY_WITH_TIMEOUT(silent.state() == QAbstractSocket::UnconnectedState, 5000);

    server.stop();
  }

  void unreachablePeerReportsASocketError()
  {
    Node clientNode;
    QVERIFY(clientNode.init());
    const quint16 port = unusedPort();
    QVERIFY(port != 0);

    QTemporaryDir source;
    QVERIFY(source.isValid());
    const QString path = writeTempFile(source.path() + QStringLiteral("/x.bin"), QByteArrayLiteral("hi"));
    QVERIFY(!path.isEmpty());

    QSemaphore err;
    QStringList log;

    ClipFileService client(*clientNode.identity, *clientNode.store);
    client.setHandshakeTimeout(2000);
    connect(&client, &ClipFileService::errorOccurred, [&](const QString &message) {
      log << message;
      err.release();
    });

    client.sendFilesTo(QStringLiteral("127.0.0.1"), port, {path});

    // refused connect, or dropped packets -> the handshake deadline. Both are
    // SOCKET-level failures and neither may leave a session behind.
    QVERIFY2(spin(err, log, 10000), qPrintable(log.join(QStringLiteral(" | "))));
    QVERIFY(log.join(QStringLiteral(" ")).contains(QStringLiteral("SOCKET")));
    QVERIFY(!client.hasPeer());
    QVERIFY(client.transfer() == nullptr);

    client.stop();
  }

private:
  /// Spins the event loop until @p sem is released (or the timeout expires).
  static bool spin(QSemaphore &sem, const QStringList &log, int timeoutMs = 20000)
  {
    QElapsedTimer timer;
    timer.start();
    while (sem.available() == 0 && timer.elapsed() < timeoutMs)
      QTest::qWait(20);
    if (sem.available() == 0 && !log.isEmpty())
      qWarning("timed out; log so far: %s", qPrintable(log.join(QStringLiteral(" | "))));
    return sem.available() > 0;
  }
};

QTEST_MAIN(ClipFileServiceTests)
#include "ClipFileServiceTests.moc"
