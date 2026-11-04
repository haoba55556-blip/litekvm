/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

/**
 * Socket policy for the keyboard/mouse stream.
 *
 * Mouse motion is a stream of tiny messages. Two TCP behaviours turn that
 * into visible stutter:
 *
 *  - Nagle (TCP_NODELAY off) holds a short write hoping to merge it with
 *    the next one. We always disable it.
 *  - Delayed ACK. On Linux TCP_QUICKACK is cleared by the kernel after each
 *    receive, so it has to be set again after every successful read.
 *    On Windows the sticky control is SIO_TCP_SET_ACK_FREQUENCY; 1 means
 *    ACK every segment (delayed ACK off).
 */
struct LowLatencySocketPolicy
{
  static constexpr bool kTcpNoDelay = true;
  static constexpr bool kRefreshQuickAckAfterRead = true;
  /// Windows SIO_TCP_SET_ACK_FREQUENCY. 1 disables delayed ACK.
  static constexpr int kWindowsAckFrequency = 1;
};
