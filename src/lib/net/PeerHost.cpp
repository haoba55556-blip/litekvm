/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/PeerHost.h"

#include "io/StreamFilter.h"
#include "net/TCPSocket.h"

std::string peerHostOf(deskflow::IStream *stream)
{
  while (auto *filter = dynamic_cast<StreamFilter *>(stream))
    stream = filter->getStream();
  if (auto *socket = dynamic_cast<TCPSocket *>(stream))
    return socket->peerHost();
  return {};
}
