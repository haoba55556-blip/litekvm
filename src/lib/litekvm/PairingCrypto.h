// SPDX-License-Identifier: GPL-2.0
// LiteKVM pairing crypto — nonce generation, PAIR_CODE derivation, HMAC verify.
//
// Spec: docs/discovery-pairing-spec.md §4 (litekvm-design repo)
#pragma once

#include <QByteArray>
#include <QString>

namespace litekvm {

/// CSPRNG nonce (32 bytes for pairing per spec)
QByteArray randomNonce(int bytes = 32);

/**
 * @brief Both peers independently compute the same 6-digit code.
 * PAIR_CODE = HKDF-SHA256(nonceA || nonceB || fpA || fpB,
 *                         salt="litekvm-pair-v1", info="PAIR_CODE")[0..4] % 1e6
 */
QString derivePairCode(const QByteArray &nonceA, const QByteArray &nonceB, const QString &fpA,
                       const QString &fpB);

/// HMAC-SHA256(code, nonceA || nonceB) — cross-verification token
QByteArray hmacPairVerify(const QByteArray &code, const QByteArray &nonceA, const QByteArray &nonceB);

/// constant-time comparison of the received MAC against expectation
bool verifyPairHmac(const QByteArray &expectedMac, const QByteArray &code, const QByteArray &nonceA,
                    const QByteArray &nonceB);

} // namespace litekvm
