// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

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
