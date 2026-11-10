// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// When an OLE/Qt drag enters the screen edge, start the transfer immediately.
//
// The pointer crosses into the other computer in well under 150 ms. A dwell
// timer is cancelled by dragLeave as soon as the screen switch warps the
// cursor, so the files never leave. Leaving the strip must not undo an enter
// that already started a transfer.
//
// The same gesture must not be offered twice. A bounce back onto the strip
// used to pass the old 1.5 s debounce and land the file again. The latch
// stays until the left button is released.
#pragma once

#include <QStringList>

namespace litekvm {

class DragEdgeGate
{
public:
  /// @return true when this enter should start a file transfer now.
  bool onEnter(const QStringList &paths, qint64 nowMs)
  {
    if (paths.isEmpty())
      return false;
    if (m_latched && paths == m_lastPaths)
      return false;
    m_lastPaths = paths;
    m_lastMs = nowMs;
    m_started = true;
    m_latched = true;
    return true;
  }

  /// The cursor left the strip (usually because the screen switched).
  /// Does not revoke a transfer onEnter() already accepted.
  void onLeave()
  {
  }

  /// The left button came up, so the next drag of the same files may start.
  void onButtonReleased()
  {
    m_latched = false;
  }

  /// The enter was accepted but the send did not start (busy session, no
  /// peer). A later DragMove of the same gesture must be allowed to try again.
  void abandon()
  {
    m_latched = false;
  }

  bool transferStarted() const
  {
    return m_started;
  }

  bool latched() const
  {
    return m_latched;
  }

private:
  QStringList m_lastPaths;
  qint64 m_lastMs = -100000;
  bool m_started = false;
  bool m_latched = false;
};

/// Edge strips are hit-testable only during a drag that has already moved.
/// A click (button down and up in place) must not reveal them, or the top
/// strip covers browser chrome and steals the click.
class EdgeStripArm
{
public:
  static constexpr int kThresholdPx = 8;
  static constexpr int kNearEdgePx = 128;
  // Used only when nobody saw the original button-down (no low-level hook).
  // A click is shorter than this; a drag held on the edge is not.
  static constexpr qint64 kHoldMs = 180;

  void setButtonDown(bool down, int x, int y, qint64 nowMs)
  {
    if (down && !m_down) {
      m_down = true;
      m_downMs = nowMs;
      // The previous poll is the button-down origin when this sample is the
      // first one that sees the button. A flick that is already at the edge
      // still counts as a drag, without a low-level hook on the GUI thread
      // (that hook stalled every injected move on the other computer).
      if (m_hasLast) {
        m_originX = m_lastX;
        m_originY = m_lastY;
      } else {
        m_originX = x;
        m_originY = y;
      }
      m_moved = false;
    } else if (!down) {
      m_down = false;
      m_moved = false;
    }
    if (m_down && !m_moved) {
      const qint64 dx = qint64(x) - m_originX;
      const qint64 dy = qint64(y) - m_originY;
      if (dx * dx + dy * dy >= qint64(kThresholdPx) * kThresholdPx)
        m_moved = true;
    }
    m_lastX = x;
    m_lastY = y;
    m_hasLast = true;
  }

  /// Trust a hook that observed the real button-down, not the first poll.
  void observe(bool down, bool moved, qint64 nowMs)
  {
    if (!down) {
      m_down = false;
      m_moved = false;
      return;
    }
    if (!m_down)
      m_downMs = nowMs;
    m_down = true;
    if (moved)
      m_moved = true;
  }

  bool buttonDown() const
  {
    return m_down;
  }

  bool moved() const
  {
    return m_moved;
  }

  bool shouldShow(bool nearEdge, qint64 nowMs, bool allowHold) const
  {
    if (!m_down || !nearEdge)
      return false;
    if (m_moved)
      return true;
    return allowHold && nowMs - m_downMs >= kHoldMs;
  }

  /// @p x and @p y are in the same coordinate space as the screen rectangle.
  static bool nearEdge(int x, int y, int left, int top, int width, int height, int margin)
  {
    if (width < 1 || height < 1 || margin < 1)
      return false;
    const int right = left + width;
    const int bottom = top + height;
    return x < left + margin || y < top + margin || x >= right - margin || y >= bottom - margin;
  }

private:
  bool m_down = false;
  bool m_moved = false;
  bool m_hasLast = false;
  int m_originX = 0;
  int m_originY = 0;
  int m_lastX = 0;
  int m_lastY = 0;
  qint64 m_downMs = 0;
};

} // namespace litekvm
