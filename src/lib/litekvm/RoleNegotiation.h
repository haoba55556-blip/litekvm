// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#pragma once

#include "DeviceIdentity.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

namespace litekvm {

/**
 * @brief Elects the mesh server (feature 3). Rule: smallest online
 * device_id. 5-second hysteresis keeps the role stable through transient
 * disconnects; every actual change bumps an epoch and emits roleChanged.
 */
class RoleNegotiation : public QObject {
  Q_OBJECT

public:
  enum class Role { Server, Client };

  explicit RoleNegotiation(const DeviceIdentity &self, QObject *parent = nullptr);

  void setOnline(const QString &deviceId);
  void setOffline(const QString &deviceId);
  void reset(const QSet<QString> &onlineIds);

  QString currentServer() const
  {
    return m_currentServer;
  }
  Role myRole() const
  {
    return m_currentServer == m_self.deviceId() ? Role::Server : Role::Client;
  }
  quint64 epoch() const
  {
    return m_epoch;
  }

Q_SIGNALS:
  void roleChanged(const QString &serverId, quint64 epoch, litekvm::RoleNegotiation::Role myRole);

private:
  QString electServer() const;
  void reelect();

  const DeviceIdentity &m_self;
  QSet<QString> m_online;                    // online peer ids (excl. self)
  QHash<QString, qint64> m_offlineSince;     // grace-window bookkeeping
  QString m_currentServer;
  quint64 m_epoch = 0;
  QTimer m_graceTimer; // single-shot: re-elect when the hysteresis window expires
};

} // namespace litekvm
