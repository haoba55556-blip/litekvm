/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>

/**
 * Collapses a burst of pointer updates into one sample.
 *
 * Absolute moves keep only the newest position (intermediate points are
 * not observable once a newer one is queued). Relative moves accumulate,
 * because each delta still has to be applied.
 *
 * The client uses this so a high-rate mouse does not turn into one
 * SetCursorPos / SendInput per packet. Callers decide when to flush
 * (a few milliseconds, or immediately on a button).
 */
/**
 * Apply every mouse sample as it is parsed.
 *
 * Waiting until the socket has nothing buffered freezes the cursor for the
 * whole burst: a continuous move keeps the next packet already readable, so
 * the pointer only catches up when the hand pauses. That is the stutter on
 * the client. Callers still keep only the newest absolute position, and there
 * is no timer — Windows' default tick is ~15.6 ms and was quantizing motion.
 */
inline bool shouldApplyMouseNow(bool moreInputAlreadyBuffered)
{
  (void)moreInputAlreadyBuffered;
  return true;
}

class MouseMotionCoalescer
{
public:
  void addAbsolute(int32_t x, int32_t y)
  {
    m_x = x;
    m_y = y;
    m_hasAbsolute = true;
  }

  void addRelative(int32_t dx, int32_t dy)
  {
    m_dx += dx;
    m_dy += dy;
    m_hasRelative = true;
  }

  bool hasAbsolute() const
  {
    return m_hasAbsolute;
  }

  bool hasRelative() const
  {
    return m_hasRelative;
  }

  bool hasPending() const
  {
    return m_hasAbsolute || m_hasRelative;
  }

  bool takeAbsolute(int32_t &x, int32_t &y)
  {
    if (!m_hasAbsolute)
      return false;
    x = m_x;
    y = m_y;
    m_hasAbsolute = false;
    return true;
  }

  bool takeRelative(int32_t &dx, int32_t &dy)
  {
    if (!m_hasRelative)
      return false;
    dx = m_dx;
    dy = m_dy;
    m_dx = 0;
    m_dy = 0;
    m_hasRelative = false;
    return true;
  }

  void clear()
  {
    m_hasAbsolute = false;
    m_hasRelative = false;
    m_dx = 0;
    m_dy = 0;
  }

private:
  int32_t m_x = 0;
  int32_t m_y = 0;
  int32_t m_dx = 0;
  int32_t m_dy = 0;
  bool m_hasAbsolute = false;
  bool m_hasRelative = false;
};
