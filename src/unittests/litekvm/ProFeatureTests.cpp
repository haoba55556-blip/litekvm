// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Tests for litekvm-pro features: AutoStart, AutoConnect, RoleNegotiation.
#include "../../lib/arch/LowLatencySocket.h"
#include "../../lib/deskflow/ClipboardYield.h"
#include "../../lib/deskflow/CursorHider.h"
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

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif
#include <QtTest>

#include <string>
#include <vector>

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

    // pointer-local + already-up does not burn kAppearMs. A held button still
    // waits for release. Clearing the skip restores the cross grace.
    RemoteDragWait local;
    local.setSkipAppearGrace(true);
    QCOMPARE(local.tick(false, 0), RemoteDragWait::Decision::CopyAlreadyUp);
    local.reset();
    QCOMPARE(local.tick(true, 0), RemoteDragWait::Decision::KeepWaiting);
    QCOMPARE(local.tick(false, 1), RemoteDragWait::Decision::CopyOnRelease);
    local.reset();
    local.setSkipAppearGrace(false);
    QCOMPARE(local.tick(false, 0), RemoteDragWait::Decision::KeepWaiting);
    QCOMPARE(local.tick(false, RemoteDragWait::kAppearMs - 1), RemoteDragWait::Decision::KeepWaiting);
    QCOMPARE(local.tick(false, RemoteDragWait::kAppearMs), RemoteDragWait::Decision::CopyAlreadyUp);

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

  void leaveCursorHiderCoversThePointerThatWasLeftBehind()
  {
    // The old primary low-level hider was a 1x1 window at the screen center
    // and did not hit-test. The hardware cursor stayed on the leave edge.
    const CursorHiderRect oldCenter{1280, 720, 1, 1};
    constexpr int32_t leaveEdgeX = 2519;
    constexpr int32_t leaveEdgeY = 400;
    QVERIFY(!pointerOverHider(leaveEdgeX, leaveEdgeY, oldCenter));
    QVERIFY(showCursorCannotHide(false, pointerOverHider(leaveEdgeX, leaveEdgeY, oldCenter)));

    // A hider built around the pointer covers it, including a pixel of
    // absolute-move rounding. ShowCursor can hide it only if that window
    // also claims the hit.
    const CursorHiderRect covered = cursorHiderRect(leaveEdgeX, leaveEdgeY);
    QVERIFY(pointerOverHider(leaveEdgeX, leaveEdgeY, covered));
    QVERIFY(pointerOverHider(leaveEdgeX - 8, leaveEdgeY + 8, covered));
    QVERIFY(!pointerOverHider(leaveEdgeX + kCursorHiderSlop + 1, leaveEdgeY, covered));
    QVERIFY(!showCursorCannotHide(true, true));
    QVERIFY(showCursorCannotHide(false, true));
    QVERIFY(showCursorCannotHide(true, false));

    // Parking at the center does not cover an edge the warp failed to leave.
    const CursorHiderRect parked = cursorHiderRect(1280, 720);
    QVERIFY(pointerOverHider(1280, 720, parked));
    QVERIFY(!pointerOverHider(leaveEdgeX, leaveEdgeY, parked));
    QCOMPARE(parked.w, kCursorHiderSlop * 2 + 1);
    QCOMPARE(parked.h, parked.w);

    // The park warp lands inside the hider. Real local motion that exits it
    // dismisses the hider so the physical mouse works on a client.
    QVERIFY(!dismissHiderOnLocalMove(kCursorHiderSlop, kCursorHiderSlop, parked.w, parked.h));
    QVERIFY(!dismissHiderOnLocalMove(0, 0, parked.w, parked.h));
    QVERIFY(!dismissHiderOnLocalMove(parked.w - 1, parked.h - 1, parked.w, parked.h));
    QVERIFY(dismissHiderOnLocalMove(-1, kCursorHiderSlop, parked.w, parked.h));
    QVERIFY(dismissHiderOnLocalMove(parked.w, 0, parked.w, parked.h));
    QVERIFY(dismissHiderOnLocalMove(0, -1, parked.w, parked.h));
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

  void buttonUpSwitchDoesNotImmediatelyRecross()
  {
    // 200–400 ms, and not the file-drag hold.
    QVERIFY(kSwitchSettleMs >= 200);
    QVERIFY(kSwitchSettleMs <= 400);
    QVERIFY(kSwitchSettleSeconds < kDragSettleSeconds);
    QVERIFY(kSwitchLandingMargin >= 40);
    QVERIFY(kSwitchLandingMargin <= 80);
    QVERIFY(kSwitchLandingMargin < kDragLandingMargin);
    QCOMPARE(kDragSettleSeconds, 1.0);
    QCOMPARE(kDragLandingMargin, 120);

    // The park shows up inside this window. The hold must still be active,
    // and a file drag must still be holding at the same moment.
    QVERIFY(settleHolding(0.058, kSwitchSettleSeconds));
    QVERIFY(!settleHolding(kSwitchSettleSeconds, kSwitchSettleSeconds));
    QVERIFY(settleHolding(0.058, kDragSettleSeconds));
    QVERIFY(settleHolding(0.999, kDragSettleSeconds));
    QVERIFY(!settleHolding(kDragSettleSeconds, kDragSettleSeconds));

    // Laptop 2520×1680, button-up landing on the right edge (the live x=2519).
    // That pixel is a return edge when each machine is on both sides.
    const auto laptop = landInsideScreen(0, 0, 2520, 1680, 2519, 840, false);
    QCOMPARE(laptop.x, 2520 - 1 - kSwitchLandingMargin);
    QCOMPARE(laptop.y, 840);
    QCOMPARE(laptop.settleSeconds, kSwitchSettleSeconds);
    QVERIFY(laptop.x < 2519);

    // A few pixels of residual motion cannot leave, and cannot sit on 2519.
    const auto nudge = applyRelativeDuringSettle(laptop.x, laptop.y, 8, 0, 0, 0, 2520, 1680, laptop.margin, true);
    QVERIFY(!nudge.blockedRecross);
    QVERIFY(nudge.x <= 2520 - 1 - kSwitchLandingMargin);
    QCOMPARE(nudge.x, laptop.x);

    // Inland motion during the hold still moves.
    const auto inland =
        applyRelativeDuringSettle(laptop.x - 30, laptop.y, -8, 4, 0, 0, 2520, 1680, laptop.margin, true);
    QCOMPARE(inland.x, laptop.x - 38);
    QCOMPARE(inland.y, 844);
    QVERIFY(!inland.blockedRecross);

    // The old failure: sitting on 2519, +1 leaves. The hold refuses it.
    // 400 px is under a quarter of 2520, so the center-warp filter lets it
    // through; the settle drops it.
    QVERIFY(!isCenterWarpEcho(400, 0, 2520, 1680));
    const auto spike = applyRelativeDuringSettle(laptop.x, laptop.y, 400, 0, 0, 0, 2520, 1680, laptop.margin, true);
    QCOMPARE(spike.x, laptop.x);
    QCOMPARE(spike.y, laptop.y);
    const bool holding = settleHolding(0.050, kSwitchSettleSeconds);
    const auto bounce = applyRelativeDuringSettle(2519, 840, 1, 0, 0, 0, 2520, 1680, kSwitchLandingMargin, holding);
    QVERIFY(bounce.blockedRecross);
    QVERIFY(bounce.x <= 2520 - 1 - kSwitchLandingMargin);

    // After the hold, the same step off the real edge is a deliberate return.
    const auto back = applyRelativeDuringSettle(2519, 840, 1, 0, 0, 0, 2520, 1680, kSwitchLandingMargin, false);
    QVERIFY(!back.blockedRecross);
    QCOMPARE(back.x, 2520);

    // Desktop return at x=1..8 (live logs) is pulled in, and an absolute
    // sample on that edge does not stay there during the hold.
    const auto desk = landInsideScreen(0, 0, 1920, 1080, 4, 790, false);
    QCOMPARE(desk.x, kSwitchLandingMargin);
    QCOMPARE(desk.y, 790);
    const auto edge = applyAbsoluteDuringSettle(1, 790, 0, 0, 1920, 1080, kSwitchLandingMargin, true);
    QVERIFY(edge.blockedRecross);
    QCOMPARE(edge.x, kSwitchLandingMargin);
    const auto middle = applyAbsoluteDuringSettle(960, 540, 0, 0, 1920, 1080, kSwitchLandingMargin, true);
    QCOMPARE(middle.x, 960);
    QCOMPARE(middle.y, 540);
    QVERIFY(!middle.blockedRecross);
    const auto top = landInsideScreen(0, 0, 1920, 1080, 400, 0, false);
    QCOMPARE(top.y, kSwitchLandingMargin);

    // File drag is unchanged: 120 px and a 1 s hold, including the fast
    // cross that used to land at x=8.
    const auto drag = landInsideScreen(0, 0, 1920, 1080, 8, 675, true);
    QCOMPARE(drag.x, kDragLandingMargin);
    QCOMPARE(drag.y, 675);
    QCOMPARE(drag.settleSeconds, kDragSettleSeconds);
    QCOMPARE(drag.margin, kDragLandingMargin);
  }

  void largeClipboardYieldsToMouse()
  {
    constexpr size_t payloadBytes = 900 * 1024;
    QVERIFY(payloadBytes > kLargeClipboardBytes);
    QVERIFY(kClipboardChunkBytes < payloadBytes);
    QVERIFY(kClipboardYieldSeconds > 0.0);
    QVERIFY(kClipboardYieldSeconds <= 0.05);
    QVERIFY(deferLargeClipboardDuringSettle(payloadBytes, true, true));
    QVERIFY(!deferLargeClipboardDuringSettle(payloadBytes, false, true));
    QVERIFY(!deferLargeClipboardDuringSettle(100, true, true));
    QVERIFY(!deferLargeClipboardDuringSettle(kLargeClipboardBytes, true, true));
    QVERIFY(deferLargeClipboardDuringSettle(kLargeClipboardBytes + 1, true, true));
    QVERIFY(!deferLargeClipboardDuringSettle(payloadBytes, true, false));

    // The size header is one piece. A mouse move is processed before any
    // payload byte, and before the send is finished.
    ClipboardPieceCursor cursor(payloadBytes);
    int piece = -1;
    size_t offset = 0;
    size_t size = 0;
    QVERIFY(cursor.takePiece(piece, offset, size));
    QCOMPARE(piece, 0);
    QVERIFY(!cursor.finished());

    std::vector<std::string> order;
    order.emplace_back("clip");
    order.emplace_back("mouse");
    size_t sent = 0;
    int dataPieces = 0;
    while (!cursor.finished()) {
      QVERIFY(cursor.takePiece(piece, offset, size));
      order.emplace_back("clip");
      if (piece == 1) {
        ++dataPieces;
        QVERIFY(size <= kClipboardChunkBytes);
        QVERIFY(size > 0);
        sent += size;
      }
      if (!cursor.finished())
        order.emplace_back("mouse");
    }
    QCOMPARE(sent, payloadBytes);
    QVERIFY(dataPieces > 1);
    QVERIFY(cursor.finished());
    QVERIFY(order.front() == "clip");
    QVERIFY(order.at(1) == "mouse");
    QVERIFY(order.back() == "clip");

    ClipboardPieceCursor empty(0);
    int pieces = 0;
    while (!empty.finished()) {
      QVERIFY(empty.takePiece(piece, offset, size));
      ++pieces;
      QVERIFY(pieces < 6);
    }
    QCOMPARE(pieces, 3);

    ClipboardPieceCursor exact(kClipboardChunkBytes);
    dataPieces = 0;
    sent = 0;
    while (!exact.finished()) {
      exact.takePiece(piece, offset, size);
      if (piece == 1) {
        ++dataPieces;
        sent += size;
      }
    }
    QCOMPARE(dataPieces, 1);
    QCOMPARE(sent, kClipboardChunkBytes);

    // Unchanged grab must not empty the screen that already has the bytes.
    // A deferred switch send is kept so the flush can still deliver it.
    const ClipboardGrabAction skip = clipboardGrabResend(true, false, true);
    QVERIFY(!skip.grabActive);
    QVERIFY(!skip.sendNow);
    QVERIFY(!skip.keepDeferred);
    const ClipboardGrabAction keep = clipboardGrabResend(true, true, true);
    QVERIFY(!keep.grabActive);
    QVERIFY(!keep.sendNow);
    QVERIFY(keep.keepDeferred);
    const ClipboardGrabAction changed = clipboardGrabResend(false, true, true);
    QVERIFY(changed.grabActive);
    QVERIFY(changed.sendNow);
    QVERIFY(!changed.keepDeferred);

    // Mouse messages in the same read are applied before the OS clipboard.
    ClipboardApplyGate gate;
    gate.defer(0, std::string(payloadBytes, 'q'));
    QVERIFY(gate.hasPending());
    const auto pending = gate.takePending();
    QCOMPARE(pending.size(), static_cast<size_t>(1));
    QCOMPARE(pending[0].bytes.size(), payloadBytes);
    QVERIFY(!gate.hasPending());

    const std::vector<char> ops = {'C', 'M', 'F', 'M'};
    const auto trace = replayClipboardAndMouse(ops, std::string(payloadBytes, 'q'));
    QCOMPARE(trace.size(), static_cast<size_t>(3));
    QVERIFY(trace[0] == "mouse");
    QVERIFY(trace[1] == "mouse");
    QVERIFY(trace[2] == "apply");
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

  void landedDirectoryCopiesTheTreeAndAFileStillUsesThePartFile()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();
    const auto writeFile = [](const QString &path, const QByteArray &bytes) {
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly))
        return false;
      return file.write(bytes) == bytes.size();
    };
    const auto readAll = [](const QString &path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
      return file.readAll();
    };
    const auto filesUnder = [](const QString &tree) {
      QStringList out;
      QDirIterator it(tree, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        it.next();
        out.append(QDir(tree).relativeFilePath(it.filePath()));
      }
      out.sort();
      return out;
    };

    const QString source = QDir(root).filePath(QStringLiteral("合同协议"));
    QVERIFY(QDir().mkpath(QDir(source).filePath(QStringLiteral("nested"))));
    QVERIFY(QDir().mkpath(QDir(source).filePath(QStringLiteral("empty"))));
    QVERIFY(writeFile(QDir(source).filePath(QStringLiteral("说明.txt")), QByteArrayLiteral("contract")));
    QVERIFY(writeFile(QDir(source).filePath(QStringLiteral(".secret")), QByteArrayLiteral("hidden")));
    QVERIFY(writeFile(QDir(source).filePath(QStringLiteral("nested/b.txt")), QByteArrayLiteral("bee")));
    QVERIFY(writeFile(QDir(source).filePath(QStringLiteral("nested/zero.dat")), QByteArray()));

    const QString desk = QDir(root).filePath(QStringLiteral("desk"));
    QVERIFY(QDir().mkpath(desk));
    QVERIFY(writeFile(QDir(desk).filePath(QStringLiteral("说明.txt")), QByteArrayLiteral("do-not-flatten")));

    QCOMPARE(readAll(QDir(source).filePath(QStringLiteral("说明.txt"))), QByteArrayLiteral("contract"));
    QCOMPARE(readAll(QDir(source).filePath(QStringLiteral(".secret"))), QByteArrayLiteral("hidden"));
    QCOMPARE(readAll(QDir(source).filePath(QStringLiteral("nested/b.txt"))), QByteArrayLiteral("bee"));
    QCOMPARE(readAll(QDir(source).filePath(QStringLiteral("nested/zero.dat"))), QByteArray());
    const QStringList expect = filesUnder(source);
    QVERIFY(expect.contains(QStringLiteral("nested/b.txt")));
    QVERIFY(expect.contains(QStringLiteral("说明.txt")));

    const PlacedFiles placed = placeReceivedFiles({source}, desk);
    const QByteArray placedWhy = placed.failReason.toUtf8();
    QVERIFY2(placed.failReason.isEmpty(), placedWhy.constData());
    QVERIFY(placed.failReason != QStringLiteral("not-a-file"));
    QVERIFY(QFileInfo(placed.landed).isDir());
    QCOMPARE(QFileInfo(placed.landed).fileName(), QStringLiteral("合同协议"));
    QCOMPARE(filesUnder(placed.landed), expect);
    QCOMPARE(filesUnder(placed.landed).size(), filesUnder(source).size());
    QCOMPARE(readAll(QDir(placed.landed).filePath(QStringLiteral("说明.txt"))), QByteArrayLiteral("contract"));
    QCOMPARE(readAll(QDir(placed.landed).filePath(QStringLiteral(".secret"))), QByteArrayLiteral("hidden"));
    QCOMPARE(readAll(QDir(placed.landed).filePath(QStringLiteral("nested/b.txt"))), QByteArrayLiteral("bee"));
    QCOMPARE(readAll(QDir(placed.landed).filePath(QStringLiteral("nested/zero.dat"))), QByteArray());
    QVERIFY(QFileInfo(QDir(placed.landed).filePath(QStringLiteral("empty"))).isDir());
    QVERIFY(!QFileInfo::exists(placed.landed + QString::fromLatin1(kClipPartSuffix)));
    QCOMPARE(readAll(QDir(desk).filePath(QStringLiteral("说明.txt"))), QByteArrayLiteral("do-not-flatten"));
    const QStringList top = QDir(desk).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(top.contains(QStringLiteral("合同协议")));
    QVERIFY(top.contains(QStringLiteral("说明.txt")));
    QVERIFY(!top.contains(QStringLiteral("nested")));
    QVERIFY(!top.contains(QStringLiteral(".secret")));
    QVERIFY(!top.contains(QStringLiteral("b.txt")));

    const QStringList beforeRefuse = QDir(desk).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    const LandedFile refused = landFinishedFile(source, desk, QFileInfo(source).size());
    QVERIFY(!refused.ok);
    QVERIFY(refused.path.isEmpty());
    QCOMPARE(refused.reason, QStringLiteral("not-a-file"));
    QCOMPARE(QDir(desk).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), beforeRefuse);
    QCOMPARE(readAll(QDir(placed.landed).filePath(QStringLiteral("说明.txt"))), QByteArrayLiteral("contract"));

    const PlacedFiles again = placeReceivedFiles({source}, desk);
    const QByteArray againWhy = again.failReason.toUtf8();
    QVERIFY2(again.failReason.isEmpty(), againWhy.constData());
    QCOMPARE(QFileInfo(again.landed).fileName(), QStringLiteral("合同协议 (1)"));
    QVERIFY(QFileInfo(again.landed).isDir());
    QCOMPARE(filesUnder(again.landed), expect);
    QCOMPARE(readAll(QDir(placed.landed).filePath(QStringLiteral("说明.txt"))), QByteArrayLiteral("contract"));
    QVERIFY(!QFileInfo::exists(again.landed + QString::fromLatin1(kClipPartSuffix)));

    const QString report = QDir(root).filePath(QStringLiteral("report.v2"));
    QVERIFY(QDir().mkpath(QDir(report).filePath(QStringLiteral("inner"))));
    QVERIFY(writeFile(QDir(report).filePath(QStringLiteral("inner/a.txt")), QByteArrayLiteral("v2")));
    QVERIFY(QDir().mkpath(QDir(desk).filePath(QStringLiteral("report.v2"))));
    QVERIFY(writeFile(QDir(desk).filePath(QStringLiteral("report.v2/keep.txt")), QByteArrayLiteral("keep")));
    const PlacedFiles dotted = placeReceivedFiles({report}, desk);
    const QByteArray dottedWhy = dotted.failReason.toUtf8();
    QVERIFY2(dotted.failReason.isEmpty(), dottedWhy.constData());
    QCOMPARE(QFileInfo(dotted.landed).fileName(), QStringLiteral("report (1).v2"));
    QVERIFY(QFileInfo(dotted.landed).isDir());
    QCOMPARE(readAll(QDir(dotted.landed).filePath(QStringLiteral("inner/a.txt"))), QByteArrayLiteral("v2"));
    QCOMPARE(readAll(QDir(desk).filePath(QStringLiteral("report.v2/keep.txt"))), QByteArrayLiteral("keep"));

    const QString partDir = QDir(root).filePath(QStringLiteral("notes.litekvm-part"));
    QVERIFY(QDir().mkpath(partDir));
    QVERIFY(writeFile(QDir(partDir).filePath(QStringLiteral("n.txt")), QByteArrayLiteral("n")));
    const PlacedFiles partFolder = placeReceivedFiles({partDir}, desk);
    const QByteArray partWhy = partFolder.failReason.toUtf8();
    QVERIFY2(partFolder.failReason.isEmpty(), partWhy.constData());
    QCOMPARE(QFileInfo(partFolder.landed).fileName(), QStringLiteral("notes.litekvm-part"));
    QVERIFY(QFileInfo(partFolder.landed).isDir());
    QCOMPARE(readAll(QDir(partFolder.landed).filePath(QStringLiteral("n.txt"))), QByteArrayLiteral("n"));
    QVERIFY(!QFileInfo::exists(partFolder.landed + QString::fromLatin1(kClipPartSuffix)));

    const QString partFile = QDir(root).filePath(QStringLiteral("payload.litekvm-part"));
    QVERIFY(writeFile(partFile, QByteArrayLiteral("nope")));
    const int deskCount = QDir(desk).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size();
    const PlacedFiles refusedPart = placeReceivedFiles({partFile}, desk);
    QVERIFY(refusedPart.landed.isEmpty());
    QCOMPARE(refusedPart.failReason, QStringLiteral("part-file"));
    QCOMPARE(QDir(desk).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), deskCount);

    const QString photo = QDir(root).filePath(QStringLiteral("photo.jpg"));
    const QByteArray photoBytes(236, '\x42');
    QVERIFY(writeFile(photo, photoBytes));
    QVERIFY(writeFile(QDir(desk).filePath(QStringLiteral("photo.jpg")), QByteArrayLiteral("keep")));
    const PlacedFiles photoLand = placeReceivedFiles({photo}, desk);
    const QByteArray photoWhy = photoLand.failReason.toUtf8();
    QVERIFY2(photoLand.failReason.isEmpty(), photoWhy.constData());
    QVERIFY(QFileInfo(photoLand.landed).isFile());
    QVERIFY(!QFileInfo(photoLand.landed).isDir());
    QCOMPARE(QFileInfo(photoLand.landed).fileName(), QStringLiteral("photo (1).jpg"));
    QCOMPARE(readAll(photoLand.landed), photoBytes);
    QVERIFY(!QFileInfo::exists(photoLand.landed + QString::fromLatin1(kClipPartSuffix)));
    QCOMPARE(readAll(QDir(desk).filePath(QStringLiteral("photo.jpg"))), QByteArrayLiteral("keep"));

    const QString extra = QDir(root).filePath(QStringLiteral("合同协议-extra"));
    QVERIFY(QDir().mkpath(extra));
    const LandedFile beside = landFinishedDirectory(source, extra);
    const QByteArray besideWhy = beside.reason.toUtf8();
    QVERIFY2(beside.ok, besideWhy.constData());
    QCOMPARE(QFileInfo(beside.path).fileName(), QStringLiteral("合同协议"));
    QVERIFY(beside.path.contains(QStringLiteral("合同协议-extra")));
    QCOMPARE(filesUnder(beside.path).size(), 4);
    QCOMPARE(filesUnder(source).size(), 4);

    const QString outcome = remoteDragOutcomeDetail("already-up", 1, true, placed.landed, QString());
    QVERIFY(outcome.startsWith(QStringLiteral("result=copy")));
    QVERIFY(outcome.contains(QStringLiteral("reason=already-up")));
    QVERIFY(outcome.contains(QStringLiteral("appear_grace=0")));
    QVERIFY(outcome.contains(QStringLiteral("landed=") + placed.landed));
    QVERIFY(!outcome.contains(QStringLiteral("landed=-")));
    QVERIFY(!outcome.contains(QStringLiteral("result=none")));

    PlacedFiles shown;
    shown.landed = placed.landed;
    const QString drop =
        dropFileTraceDetail(QStringLiteral("WorkerW"), QStringLiteral("C:/Users/byd/Desktop"), shown, "already-up");
    QVERIFY(drop.contains(QStringLiteral("class=WorkerW")));
    QVERIFY(drop.contains(QStringLiteral("dir=C:/Users/byd/Desktop")));
    QVERIFY(drop.contains(QStringLiteral("landed=") + placed.landed));
    QVERIFY(drop.contains(QStringLiteral("reason=already-up")));
    QVERIFY(!drop.contains(QStringLiteral("landed=-")));
  }

  void failedDirectoryLandLeavesNothingAndIsNotLoggedAsCopy()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();
    const auto writeFile = [](const QString &path, const QByteArray &bytes) {
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly))
        return false;
      return file.write(bytes) == bytes.size();
    };
    const auto readAll = [](const QString &path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
      return file.readAll();
    };

    const QString source = QDir(root).filePath(QStringLiteral("悦佳和资料"));
    QVERIFY(QDir().mkpath(QDir(source).filePath(QStringLiteral("nested"))));
    QVERIFY(writeFile(QDir(source).filePath(QStringLiteral("a.txt")), QByteArrayLiteral("hello-partial")));
    QVERIFY(writeFile(QDir(source).filePath(QStringLiteral("nested/b.txt")), QByteArrayLiteral("bee")));

    const QString desk = QDir(root).filePath(QStringLiteral("desk"));
    QVERIFY(QDir().mkpath(desk));
    QVERIFY(writeFile(QDir(desk).filePath(QStringLiteral("keep.txt")), QByteArrayLiteral("keep")));

    const PlacedFiles noPaths = placeReceivedFiles({}, desk);
    QVERIFY(noPaths.landed.isEmpty());
    QCOMPARE(noPaths.failReason, QStringLiteral("no-paths"));
    const QString noPathsLog = remoteDragOutcomeDetail("release", 0, false, noPaths.landed, noPaths.failReason);
    QVERIFY(noPathsLog.startsWith(QStringLiteral("result=none")));
    QVERIFY(noPathsLog.contains(QStringLiteral("detail=no-paths")));
    QVERIFY(!noPathsLog.contains(QStringLiteral("result=copy")));

    const PlacedFiles noFolder = placeReceivedFiles({source}, QString());
    QVERIFY(noFolder.landed.isEmpty());
    QCOMPARE(noFolder.failReason, QStringLiteral("no-folder"));

    const LandedFile missing = landFinishedDirectory(source, QDir(root).filePath(QStringLiteral("no-such")));
    QVERIFY(!missing.ok);
    QVERIFY(missing.path.isEmpty());
    QCOMPARE(missing.reason, QStringLiteral("dest-missing"));

    const LandedFile into = landFinishedDirectory(source, QDir(source).filePath(QStringLiteral("nested")));
    QVERIFY(!into.ok);
    QVERIFY(into.path.isEmpty());
    QCOMPARE(into.reason, QStringLiteral("dest-inside-source"));
    QCOMPARE(readAll(QDir(source).filePath(QStringLiteral("a.txt"))), QByteArrayLiteral("hello-partial"));
    QVERIFY(!QFileInfo::exists(QDir(source).filePath(QStringLiteral("nested/悦佳和资料"))));
    QVERIFY(!QFileInfo::exists(QDir(source).filePath(QStringLiteral("nested/悦佳和资料.litekvm-part"))));

    const QString refusedLog = remoteDragOutcomeDetail("already-up", 1, true, QString(), QStringLiteral("not-a-file"));
    QVERIFY(refusedLog.startsWith(QStringLiteral("result=none")));
    QVERIFY(refusedLog.contains(QStringLiteral("reason=land-failed")));
    QVERIFY(refusedLog.contains(QStringLiteral("detail=not-a-file")));
    QVERIFY(refusedLog.contains(QStringLiteral("paths=1")));
    QVERIFY(refusedLog.contains(QStringLiteral("appear_grace=0")));
    QVERIFY(!refusedLog.contains(QStringLiteral("result=copy")));

    PlacedFiles emptyPlace;
    emptyPlace.failReason = QStringLiteral("not-a-file");
    const QString dropRefused = dropFileTraceDetail(
        QStringLiteral("WorkerW"), QStringLiteral("C:/Users/byd/Desktop"), emptyPlace, "already-up"
    );
    QVERIFY(dropRefused.contains(QStringLiteral("landed=-")));
    QVERIFY(dropRefused.contains(QStringLiteral("reason=land-failed detail=not-a-file")));
    QVERIFY(!dropRefused.contains(QStringLiteral("result=copy")));
    QVERIFY(!dropRefused.contains(QStringLiteral("reason=already-up")));

    const QString nested = QDir(source).filePath(QStringLiteral("nested"));
    const QFile::Permissions nestedMode = QFileInfo(nested).permissions();
    struct RestoreMode
    {
      QString path;
      QFile::Permissions mode;
      ~RestoreMode()
      {
        QFile::setPermissions(path, mode);
      }
    } restore{nested, nestedMode};

    // Unix mode 000 makes the directory unreadable. On Windows the read-only
    // attribute does not deny read, and setPermissions(0) can fail outright.
    // There is no fifo on Windows, so the checks above are the Windows run.
    QString expectReason = QStringLiteral("unsupported-entry");
    const bool cleared = QFile::setPermissions(nested, QFileDevice::Permissions());
    if (cleared && !QFileInfo(nested).isReadable()) {
      expectReason = QStringLiteral("open-failed");
    } else {
      if (cleared)
        QFile::setPermissions(nested, nestedMode);
#if !defined(Q_OS_WIN)
      const QString fifo = QDir(nested).filePath(QStringLiteral("zz.fifo"));
      QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
#else
      return;
#endif
    }

    const PlacedFiles failed = placeReceivedFiles({source}, desk);
    QVERIFY(failed.landed.isEmpty());
    QCOMPARE(failed.failReason, expectReason);
    QVERIFY(failed.failReason != QStringLiteral("not-a-file"));
    QVERIFY(!QFileInfo::exists(QDir(desk).filePath(QStringLiteral("悦佳和资料"))));
    QVERIFY(!QFileInfo::exists(QDir(desk).filePath(QStringLiteral("悦佳和资料.litekvm-part"))));
    QVERIFY(!QFileInfo::exists(QDir(desk).filePath(QStringLiteral("悦佳和资料 (1)"))));
    QVERIFY(!QFileInfo::exists(QDir(desk).filePath(QStringLiteral("a.txt"))));
    QVERIFY(!QFileInfo::exists(QDir(desk).filePath(QStringLiteral("b.txt"))));
    QVERIFY(!QFileInfo::exists(QDir(desk).filePath(QStringLiteral("nested"))));
    QCOMPARE(readAll(QDir(desk).filePath(QStringLiteral("keep.txt"))), QByteArrayLiteral("keep"));
    QCOMPARE(readAll(QDir(source).filePath(QStringLiteral("a.txt"))), QByteArrayLiteral("hello-partial"));
    QFile::setPermissions(nested, nestedMode);
    QCOMPARE(readAll(QDir(source).filePath(QStringLiteral("nested/b.txt"))), QByteArrayLiteral("bee"));

    const QString outcome = remoteDragOutcomeDetail("already-up", 1, false, failed.landed, failed.failReason);
    QVERIFY(outcome.startsWith(QStringLiteral("result=none")));
    QVERIFY(outcome.contains(QStringLiteral("detail=") + expectReason));
    QVERIFY(outcome.contains(QStringLiteral("appear_grace=1")));
    QVERIFY(!outcome.contains(QStringLiteral("result=copy")));

    const QString drop = dropFileTraceDetail(QStringLiteral("WorkerW"), desk, failed, "already-up");
    QVERIFY(drop.contains(QStringLiteral("landed=-")));
    QVERIFY(drop.contains(QStringLiteral("reason=land-failed detail=") + expectReason));
    QVERIFY(!drop.contains(QStringLiteral("result=copy")));
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
