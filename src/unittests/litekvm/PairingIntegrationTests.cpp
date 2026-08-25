// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// Integration test: two real PairingService instances (server B + client A)
// pair over loopback TCP using the spec §4 message flow.
#include "../../lib/litekvm/DeviceIdentity.h"
#include "../../lib/litekvm/PairingService.h"
#include "../../lib/litekvm/TrustStore.h"

#include <QElapsedTimer>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtTest>

using namespace litekvm;

class PairingIntegrationTests : public QObject {
  Q_OBJECT

  // helper: create isolated identity+store
  struct Node {
    QTemporaryDir dir;
    std::optional<DeviceIdentity> identity;
    std::unique_ptr<TrustStore> store;

    bool init()
    {
      identity = DeviceIdentity::loadOrCreate(dir.path() + "/id.json");
      if (!identity)
        return false;
      store = std::make_unique<TrustStore>(dir.path() + "/trust.json");
      return true;
    }
  };

private Q_SLOTS:

  void fullPairingHandshakeOverLoopback()
  {
    Node serverNode, clientNode;
    QVERIFY(serverNode.init());
    QVERIFY(clientNode.init());

    PairingService server(*serverNode.identity, *serverNode.store);
    PairingService client(*clientNode.identity, *clientNode.store);
    QVERIFY(server.listen(25901)); // default port; loopback only in test env

    // captured signals
    QString challengeCode;
    QString challengePeerName;
    connect(&client, &PairingService::pairChallenge,
            [&](const QString &name, const QString &, const QString &code) {
              challengePeerName = name;
              challengeCode = code;
              // simulate the user typing the code shown on the server screen
              client.submitPairCode(code);
            });

    QSemaphore succeeded;
    QString pairedIdOnClient;
    connect(&client, &PairingService::pairingSucceeded,
            [&](const QString &deviceId, const QString &) {
              pairedIdOnClient = deviceId;
              succeeded.release();
            });
    QString pairedIdOnServer;
    QSemaphore serverSucceeded;
    connect(&server, &PairingService::pairingSucceeded,
            [&](const QString &deviceId, const QString &) {
              pairedIdOnServer = deviceId;
              serverSucceeded.release();
            });

    PairingService::Error clientError = PairingService::Error::Timeout;
    QSemaphore failed;
    connect(&client, &PairingService::pairingFailed, [&](PairingService::Error e) {
      clientError = e;
      failed.release();
    });

    DiscoveredPeer target;
    target.deviceId = serverNode.identity->deviceId();
    target.name = serverNode.identity->name();
    target.host = QStringLiteral("127.0.0.1");
    target.port = 25901;
    client.pairWith(target);

    // spin the event loop while background signals fire
    QElapsedTimer timer;
    timer.start();
    while (!succeeded.available() && timer.elapsed() < 10000)
      QTest::qWait(100);
    QVERIFY(succeeded.available() > 0);

    // both sides now trust each other
    QVERIFY(clientNode.store->contains(serverNode.identity->deviceId()));
    QVERIFY(serverNode.store->contains(clientNode.identity->deviceId()));
    QCOMPARE(pairedIdOnClient, serverNode.identity->deviceId());
    QCOMPARE(pairedIdOnServer, clientNode.identity->deviceId());
    QCOMPARE(serverNode.store->find(clientNode.identity->deviceId())->name,
             clientNode.identity->name());
    QCOMPARE(clientNode.store->find(serverNode.identity->deviceId())->name,
             serverNode.identity->name());
  }

  void wrongCodeFailsAndCounts()
  {
    Node serverNode, clientNode;
    QVERIFY(serverNode.init());
    QVERIFY(clientNode.init());

    PairingService server(*serverNode.identity, *serverNode.store);
    PairingService client(*clientNode.identity, *clientNode.store);
    QVERIFY(server.listen(25902));

    QSemaphore challengeSeen;
    connect(&client, &PairingService::pairChallenge, [&] { challengeSeen.release(); });

    QSemaphore wrongReported;
    connect(&client, &PairingService::pairingFailed, [&](PairingService::Error e) {
      if (e == PairingService::Error::WrongCode)
        wrongReported.release();
    });

    DiscoveredPeer target;
    target.deviceId = serverNode.identity->deviceId();
    target.host = QStringLiteral("127.0.0.1");
    target.port = 25902;
    client.pairWith(target);
    QElapsedTimer t1;
    t1.start();
    while (challengeSeen.available() == 0 && t1.elapsed() < 10000)
      QTest::qWait(100);
    QVERIFY(challengeSeen.available() > 0);

    // submit a definitely-wrong code
    client.submitPairCode(QStringLiteral("000000"));
    QElapsedTimer t2;
    t2.start();
    while (wrongReported.available() == 0 && t2.elapsed() < 10000)
      QTest::qWait(100);
    QVERIFY(wrongReported.available() > 0);

    // trust must NOT have been established
    QVERIFY(!serverNode.store->contains(clientNode.identity->deviceId()));
    QVERIFY(!clientNode.store->contains(serverNode.identity->deviceId()));

    client.cancelPairing();
  }

  void rateLimitAfterFiveBadAttempts()
  {
    Node serverNode, clientNode;
    QVERIFY(serverNode.init());
    QVERIFY(clientNode.init());

    PairingService server(*serverNode.identity, *serverNode.store);
    PairingService client(*clientNode.identity, *clientNode.store);
    QVERIFY(server.listen(25903));

    QSemaphore challengeSeen;
    connect(&client, &PairingService::pairChallenge, [&] { challengeSeen.release(); });

    PairingService::Error lastError = PairingService::Error::Timeout;
    QSemaphore errorSeen;
    connect(&client, &PairingService::pairingFailed, [&](PairingService::Error e) {
      lastError = e;
      errorSeen.release();
    });

    DiscoveredPeer target;
    target.deviceId = serverNode.identity->deviceId();
    target.host = QStringLiteral("127.0.0.1");
    target.port = 25903;
    client.pairWith(target);
    QElapsedTimer t1;
    t1.start();
    while (challengeSeen.available() == 0 && t1.elapsed() < 10000)
      QTest::qWait(100);
    QVERIFY(challengeSeen.available() > 0);

    // five bad attempts -> sixth interaction must be RATE_LIMITED
    for (int i = 0; i < 5; ++i) {
      client.submitPairCode(QStringLiteral("000000"));
      QElapsedTimer tw;
      tw.start();
      while (errorSeen.available() == 0 && tw.elapsed() < 10000)
        QTest::qWait(100);
      QVERIFY(errorSeen.available() > 0);
    }

    // after lockout the server rejects new sessions with RATE_LIMITED
    DiscoveredPeer again = target;
    PairingService freshClient(*clientNode.identity, *clientNode.store);
    QSemaphore rateLimited;
    connect(&freshClient, &PairingService::pairingFailed, [&](PairingService::Error e) {
      if (e == PairingService::Error::RateLimited)
        rateLimited.release();
    });
    freshClient.pairWith(again);
    QElapsedTimer t3;
    t3.start();
    while (rateLimited.available() == 0 && t3.elapsed() < 10000)
      QTest::qWait(100);
    QVERIFY(rateLimited.available() > 0);
  }
};

QTEST_MAIN(PairingIntegrationTests)
#include "PairingIntegrationTests.moc"
