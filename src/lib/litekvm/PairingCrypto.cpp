// SPDX-License-Identifier: GPL-2.0
// LiteKVM pairing code derivation — both sides independently compute the same
// 6-digit PIN from the exchanged nonces + both fingerprints.
//
// Spec: docs/discovery-pairing-spec.md §4 (litekvm-design repo)
#include "PairingCrypto.h"

#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace litekvm {

namespace {

// HKDF-Extract + Expand (RFC 5869) with SHA-256
QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len)
{
  // extract
  uint8_t prk[EVP_MAX_MD_SIZE];
  unsigned int prkLen = 0;
  HMAC(EVP_sha256(), salt.constData(), salt.size(), reinterpret_cast<const uint8_t *>(ikm.constData()),
       ikm.size(), prk, &prkLen);

  // expand
  QByteArray okm;
  okm.reserve(len);
  uint8_t t[EVP_MAX_MD_SIZE];
  unsigned int tLen = 0;
  QByteArray counter;
  int offset = 0;
  uint8_t iteration = 1;
  while (offset < len) {
    QByteArray input(reinterpret_cast<const char *>(t), tLen);
    input.append(info);
    input.append(char(iteration));
    HMAC(EVP_sha256(), prk, prkLen, reinterpret_cast<const uint8_t *>(input.constData()), input.size(), t,
         &tLen);
    const int take = qMin(int(tLen), len - offset);
    okm.append(reinterpret_cast<const char *>(t), take);
    offset += take;
    ++iteration;
  }
  return okm;
}

} // namespace

QByteArray randomNonce(int bytes)
{
  QByteArray out(bytes, Qt::Uninitialized);
  RAND_bytes(reinterpret_cast<uint8_t *>(out.data()), bytes);
  return out;
}

QString derivePairCode(const QByteArray &nonceA, const QByteArray &nonceB, const QString &fpA,
                       const QString &fpB)
{
  QByteArray ikm;
  ikm.append(nonceA);
  ikm.append(nonceB);
  ikm.append(QByteArray::fromHex(fpA.toUtf8()));
  ikm.append(QByteArray::fromHex(fpB.toUtf8()));

  const QByteArray okm = hkdfSha256(ikm, "litekvm-pair-v1", "PAIR_CODE", 4);
  if (okm.size() < 4)
    return {};

  const uint32_t v = (uint32_t(uint8_t(okm[0])) << 24) | (uint32_t(uint8_t(okm[1])) << 16) |
                     (uint32_t(uint8_t(okm[2])) << 8) | uint32_t(uint8_t(okm[3]));
  return QStringLiteral("%1").arg(v % 1000000, 6, 10, QLatin1Char('0'));
}

QByteArray hmacPairVerify(const QByteArray &code, const QByteArray &nonceA, const QByteArray &nonceB)
{
  const QByteArray key = code; // ASCII digits, already UTF-8 clean
  QByteArray msg;
  msg.append(nonceA).append(nonceB);

  uint8_t mac[EVP_MAX_MD_SIZE];
  unsigned int macLen = 0;
  HMAC(EVP_sha256(), key.constData(), key.size(), reinterpret_cast<const uint8_t *>(msg.constData()),
       msg.size(), mac, &macLen);
  return QByteArray(reinterpret_cast<const char *>(mac), int(macLen));
}

bool verifyPairHmac(const QByteArray &expectedMac, const QByteArray &code, const QByteArray &nonceA,
                    const QByteArray &nonceB)
{
  const QByteArray actual = hmacPairVerify(code, nonceA, nonceB);
  return expectedMac.size() == actual.size() &&
         CRYPTO_memcmp(actual.constData(), expectedMac.constData(), actual.size()) == 0;
}

} // namespace litekvm
