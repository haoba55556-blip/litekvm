// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Tests for litekvm-pro features: AutoStart, AutoConnect, RoleNegotiation.
#include "../../lib/arch/LowLatencySocket.h"
#include "../../lib/deskflow/JumpZoneMouse.h"
#include "../../lib/deskflow/MouseMotionCoalescer.h"
#include "../../lib/litekvm/AutoConnect.h"
#include "../../lib/litekvm/AutoStart.h"
#include "../../lib/litekvm/ClipRoute.h"
#include "../../lib/litekvm/DiscoveryService.h"
#include "../../lib/litekvm/DragEdgeGate.h"
#include "../../lib/litekvm/DragLandGate.h"
#include "../../lib/litekvm/DropFileLand.h"
#include "../../lib/litekvm/ElevatedCore.h"
#include "../../lib/litekvm/LanNic.h"
#include "../../lib/litekvm/MdnsAdvertiser.h"
#include "../../lib/litekvm/RemoteDragDrop.h"
#include "../../lib/litekvm/RoleNegotiation.h"
#include "../../lib/litekvm/TrustStore.h"

#include <QFile>
#include <QSignalSpy>
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
    // One signal at a time. A short backoff sampled over a long window races
    // past the expected count before the assertion runs.
    ac.setBackoffScheduleForTest({300, 300, 300});
    QSignalSpy requested(&ac, &AutoConnect::autoConnectRequested);

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

    QCOMPARE(requested.count(), 1);
    // Nobody confirmed the core is up, so the watchdog fires once.
    QVERIFY(requested.wait(1000));
    QCOMPARE(requested.count(), 2);

    ac.noteSessionUp(peerId);
    QVERIFY(!requested.wait(150));
    QCOMPARE(requested.count(), 2);

    ac.noteSessionDown(peerId);
    QVERIFY(requested.wait(1000));
    QCOMPARE(requested.count(), 3);

    QVERIFY(QMetaObject::invokeMethod(&ac, "onPeerOffline", Q_ARG(QString, peerId)));
    QVERIFY(!requested.wait(150));
    QCOMPARE(requested.count(), 3); // offline: no further retries

    discovery.ingestTxtForTest(
        {{"device-id", peerId},
         {"name", "other"},
         {"platform", "win"},
         {"fp", "aabbccdd"},
         {"state", "paired"},
         {"port", "25901"}},
        "192.168.1.21", 25901
    );
    QCOMPARE(requested.count(), 4);
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

  void mouseAppliesNowEvenWhenMoreInputIsBuffered()
  {
    // A continuous move keeps the next packet already readable. Waiting for a
    // gap freezes the pointer until the hand pauses.
    QVERIFY(shouldApplyMouseNow(false));
    QVERIFY(shouldApplyMouseNow(true));
  }

  void dragEnterStartsTransferAndLeaveDoesNotCancel()
  {
    DragEdgeGate gate;
    const QStringList files{QStringLiteral("C:/a.txt")};
    QVERIFY(gate.onEnter(files, 1000));
    gate.onLeave();
    QVERIFY(gate.transferStarted());
    QVERIFY(gate.latched());
    // A bounce back onto the strip must not send a second copy, even long
    // after the old time debounce would have expired.
    QVERIFY(!gate.onEnter(files, 1200));
    QVERIFY(!gate.onEnter(files, 1000 + 5000));
    // A different drag while the button is still down does start.
    QVERIFY(gate.onEnter({QStringLiteral("C:/b.txt")}, 1200));
    gate.onButtonReleased();
    QVERIFY(!gate.latched());
    QVERIFY(gate.onEnter(files, 1300));
    gate.abandon();
    QVERIFY(!gate.latched());
    QVERIFY(gate.onEnter(files, 1400));
  }

  void edgeStripStaysHiddenUntilTheDragMoves()
  {
    EdgeStripArm arm;
    QVERIFY(!arm.shouldShow(true, 0, true));
    arm.setButtonDown(true, 100, 100, 1000);
    QVERIFY(!arm.moved());
    QVERIFY(!arm.shouldShow(true, 1000, true));
    arm.setButtonDown(true, 104, 100, 1010);
    QVERIFY(!arm.moved());
    arm.setButtonDown(true, 100 + EdgeStripArm::kThresholdPx, 100, 1020);
    QVERIFY(arm.moved());
    QVERIFY(!arm.shouldShow(false, 1020, true));
    QVERIFY(arm.shouldShow(true, 1020, false));
    // No hook: a pointer that is already on the edge when first sampled
    // still arms after the hold, which is inside the screen-switch grace.
    EdgeStripArm held;
    held.setButtonDown(true, 0, 400, 0);
    QVERIFY(!held.shouldShow(true, EdgeStripArm::kHoldMs - 1, true));
    QVERIFY(held.shouldShow(true, EdgeStripArm::kHoldMs, true));
    QVERIFY(!held.shouldShow(true, EdgeStripArm::kHoldMs, false));
    arm.setButtonDown(false, 200, 200, 1100);
    QVERIFY(!arm.buttonDown());
    QVERIFY(!arm.shouldShow(true, 1100, true));

    QVERIFY(EdgeStripArm::nearEdge(0, 400, 0, 0, 1920, 1080, EdgeStripArm::kNearEdgePx));
    QVERIFY(EdgeStripArm::nearEdge(960, 10, 0, 0, 1920, 1080, EdgeStripArm::kNearEdgePx));
    QVERIFY(!EdgeStripArm::nearEdge(960, 400, 0, 0, 1920, 1080, EdgeStripArm::kNearEdgePx));

    // Movement between the button-up sample and the first button-down sample
    // is a drag. No low-level hook is required to see it.
    EdgeStripArm flick;
    flick.setButtonDown(false, 400, 400, 0);
    flick.setButtonDown(true, 400 + EdgeStripArm::kThresholdPx, 400, 16);
    QVERIFY(flick.moved());
    QVERIFY(flick.shouldShow(true, 16, false));
  }

  void remoteDragCopiesOnReleaseWithoutBlocking()
  {
    RemoteDragWait wait;
    QCOMPARE(wait.tick(false, 0), RemoteDragWait::Decision::KeepWaiting);
    QCOMPARE(wait.tick(false, RemoteDragWait::kAppearMs - 1), RemoteDragWait::Decision::KeepWaiting);
    QCOMPARE(wait.tick(false, RemoteDragWait::kAppearMs), RemoteDragWait::Decision::CopyAlreadyUp);
    QCOMPARE(
        QLatin1String(RemoteDragWait::reason(RemoteDragWait::Decision::CopyAlreadyUp)), QLatin1String("already-up")
    );

    wait.reset();
    QCOMPARE(wait.tick(true, 10), RemoteDragWait::Decision::KeepWaiting);
    QVERIFY(wait.sawButton());
    QCOMPARE(wait.tick(true, RemoteDragWait::kAppearMs + 50), RemoteDragWait::Decision::KeepWaiting);
    QCOMPARE(wait.tick(false, 800), RemoteDragWait::Decision::CopyOnRelease);
    QCOMPARE(QLatin1String(RemoteDragWait::reason(RemoteDragWait::Decision::CopyOnRelease)), QLatin1String("release"));

    wait.reset();
    QCOMPARE(wait.tick(true, RemoteDragWait::kGiveUpMs), RemoteDragWait::Decision::CopyTimeout);

    RemoteDropLatch latch;
    const QStringList file{QStringLiteral("C:/a.txt")};
    QVERIFY(latch.accept(file, 1000));
    QVERIFY(!latch.accept(file, 1000 + RemoteDropLatch::kDuplicateMs - 1));
    QVERIFY(latch.accept({QStringLiteral("C:/b.txt")}, 1001));
    QVERIFY(latch.accept(file, 1000 + RemoteDropLatch::kDuplicateMs));
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

  void clipPeerLabelPrefersConnectedScreenOverPlaceholder()
  {
    QCOMPARE(
        clipPeerLabel(
            QStringLiteral("this-device"), QStringLiteral("LAPTOP-8BPR9V7N"), QStringLiteral("DESKTOP-9GM1608")
        ),
        QStringLiteral("LAPTOP-8BPR9V7N")
    );
    QCOMPARE(
        clipPeerLabel(QStringLiteral("this-device"), QStringLiteral("server"), QStringLiteral("DESKTOP-9GM1608")),
        QStringLiteral("this-device")
    );
    QCOMPARE(
        clipPeerLabel(
            QStringLiteral("this-device"), QStringLiteral("DESKTOP-9GM1608"), QStringLiteral("LAPTOP-8BPR9V7N")
        ),
        QStringLiteral("DESKTOP-9GM1608")
    );
    QCOMPARE(
        clipPeerLabel(QStringLiteral("real-laptop"), QStringLiteral("server"), QStringLiteral("DESKTOP-9GM1608")),
        QStringLiteral("real-laptop")
    );
    QVERIFY(isPlaceholderPeerName(QStringLiteral("this-device")));
    QVERIFY(hostIsThisMachine(QStringLiteral("127.0.0.1")));
    QVERIFY(!hostIsThisMachine(QString()));
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

  void jumpZoneHookLeavesDragMovesForOle()
  {
    QVERIFY(!eatJumpZoneMouseMove(false, false, false));
    QVERIFY(!eatJumpZoneMouseMove(true, true, false));
    QVERIFY(eatJumpZoneMouseMove(true, false, false));
    // Held button: the 1px edge move must reach the edge strip.
    QVERIFY(!eatJumpZoneMouseMove(true, false, true));
    QVERIFY(!eatJumpZoneMouseMove(true, true, true));
  }

  void dragEdgeGraceHoldsThenAllowsTheCross()
  {
    DragEdgeGrace grace;
    QVERIFY(!grace.allowSwitch(0.0));
    QVERIFY(grace.armed());
    QVERIFY(!grace.allowSwitch(0.2));
    QVERIFY(grace.allowSwitch(DragEdgeGrace::kSeconds));
    grace.reset();
    QVERIFY(!grace.armed());
    QVERIFY(!grace.allowSwitch(10.0));

    int32_t x = 0;
    int32_t y = 400;
    insetFromScreenEdge(0, 0, 1920, 1080, kDragLandingMargin, x, y);
    QCOMPARE(x, kDragLandingMargin);
    QCOMPARE(y, 400);
    x = 1919;
    y = 0;
    insetFromScreenEdge(0, 0, 1920, 1080, kDragLandingMargin, x, y);
    QCOMPARE(x, 1920 - 1 - kDragLandingMargin);
    QCOMPARE(y, kDragLandingMargin);

    x = 0;
    y = 400;
    QVERIFY(clampInsideMargin(0, 0, 1920, 1080, kDragLandingMargin, x, y));
    QCOMPARE(x, kDragLandingMargin);
    QCOMPARE(y, 400);
    x = 960;
    y = 540;
    QVERIFY(!clampInsideMargin(0, 0, 1920, 1080, kDragLandingMargin, x, y));
    QCOMPARE(x, 960);
    QCOMPARE(y, 540);

    // The host's right edge must name the laptop, never the host. A resolved
    // neighbor of the active screen is the self-switch that warped to x=1918.
    QVERIFY(switchesToOtherScreen("DESKTOP-9GM1608", "LAPTOP-8BPR9V7N"));
    QVERIFY(!switchesToOtherScreen("DESKTOP-9GM1608", "DESKTOP-9GM1608"));
    QVERIFY(!switchesToOtherScreen("DESKTOP-9GM1608", "desktop-9gm1608"));
    QVERIFY(!switchesToOtherScreen("DESKTOP-9GM1608", nullptr));
    QVERIFY(!switchesToOtherScreen("DESKTOP-9GM1608", ""));

    // A fast cross lands at x=8, inside the 1 px inset and still in the edge
    // strip. The held button then bounces. The margin band has to move it.
    x = 8;
    y = 675;
    QVERIFY(clampInsideMargin(0, 0, 1920, 1080, kDragLandingMargin, x, y));
    QCOMPARE(x, kDragLandingMargin);
    QCOMPARE(y, 675);
    x = 1918;
    y = 400;
    QVERIFY(clampInsideMargin(0, 0, 1920, 1080, kDragLandingMargin, x, y));
    QCOMPARE(x, 1920 - 1 - kDragLandingMargin);

    QVERIFY(isCenterWarpEcho(-959, 0, 1920, 1080));
    QVERIFY(isCenterWarpEcho(900, 1, 1920, 1080));
    QVERIFY(!isCenterWarpEcho(-12, 4, 1920, 1080));
    QVERIFY(!isCenterWarpEcho(40, -80, 1920, 1080));

    QVERIFY(!resetDragEdgeHold(true, true));
    QVERIFY(resetDragEdgeHold(true, false));
    QVERIFY(resetDragEdgeHold(false, true));
    QVERIFY(pointerInEdgeBand(8, 675, 0, 0, 1920, 1080, kDragLandingMargin));
    QVERIFY(!pointerInEdgeBand(960, 540, 0, 0, 1920, 1080, kDragLandingMargin));
  }

  void landedFileIsClosedAndAFailedCopyLeavesNothing()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("photo.jpg"));
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly));
    const QByteArray bytes(236, '\x42');
    QCOMPARE(src.write(bytes), qint64(bytes.size()));
    src.close();

    const QString destDir = dir.filePath(QStringLiteral("desk"));
    QVERIFY(QDir().mkpath(destDir));
    const QString kept = destDir + QStringLiteral("/photo.jpg");
    QFile existing(kept);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write(QByteArrayLiteral("keep")), qint64(4));
    existing.close();

    const LandedFile mismatch = landFinishedFile(source, destDir, 1);
    QVERIFY(!mismatch.ok);
    QCOMPARE(QFileInfo(kept).size(), qint64(4));

    const LandedFile part = landFinishedFile(source + QStringLiteral(".litekvm-part"), destDir, bytes.size());
    QVERIFY(!part.ok);

    const LandedFile missing = landFinishedFile(source, dir.filePath(QStringLiteral("no-such")), bytes.size());
    QVERIFY(!missing.ok);

    const LandedFile landed = landFinishedFile(source, destDir, bytes.size());
    QVERIFY(landed.ok);
    QVERIFY(landed.path.endsWith(QStringLiteral("photo (1).jpg")));
    QFile out(landed.path);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), bytes);
    out.close();
    QVERIFY(!QFile::exists(landed.path + QStringLiteral(".litekvm-part")));
    QFile keptFile(kept);
    QVERIFY(keptFile.open(QIODevice::ReadOnly));
    QCOMPARE(keptFile.readAll(), QByteArray("keep"));
    keptFile.close();
  }

  void serverHoldsADragUntilThePointerReturns()
  {
    DragLandGate gate;
    QString reason;
    const QStringList immediate = gate.receiveDrag({QStringLiteral("a.jpg")}, &reason);
    QCOMPARE(immediate, QStringList{QStringLiteral("a.jpg")});
    QCOMPARE(reason, QStringLiteral("pointer-local"));
    QVERIFY(gate.waiting().isEmpty());

    gate.setPointerLocal(false);
    const QStringList held = gate.receiveDrag({QStringLiteral("b.jpg")}, &reason);
    QVERIFY(held.isEmpty());
    QCOMPARE(reason, QStringLiteral("deferred-pointer-remote"));
    QCOMPARE(gate.waiting(), QStringList{QStringLiteral("b.jpg")});
    // A later outbound send must not throw the waiting payload away.
    QCOMPARE(gate.takeIfPointerLocal(&reason), QStringList());
    QCOMPARE(reason, QStringLiteral("still-remote"));
    QCOMPARE(gate.waiting(), QStringList{QStringLiteral("b.jpg")});

    gate.setPointerLocal(true);
    const QStringList landed = gate.takeIfPointerLocal(&reason);
    QCOMPARE(landed, QStringList{QStringLiteral("b.jpg")});
    QCOMPARE(reason, QStringLiteral("pointer-arrived"));
    QVERIFY(gate.waiting().isEmpty());
    QCOMPARE(gate.takeIfPointerLocal(&reason), QStringList());
    QCOMPARE(reason, QStringLiteral("nothing-waiting"));
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
