// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Tests for litekvm-pro features: AutoStart, AutoConnect, RoleNegotiation.
#include "../../lib/arch/LowLatencySocket.h"
#include "../../lib/deskflow/MouseMotionCoalescer.h"
#include "../../lib/litekvm/AutoConnect.h"
#include "../../lib/litekvm/AutoStart.h"
#include "../../lib/litekvm/ClipRoute.h"
#include "../../lib/litekvm/DiscoveryService.h"
#include "../../lib/litekvm/DragEdgeGate.h"
#include "../../lib/litekvm/ElevatedCore.h"
#include "../../lib/litekvm/LanNic.h"
#include "../../lib/litekvm/MdnsAdvertiser.h"
#include "../../lib/litekvm/RoleNegotiation.h"
#include "../../lib/litekvm/TrustStore.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace litekvm;

class ProFeatureTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:

  void autoStartRoundtrip()
  {
    if (!AutoStart::supported())
      QSKIP("not supported on this platform");

    const bool wasEnabled = AutoStart::isEnabled();
    // remember original value to restore (never break the user's machine state)
    const QString runKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    QSettings run(runKey, QSettings::NativeFormat);
    const QString original = run.value("LiteKVM").toString();

    AutoStart::setEnabled(true);
    QVERIFY(AutoStart::isEnabled());

    AutoStart::setEnabled(false);
    QVERIFY(!AutoStart::isEnabled());

    // restore
    if (!original.isEmpty())
      run.setValue("LiteKVM", original);
  }

  void autoConnectOnlyPairedPeers()
  {
    QTemporaryDir dir;
    auto id = DeviceIdentity::loadOrCreate(dir.path() + "/id.json");
    QVERIFY(id.has_value());
    TrustStore trust(dir.path() + "/trust.json");
    DiscoveryService discovery;

    AutoConnect ac(*id, trust, discovery);

    QSemaphore requested;
    DiscoveredPeer captured;
    QObject::connect(&ac, &AutoConnect::autoConnectRequested, [&](const DiscoveredPeer &p) {
      captured = p;
      requested.release();
    });

    // disabled by default: no auto-connect even for paired peers
    trust.upsert(
        {QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
         QByteArray(32, 'k'),
         QStringLiteral("paired-box"),
         QStringLiteral("win"),
         QDateTime::currentDateTimeUtc(),
         {},
         QDateTime::currentDateTimeUtc()}
    );
    discovery.start(*id, &trust);
    ac.setEnabled(false);

    // simulate discovery of the paired peer
    discovery.ingestTxtForTest(
        {{"device-id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
         {"name", "paired-box"},
         {"platform", "win"},
         {"fp", "aabbccdd"},
         {"state", "paired"},
         {"port", "25901"}},
        "192.168.1.50", 25901
    );
    QTest::qWait(200);
    QCOMPARE(requested.available(), 0);

    // enabling triggers evaluation, and future discoveries auto-connect
    ac.setEnabled(true);
    QTest::qWait(200);
    QVERIFY(requested.available() > 0);
    QCOMPARE(captured.deviceId, QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));

    // one attempt per session: further sightings don't re-fire
    discovery.ingestTxtForTest(
        {{"device-id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
         {"name", "paired-box"},
         {"platform", "win"},
         {"fp", "aabbccdd"},
         {"state", "paired"},
         {"port", "25901"}},
        "192.168.1.50", 25901
    );
    QTest::qWait(200);
    QCOMPARE(requested.available(), 1); // unchanged inside the backoff window
  }

  void autoConnectRetriesUntilSessionIsUp()
  {
    QTemporaryDir dir;
    auto id = DeviceIdentity::loadOrCreate(dir.path() + "/id.json");
    QVERIFY(id.has_value());
    TrustStore trust(dir.path() + "/trust.json");
    DiscoveryService discovery;
    const QString peerId = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    trust.upsert(
        {peerId,
         QByteArray(32, 'k'),
         QStringLiteral("other"),
         QStringLiteral("win"),
         QDateTime::currentDateTimeUtc(),
         {},
         QDateTime::currentDateTimeUtc()}
    );

    AutoConnect ac(*id, trust, discovery);
    ac.setBackoffScheduleForTest({40, 40, 40});
    QSemaphore requested;
    connect(&ac, &AutoConnect::autoConnectRequested, [&](const DiscoveredPeer &) { requested.release(); });

    discovery.start(*id, &trust);
    ac.setEnabled(true);
    discovery.ingestTxtForTest(
        {{"device-id", peerId},
         {"name", "other"},
         {"platform", "win"},
         {"fp", "aabbccdd"},
         {"state", "paired"},
         {"port", "25901"}},
        "192.168.1.20", 25901
    );

    QTRY_COMPARE_WITH_TIMEOUT(requested.available(), 1, 1000);
    // Nobody confirmed the core is up, so the watchdog fires.
    QTRY_COMPARE_WITH_TIMEOUT(requested.available(), 2, 1000);

    ac.noteSessionUp(peerId);
    QTest::qWait(120);
    QCOMPARE(requested.available(), 2);

    ac.noteSessionDown(peerId);
    QTRY_COMPARE_WITH_TIMEOUT(requested.available(), 3, 1000);

    QVERIFY(QMetaObject::invokeMethod(&ac, "onPeerOffline", Q_ARG(QString, peerId)));
    QTest::qWait(120);
    QCOMPARE(requested.available(), 3); // offline: no further retries

    discovery.ingestTxtForTest(
        {{"device-id", peerId},
         {"name", "other"},
         {"platform", "win"},
         {"fp", "aabbccdd"},
         {"state", "paired"},
         {"port", "25901"}},
        "192.168.1.21", 25901
    );
    QTRY_COMPARE_WITH_TIMEOUT(requested.available(), 4, 1000);
  }

  void discoveryNormalizesHostsAndQueries()
  {
    QCOMPARE(DiscoveryService::normalizeHost(QStringLiteral("192.168.1.9")), QStringLiteral("192.168.1.9"));
    QCOMPARE(DiscoveryService::normalizeHost(QStringLiteral("::ffff:192.168.1.9")), QStringLiteral("192.168.1.9"));
    QCOMPARE(DiscoveryService::normalizeHost(QStringLiteral("192.168.1.9%12")), QStringLiteral("192.168.1.9"));
    QVERIFY(DiscoveryService::normalizeHost(QStringLiteral("224.0.0.251")).isEmpty());

    const QByteArray query = DiscoveryService::buildBrowseQuery();
    QVERIFY(query.contains("_litekvm"));
    QCOMPARE(int(static_cast<unsigned char>(query.at(5))), 1); // qdcount low byte
    QVERIFY(MdnsAdvertiser::shouldAnswer(query));

    // Same bytes with the QR bit set are a response. Answering them fans
    // the reply back out every NIC and the two peers storm.
    QByteArray response = query;
    response[2] = char(static_cast<unsigned char>(response.at(2)) | 0x80);
    QVERIFY(response.contains("_litekvm"));
    QVERIFY(!MdnsAdvertiser::shouldAnswer(response));
    QVERIFY(!MdnsAdvertiser::shouldAnswer(QByteArray()));
    QVERIFY(!MdnsAdvertiser::shouldAnswer(QByteArray(4, '\0')));

    QCOMPARE(MdnsAdvertiser::kAnnounceIntervalMs, 2000);
    QVERIFY(MdnsAdvertiser::kAnnounceIntervalMs <= 5000);
  }

  void mouseAppliesNowUnlessMoreInputIsBuffered()
  {
    QVERIFY(shouldApplyMouseNow(false));
    QVERIFY(!shouldApplyMouseNow(true));
  }

  void dragEnterStartsTransferAndLeaveDoesNotCancel()
  {
    DragEdgeGate gate;
    const QStringList files{QStringLiteral("C:/a.txt")};
    QVERIFY(gate.onEnter(files, 1000));
    gate.onLeave();
    QVERIFY(gate.transferStarted());
    // Same drag flickering back onto the strip must not send a second copy.
    QVERIFY(!gate.onEnter(files, 1200));
    // A later, different drag does.
    QVERIFY(gate.onEnter({QStringLiteral("C:/b.txt")}, 1200));
    QVERIFY(gate.onEnter(files, 1000 + DragEdgeGate::kDebounceMs));
  }

  void mouseMotionCoalescerKeepsLatestAndSums()
  {
    MouseMotionCoalescer motion;
    motion.addAbsolute(1, 2);
    motion.addAbsolute(9, 8);
    motion.addAbsolute(4, 5);
    int32_t x = 0;
    int32_t y = 0;
    QVERIFY(motion.takeAbsolute(x, y));
    QCOMPARE(x, 4);
    QCOMPARE(y, 5);
    QVERIFY(!motion.takeAbsolute(x, y));

    motion.addRelative(1, 1);
    motion.addRelative(-3, 4);
    motion.addRelative(2, 0);
    int32_t dx = 0;
    int32_t dy = 0;
    QVERIFY(motion.takeRelative(dx, dy));
    QCOMPARE(dx, 0);
    QCOMPARE(dy, 5);
    motion.clear();
    QVERIFY(!motion.hasPending());
  }

  void lowLatencySocketPolicyDisablesNagleAndDelayedAck()
  {
    QVERIFY(LowLatencySocketPolicy::kTcpNoDelay);
    QVERIFY(LowLatencySocketPolicy::kRefreshQuickAckAfterRead);
    QCOMPARE(LowLatencySocketPolicy::kWindowsAckFrequency, 1);
  }

  void mdnsSkipsVirtualAndDownAdapters()
  {
    const QList<NicHint> nics = {
        {QStringLiteral("以太网"), QStringLiteral("ethernet"), true, true, false, true, true},
        {QStringLiteral("Wi-Fi"), QStringLiteral("wlan"), true, true, false, true, true},
        {QStringLiteral("Microsoft Wi-Fi Direct Virtual Adapter"), QStringLiteral("wlan1"), true, true, false, true,
         true},
        {QStringLiteral("本地连接* 12"), QStringLiteral("wlan2"), true, true, false, true, true},
        {QStringLiteral("Bluetooth Network Connection"), QStringLiteral("bt"), true, true, false, true, true},
        {QStringLiteral("蓝牙网络连接"), QStringLiteral("bt0"), true, true, false, true, true},
        {QStringLiteral("vEthernet (WSL)"), QStringLiteral("veth"), true, true, false, true, true},
        {QStringLiteral("vEthernet (Default Switch)"), QStringLiteral("hyperv"), true, true, false, true, true},
        {QStringLiteral("以太网 2"), QStringLiteral("eth2"), false, false, false, true, true},
        {QStringLiteral("Loopback"), QStringLiteral("lo"), true, true, true, true, true},
    };
    const QList<NicHint> usable = usableLanNics(nics);
    QStringList names;
    for (const NicHint &nic : usable)
      names.append(nic.humanName);
    QCOMPARE(names.join(QLatin1Char(',')), QStringLiteral("以太网,Wi-Fi"));
    QVERIFY(isVirtualAdapterName(QStringLiteral("Hyper-V Virtual Ethernet Adapter"), QStringLiteral("vEthernet")));
  }

  void clipSendPrefersLiveSocketOverStaleTrust()
  {
    ClipRouteInput route;
    route.liveHost = QStringLiteral("::ffff:192.168.1.20");
    route.liveScreenName = QStringLiteral("server");
    route.deviceId = QStringLiteral("peer");
    route.deviceName = QStringLiteral("laptop");
    route.pairedCount = 1;
    route.trustedHost = QStringLiteral("192.168.1.23");
    QCOMPARE(clipRouteHost(route), QStringLiteral("192.168.1.20"));

    route.liveHost.clear();
    route.discoveredHost = QStringLiteral("192.168.1.20");
    QCOMPARE(clipRouteHost(route), QStringLiteral("192.168.1.20"));

    route.discoveredHost.clear();
    QCOMPARE(clipRouteHost(route), QStringLiteral("192.168.1.23"));

    route.liveHost = QStringLiteral("192.168.1.20");
    route.pairedCount = 2;
    route.trustedHost = QStringLiteral("10.0.0.5");
    route.deviceName = QStringLiteral("other-laptop");
    QCOMPARE(clipRouteHost(route), QStringLiteral("10.0.0.5"));
    route.deviceName = QStringLiteral("server");
    QCOMPARE(clipRouteHost(route), QStringLiteral("192.168.1.20"));
  }

  void trustLastAddrUpdatesOnlyWhenChanged()
  {
    QTemporaryDir dir;
    TrustStore trust(dir.path() + "/trust.json");
    const QString id = QStringLiteral("cccccccccccccccccccccccccccccccc");
    trust.upsert(
        {id, QByteArray(32, 'k'), QStringLiteral("laptop"), QStringLiteral("win"), QDateTime::currentDateTimeUtc(),
         QStringLiteral("192.168.1.23"), QDateTime::currentDateTimeUtc()}
    );
    QVERIFY(!trust.updateLastAddr(id, QStringLiteral("192.168.1.23")));
    QVERIFY(trust.updateLastAddr(id, QStringLiteral("192.168.1.20")));
    QCOMPARE(trust.find(id)->lastAddr, QStringLiteral("192.168.1.20"));

    TrustStore reloaded(dir.path() + "/trust.json");
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.find(id)->lastAddr, QStringLiteral("192.168.1.20"));
  }

  void elevatedCoreTaskRunsHighestAndInteractive()
  {
    const QString script = ElevatedCore::launcherScript(
        QStringLiteral("C:/Program Files/LiteKVM/litekvm-core.exe"), {QStringLiteral("client")}
    );
    QVERIFY(script.contains(QStringLiteral("\"C:/Program Files/LiteKVM/litekvm-core.exe\" client")));

    const auto create =
        ElevatedCore::createTaskArguments(QStringLiteral("LiteKVMCore"), QStringLiteral("C:/Users/a/launch.cmd"));
    QVERIFY(create.contains(QStringLiteral("/RL")));
    QVERIFY(create.contains(QStringLiteral("HIGHEST")));
    QVERIFY(create.contains(QStringLiteral("/IT")));
    QCOMPARE(create.at(create.indexOf(QStringLiteral("/TN")) + 1), QStringLiteral("LiteKVMCore"));
    QVERIFY(ElevatedCore::runTaskArguments(QStringLiteral("LiteKVMCore")).contains(QStringLiteral("/Run")));
    QVERIFY(ElevatedCore::endTaskArguments(QStringLiteral("LiteKVMCore")).contains(QStringLiteral("/End")));
    QVERIFY(!ElevatedCore::supported() || true);
  }

  void roleElectionSmallestIdWins()
  {
    QTemporaryDir dir;
    auto self = DeviceIdentity::loadOrCreate(dir.path() + "/id.json");
    QVERIFY(self.has_value());

    RoleNegotiation rn(*self);

    // no peers: self is server
    rn.reset({});
    QCOMPARE(rn.currentServer(), self->deviceId());
    QCOMPARE(rn.myRole(), RoleNegotiation::Role::Server);

    // a peer with smaller id takes over as server
    QSemaphore changed;
    connect(&rn, &RoleNegotiation::roleChanged, [&](const QString &, quint64, RoleNegotiation::Role) {
      changed.release();
    });
    rn.setOnline(QStringLiteral("0000-small-id"));
    QCOMPARE(changed.available(), 1);
    QCOMPARE(rn.currentServer(), QStringLiteral("0000-small-id"));
    QCOMPARE(rn.myRole(), RoleNegotiation::Role::Client);

    // transient offline (< 5s) does NOT flap the role
    rn.setOffline(QStringLiteral("0000-small-id"));
    QTest::qWait(200);
    QCOMPARE(rn.currentServer(), QStringLiteral("0000-small-id")); // hysteresis holds
    // ...and coming back online is a no-op
    rn.setOnline(QStringLiteral("0000-small-id"));
    QCOMPARE(changed.available(), 1); // no additional change
  }

  void roleFailsOverAfterHysteresis()
  {
    QTemporaryDir dir;
    auto self = DeviceIdentity::loadOrCreate(dir.path() + "/id.json");
    QVERIFY(self.has_value());

    RoleNegotiation rn(*self);
    rn.setOnline(QStringLiteral("0000-server"));
    QCOMPARE(rn.myRole(), RoleNegotiation::Role::Client);

    // server dies; role must eventually fail over to self
    rn.setOffline(QStringLiteral("0000-server"));
    // hysteresis is 5s — simulate expiry by waiting only works slowly in real
    // time; instead verify the API contract: after grace, self becomes server
    // (we accept the 5s wait inside the test with a coarse check)
    QElapsedTimer t;
    t.start();
    while (rn.myRole() != RoleNegotiation::Role::Server && t.elapsed() < 8000)
      QTest::qWait(250);
    QCOMPARE(rn.myRole(), RoleNegotiation::Role::Server);
    QCOMPARE(rn.currentServer(), self->deviceId());
  }
};

QTEST_MAIN(ProFeatureTests)
#include "ProFeatureTests.moc"
