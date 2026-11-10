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

// A resolved neighbor that is the screen we are already on is not a cross.
// switchScreen used to accept it and warp the pointer to the far edge
// (width - 1 - jump zone, 1918 on a 1920-wide screen), which pins it there.
inline bool sameScreenName(const char *a, const char *b)
{
  if (a == nullptr || b == nullptr)
    return false;
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z')
      ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb)
      return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

inline bool switchesToOtherScreen(const char *active, const char *candidate)
{
  if (candidate == nullptr || candidate[0] == '\0')
    return false;
  return !sameScreenName(active, candidate);
}

// The primary hides its cursor by warping to the center. That sample is about
// half a screen wide. The old check only dropped deltas within 10 px of that
// distance, so a DPI-rounded warp was applied on the other machine and threw
// the pointer back across the edge.
inline bool isCenterWarpEcho(int32_t dx, int32_t dy, int32_t width, int32_t height)
{
  const int32_t limitX = width > 4 ? width / 4 : 1;
  const int32_t limitY = height > 4 ? height / 4 : 1;
  return dx >= limitX || dx <= -limitX || dy >= limitY || dy <= -limitY;
}

inline bool
pointerInEdgeBand(int32_t x, int32_t y, int32_t left, int32_t top, int32_t width, int32_t height, int32_t band)
{
  if (width < 1 || height < 1 || band < 1)
    return false;
  const int32_t right = left + width;
  const int32_t bottom = top + height;
  return x < left + band || y < top + band || x >= right - band || y >= bottom - band;
}

// A held button at the edge must keep its switch hold across 1 px of jitter.
// Resetting on every sample outside the jump zone restarts the hold forever.
inline bool resetDragEdgeHold(bool buttonDown, bool inEdgeBand)
{
  if (!buttonDown)
    return true;
  return !inEdgeBand;
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

// How far inside the destination a drag should land, and how far from the
// edge the pointer is held while that landing settles. Farther than the 1 px
// jump zone, so the warp back to center cannot throw the pointer across.
inline constexpr int32_t kDragLandingMargin = 120;

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

// While a drag landing is settling, keep an absolute pointer (the primary
// screen reports positions, not deltas) out of the edge band. @return true
// when @p x or @p y was moved.
inline bool
clampInsideMargin(int32_t left, int32_t top, int32_t width, int32_t height, int32_t margin, int32_t &x, int32_t &y)
{
  if (width < 1 || height < 1)
    return false;
  if (margin < 1)
    margin = 1;
  if (width <= margin * 2 || height <= margin * 2)
    margin = 1;
  const int32_t minX = left + margin;
  const int32_t maxX = left + width - 1 - margin;
  const int32_t minY = top + margin;
  const int32_t maxY = top + height - 1 - margin;
  const int32_t oldX = x;
  const int32_t oldY = y;
  if (x < minX)
    x = minX;
  else if (x > maxX)
    x = maxX;
  if (y < minY)
    y = minY;
  else if (y > maxY)
    y = maxY;
  return x != oldX || y != oldY;
}
