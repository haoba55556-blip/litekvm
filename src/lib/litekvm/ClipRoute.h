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

#include <QString>

namespace litekvm {

struct ClipRouteInput {
  QString liveHost;        ///< peer address of the connected core socket
  QString liveScreenName;  ///< screen name reported with that socket (may be empty)
  QString deviceId;
  QString deviceName;
  int pairedCount = 1;
  QString discoveredHost;
  QString trustedHost;
};

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
