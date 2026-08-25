// SPDX-License-Identifier: GPL-2.0
// LiteKVM trust store — persisted list of paired devices.
//
// Spec: docs/discovery-pairing-spec.md §6 (litekvm-design repo)
#include "TrustStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>

namespace litekvm {

namespace {

QString defaultPath()
{
#if defined(Q_OS_WIN)
  const QString base = qEnvironmentVariable("APPDATA");
#else
  const QString base = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
#endif
  return QString("%1/LiteKVM/trust.json").arg(base);
}

QJsonObject entryToJson(const TrustEntry &e)
{
  QJsonObject o;
  o.insert("device_id", e.deviceId);
  o.insert("pubkey", QString::fromLatin1(e.pubkey.toBase64()));
  o.insert("name", e.name);
  o.insert("platform", e.platform);
  o.insert("paired_at", e.pairedAt.toString(Qt::ISODate));
  o.insert("last_addr", e.lastAddr);
  o.insert("last_seen", e.lastSeen.toString(Qt::ISODate));
  return o;
}

std::optional<TrustEntry> entryFromJson(const QJsonObject &o)
{
  TrustEntry e;
  e.deviceId = o.value("device_id").toString();
  e.pubkey = QByteArray::fromBase64(o.value("pubkey").toString().toUtf8());
  e.name = o.value("name").toString();
  e.platform = o.value("platform").toString();
  e.pairedAt = QDateTime::fromString(o.value("paired_at").toString(), Qt::ISODate);
  e.lastAddr = o.value("last_addr").toString();
  e.lastSeen = QDateTime::fromString(o.value("last_seen").toString(), Qt::ISODate);
  if (e.deviceId.isEmpty() || e.pubkey.isEmpty())
    return std::nullopt;
  return e;
}

} // namespace

TrustStore::TrustStore(const QString &filePath) : m_path(filePath.isEmpty() ? defaultPath() : filePath)
{
  load();
}

bool TrustStore::load()
{
  m_entries.clear();
  QFile f(m_path);
  if (!f.exists() || !f.open(QIODevice::ReadOnly))
    return false;

  const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object().value("devices").toArray();
  for (const auto &v : arr) {
    if (auto e = entryFromJson(v.toObject()))
      m_entries.push_back(*e);
  }
  return true;
}

bool TrustStore::save() const
{
  QJsonArray arr;
  for (const auto &e : m_entries)
    arr.append(entryToJson(e));

  QJsonObject root;
  root.insert("version", 1);
  root.insert("devices", arr);

  QDir().mkpath(QFileInfo(m_path).absolutePath());
  QFile f(m_path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  return true;
}

bool TrustStore::contains(const QString &deviceId) const
{
  return find(deviceId) != nullptr;
}

const TrustEntry *TrustStore::find(const QString &deviceId) const
{
  for (const auto &e : m_entries)
    if (e.deviceId == deviceId)
      return &e;
  return nullptr;
}

void TrustStore::upsert(const TrustEntry &entry)
{
  for (auto &e : m_entries) {
    if (e.deviceId == entry.deviceId) {
      e = entry;
      save();
      return;
    }
  }
  m_entries.push_back(entry);
  save();
}

bool TrustStore::remove(const QString &deviceId)
{
  const auto before = m_entries.size();
  m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                 [&](const TrustEntry &e) { return e.deviceId == deviceId; }),
                  m_entries.end());
  if (m_entries.size() != before) {
    save();
    return true;
  }
  return false;
}

const std::vector<TrustEntry> &TrustStore::entries() const
{
  return m_entries;
}

} // namespace litekvm
