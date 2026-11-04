// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Which address a clipboard send should dial.
//
// Discovery can be down, and trust.json keeps the address from the last time
// a file transfer (or pairing) happened to learn one. A laptop that was .23
// and is now .20 is still the machine on the other end of the live core
// session — that socket address wins.
#pragma once

#include "DiscoveryService.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QString>

namespace litekvm {

struct ClipRouteInput
{
  QString liveHost;       ///< peer address of the connected core socket
  QString liveScreenName; ///< screen name reported with that socket (may be empty)
  QString deviceId;
  QString deviceName;
  int pairedCount = 1;
  QString discoveredHost;
  QString trustedHost;
};

/// "this-device" is the identity default, and the client reports the live peer
/// as "server" when it does not know the screen name. Neither is a machine.
inline bool isPlaceholderPeerName(const QString &name)
{
  const QString trimmed = name.trimmed();
  return trimmed.isEmpty() || trimmed.compare(QLatin1String("this-device"), Qt::CaseInsensitive) == 0 ||
         trimmed.compare(QLatin1String("server"), Qt::CaseInsensitive) == 0;
}

/// Label to show and log. A live core screen name beats a placeholder stored
/// at pairing time, and never the name of this machine.
inline QString clipPeerLabel(const QString &storedName, const QString &liveScreenName, const QString &selfName)
{
  const auto usable = [&](const QString &name) {
    return !isPlaceholderPeerName(name) && (selfName.isEmpty() || name.compare(selfName, Qt::CaseInsensitive) != 0);
  };
  if (usable(liveScreenName))
    return liveScreenName.trimmed();
  if (usable(storedName))
    return storedName.trimmed();
  return storedName.trimmed();
}

/// True for loopback, multicast, or an address configured on this machine.
/// Dialing one of those is a self-loop on the file port.
inline bool hostIsThisMachine(const QString &host)
{
  const QString trimmed = host.trimmed();
  if (trimmed.isEmpty())
    return false;
  const QString normalized = DiscoveryService::normalizeHost(trimmed);
  if (normalized.isEmpty())
    return true;
  const QHostAddress want(normalized);
  if (want.isNull())
    return false;
  const auto mine = QNetworkInterface::allAddresses();
  for (const QHostAddress &addr : mine) {
    if (addr.isEqual(want, QHostAddress::TolerantConversion))
      return true;
  }
  return false;
}

inline bool liveHostApplies(const ClipRouteInput &in)
{
  const QString live = DiscoveryService::normalizeHost(in.liveHost);
  const QString usable = live.isEmpty() ? in.liveHost.trimmed() : live;
  if (usable.isEmpty())
    return false;
  if (in.pairedCount <= 1)
    return true;
  return !in.liveScreenName.isEmpty() && !in.deviceName.isEmpty() &&
         in.deviceName.compare(in.liveScreenName, Qt::CaseInsensitive) == 0;
}

inline QString clipRouteHost(const ClipRouteInput &in)
{
  const auto usable = [](const QString &host) {
    const QString normalized = DiscoveryService::normalizeHost(host);
    return normalized.isEmpty() ? host.trimmed() : normalized;
  };

  if (liveHostApplies(in))
    return usable(in.liveHost);

  const QString discovered = usable(in.discoveredHost);
  if (!discovered.isEmpty())
    return discovered;

  return usable(in.trustedHost);
}

} // namespace litekvm
