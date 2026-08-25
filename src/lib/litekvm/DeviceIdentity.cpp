// SPDX-License-Identifier: GPL-2.0
// LiteKVM device identity — Ed25519 keypair, persistent device_id, fingerprint.
//
// Spec: docs/discovery-pairing-spec.md §2 (litekvm-design repo)
#include "DeviceIdentity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QtGlobal>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>

namespace litekvm {

namespace {

QByteArray signKeypath()
{
  // Windows: %APPDATA%/LiteKVM; others: ~/.config/LiteKVM (Qt default org/app)
#if defined(Q_OS_WIN)
  const QString base = qEnvironmentVariable("APPDATA");
  return QString("%1/LiteKVM/identity.json").arg(base).toUtf8();
#else
  const QString base = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
  return QString("%1/LiteKVM/identity.json").arg(base).toUtf8();
#endif
}

QByteArray b64(const uint8_t *data, size_t len)
{
  return QByteArray(reinterpret_cast<const char *>(data), int(len)).toBase64();
}

bool unB64(const QString &in, uint8_t *out, size_t expect)
{
  const QByteArray raw = QByteArray::fromBase64(in.toUtf8());
  if (raw.size() != int(expect))
    return false;
  memcpy(out, raw.constData(), expect);
  return true;
}

struct KeyPair {
  std::array<uint8_t, 32> pub{};
  std::array<uint8_t, 64> priv{}; // seed(32) + pubkey(32) as in RFC8032 private form used by OpenSSL
};

bool generateKeyPair(KeyPair &kp)
{
  EVP_PKEY *pkey = nullptr;
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  if (!ctx)
    return false;
  bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_generate(ctx, &pkey) == 1;
  EVP_PKEY_CTX_free(ctx);
  if (!ok || !pkey)
    return false;

  size_t pubLen = kp.pub.size();
  size_t privLen = kp.priv.size();
  ok = EVP_PKEY_get_raw_public_key(pkey, kp.pub.data(), &pubLen) == 1 && pubLen == kp.pub.size() &&
       EVP_PKEY_get_raw_private_key(pkey, kp.priv.data(), &privLen) == 1 && privLen == 32 /*seed*/;
  // NOTE: raw private key of ED25519 in OpenSSL is the 32-byte seed; we store
  // it in the first half and leave the second half zero-filled (unused).
  EVP_PKEY_free(pkey);
  return ok;
}

} // namespace

QString DeviceIdentity::deviceIdFromPubkey(const uint8_t *pub, size_t len)
{
  uint8_t hash[SHA256_DIGEST_LENGTH];
  SHA256(pub, len, hash);
  QByteArray hex = QByteArray(reinterpret_cast<char *>(hash), 16).toHex();
  return QString::fromLatin1(hex);
}

QString DeviceIdentity::fingerprintFromPubkey(const uint8_t *pub, size_t len)
{
  uint8_t hash[SHA256_DIGEST_LENGTH];
  SHA256(pub, len, hash);
  return QString::fromLatin1(QByteArray(reinterpret_cast<char *>(hash), 8).toHex());
}

std::optional<DeviceIdentity> DeviceIdentity::loadOrCreate(const QString &storageDirOverride)
{
  const QString dir = storageDirOverride.isEmpty() ? QString::fromUtf8(signKeypath()) : storageDirOverride;
  QFile file(dir);

  DeviceIdentity id;

  if (file.exists() && file.open(QIODevice::ReadOnly)) {
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const bool havePub = unB64(obj.value("pubkey").toString(), id.m_pub.data(), id.m_pub.size());
    const bool havePriv = unB64(obj.value("privkey").toString(), id.m_priv.data(), 32);
    if (havePub && havePriv) {
      // re-derive priv buffer into the 64-byte layout used at signing time
      memset(id.m_priv.data() + 32, 0, 32);
      id.m_deviceId = obj.value("device_id").toString();
      id.m_name = obj.value("name").toString();
      // integrity check: device_id must match pubkey
      if (id.m_deviceId == deviceIdFromPubkey(id.m_pub.data(), id.m_pub.size()))
        return id;
    }
  }

  // create new identity
  KeyPair kp;
  if (!generateKeyPair(kp))
    return std::nullopt;

  id.m_pub = kp.pub;
  memset(id.m_priv.data(), 0, id.m_priv.size());
  memcpy(id.m_priv.data(), kp.priv.data(), 32);
  id.m_deviceId = deviceIdFromPubkey(id.m_pub.data(), id.m_pub.size());

  if (id.m_name.isEmpty())
    id.m_name = QSysInfo::machineHostName();

  // persist
  QJsonObject obj;
  obj.insert("pubkey", QString::fromLatin1(b64(id.m_pub.data(), id.m_pub.size())));
  obj.insert("privkey", QString::fromLatin1(b64(kp.priv.data(), 32)));
  obj.insert("device_id", id.m_deviceId);
  obj.insert("name", id.m_name);

  // ensure directory exists
  const QFileInfo fi(dir);
  QDir().mkpath(fi.absolutePath());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return std::nullopt;
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
  file.close();

#ifdef Q_OS_WIN
  // best-effort DPAPI would be nicer; restrict perms via Qt is not available.
#endif
  return id;
}

const std::array<uint8_t, 32> &DeviceIdentity::publicKey() const
{
  return m_pub;
}

QString DeviceIdentity::deviceId() const
{
  return m_deviceId;
}

QString DeviceIdentity::fingerprint() const
{
  return fingerprintFromPubkey(m_pub.data(), m_pub.size());
}

QString DeviceIdentity::name() const
{
  return m_name;
}

void DeviceIdentity::setName(const QString &name)
{
  m_name = name;
}

} // namespace litekvm
