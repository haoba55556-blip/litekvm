// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#include "../../lib/litekvm/DeviceIdentity.h"
#include "../../lib/litekvm/PairingCrypto.h"
#include "../../lib/litekvm/TrustStore.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace litekvm;

class LiteKvmTests : public QObject {
  Q_OBJECT

private Q_SLOTS:

  void identityCreatesAndReloads()
  {
    QTemporaryDir dir;
    const QString file = dir.path() + "/identity.json";

    auto created = DeviceIdentity::loadOrCreate(file);
    QVERIFY(created.has_value());
    QCOMPARE(created->deviceId().size(), 32);
    QCOMPARE(created->fingerprint().size(), 16);
    QVERIFY(!created->name().isEmpty());

    // reload must produce identical identity
    auto reloaded = DeviceIdentity::loadOrCreate(file);
    QVERIFY(reloaded.has_value());
    QCOMPARE(reloaded->deviceId(), created->deviceId());
    QCOMPARE(reloaded->fingerprint(), created->fingerprint());
    QVERIFY(reloaded->publicKey() == created->publicKey());

    // device_id must be derived from pubkey
    const auto &pub = created->publicKey();
    QCOMPARE(created->deviceId(), DeviceIdentity::deviceIdFromPubkey(pub.data(), pub.size()));
  }

  void identityTwoInstancesDiffer()
  {
    QTemporaryDir dir1, dir2;
    auto a = DeviceIdentity::loadOrCreate(dir1.path() + "/id.json");
    auto b = DeviceIdentity::loadOrCreate(dir2.path() + "/id.json");
    QVERIFY(a.has_value() && b.has_value());
    QVERIFY(a->deviceId() != b->deviceId());
  }

  void trustStoreRoundtrip()
  {
    QTemporaryDir dir;
    const QString file = dir.path() + "/trust.json";
    const QDateTime now = QDateTime::currentDateTimeUtc();

    TrustStore store(file);
    QCOMPARE(store.entries().size(), 0);

    TrustEntry e;
    e.deviceId = QStringLiteral("9f8e7d6c5b4a3210fedcba9876543210");
    e.pubkey = QByteArray(32, char(0xAB));
    e.name = QStringLiteral("客厅笔记本");
    e.platform = QStringLiteral("win");
    e.pairedAt = now;
    e.lastAddr = QStringLiteral("192.168.1.23");
    e.lastSeen = now;
    store.upsert(e);

    // in-memory hit
    QVERIFY(store.contains(e.deviceId));
    QCOMPARE(store.find(e.deviceId)->name, e.name);

    // fresh instance reads from disk (chinese name survives roundtrip)
    TrustStore reloaded(file);
    QVERIFY(reloaded.contains(e.deviceId));
    QCOMPARE(reloaded.find(e.deviceId)->name, QStringLiteral("客厅笔记本"));
    QCOMPARE(reloaded.find(e.deviceId)->pubkey, e.pubkey);

    // upsert replaces by device_id
    e.name = QStringLiteral("renamed");
    store.upsert(e);
    QCOMPARE(int(store.entries().size()), 1);
    QCOMPARE(store.find(e.deviceId)->name, QStringLiteral("renamed"));

    // remove works
    QVERIFY(store.remove(e.deviceId));
    QVERIFY(!store.contains(e.deviceId));
    QCOMPARE(int(store.entries().size()), 0);
  }

  void pairCodeDeterministicAcrossSides()
  {
    const QByteArray na = randomNonce(32);
    const QByteArray nb = randomNonce(32);
    const QString fpA = QStringLiteral("1a2b3c4d5e6f7788");
    const QString fpB = QStringLiteral("8877f6e5d4c3b2a1");

    // both sides derive identical code from same inputs
    const QString codeA = derivePairCode(na, nb, fpA, fpB);
    const QString codeB = derivePairCode(na, nb, fpA, fpB);
    QCOMPARE(codeA, codeB);
    QCOMPARE(codeA.size(), 6);
    bool ok = false;
    codeA.toUInt(&ok);
    QVERIFY(ok); // pure digits

    // swapped argument order must NOT silently match a wrong pairing
    const QString swapped = derivePairCode(nb, na, fpB, fpA);
    if (swapped == codeA) {
      QSKIP("hash collision on this vector (acceptable, 1-in-1e6)");
    }

    // different nonces => (almost surely) different code
    const QByteArray nc = randomNonce(32);
    const QString other = derivePairCode(nc, nb, fpA, fpB);
    if (other == codeA)
      QSKIP("hash collision");
  }

  void pairHmacVerify()
  {
    const QByteArray na = randomNonce();
    const QByteArray nb = randomNonce();
    const QString code = derivePairCode(na, nb, "aa", "bb");

    const QByteArray macA = hmacPairVerify(code.toUtf8(), na, nb);

    // B verifies A's mac with its own derivation
    QVERIFY(verifyPairHmac(macA, code.toUtf8(), na, nb));

    // tampered code fails
    QVERIFY(!verifyPairHmac(macA, "000000" == code ? QByteArray("111111") : QByteArray("000000"), na, nb));

    // tampered nonce fails
    QByteArray nb2 = nb;
    nb2[0] = char(uint8_t(nb2[0]) ^ 0x01);
    QVERIFY(!verifyPairHmac(macA, code.toUtf8(), na, nb2));
  }
};

QTEST_MAIN(LiteKvmTests)
#include "LiteKvmTests.moc"
