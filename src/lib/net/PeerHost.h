/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>

namespace deskflow {
class IStream;
}

/// Walk stream filters down to the TCP socket and return getpeername().
std::string peerHostOf(deskflow::IStream *stream);
