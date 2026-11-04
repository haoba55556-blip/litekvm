// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

// The Windows low-level hook eats mouse moves inside the 1px jump zone so the
// server can switch screens. Eating the move also hides it from OLE, and the
// cursor is clamped to that exact pixel while the user pushes against the
// edge, so an edge strip never receives DragEnter.
//
// A held mouse button is a drag (Explorer file drag included). Pass the move
// through. The server stays on this screen via isLockedToScreen() until the
// button is released, which is long enough for DragEnter.
inline bool eatJumpZoneMouseMove(bool insideJumpZone, bool bogusPosition, bool anyMouseButtonDown)
{
  return insideJumpZone && !bogusPosition && !anyMouseButtonDown;
}

// Hold the screen switch briefly after the cursor hits the edge with a button
// down, long enough for the edge strip to receive DragEnter, then let the
// cursor cross while the button is still held so the other machine can drop.
class DragEdgeGrace
{
public:
  static constexpr double kSeconds = 0.4;

  void reset()
  {
    m_armed = false;
  }

  bool armed() const
  {
    return m_armed;
  }

  /// First call arms and refuses. Later calls allow once @p elapsedSeconds
  /// since that arming reaches kSeconds.
  bool allowSwitch(double elapsedSeconds)
  {
    if (!m_armed) {
      m_armed = true;
      return false;
    }
    return elapsedSeconds >= kSeconds;
  }

private:
  bool m_armed = false;
};

// A drag lands on the shared edge. The next relative delta is still aimed
// off that edge (and the primary cursor warp adds a large one), so the
// pointer is thrown back. Pull the landing point inside the destination.
inline void
insetFromScreenEdge(int32_t left, int32_t top, int32_t width, int32_t height, int32_t margin, int32_t &x, int32_t &y)
{
  if (width < 1 || height < 1)
    return;
  if (margin < 1)
    margin = 1;
  if (width <= margin * 2 || height <= margin * 2)
    margin = 1;
  const int32_t right = left + width - 1;
  const int32_t bottom = top + height - 1;
  if (x <= left + 1)
    x = left + margin;
  else if (x >= right - 1)
    x = right - margin;
  if (y <= top + 1)
    y = top + margin;
  else if (y >= bottom - 1)
    y = bottom - margin;
  if (x < left)
    x = left;
  if (y < top)
    y = top;
  if (x > right)
    x = right;
  if (y > bottom)
    y = bottom;
}
