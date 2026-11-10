// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

// ShowCursor's display counter is per thread. It hides the pointer only
// while that pointer is over a window the calling thread created. A hider
// that does not hit-test, or that sits at the screen center while the
// pointer is still on the leave edge, does not hide anything.

// Half-size of the hider, in pixels. Absolute moves round by about a pixel,
// so a 1x1 window misses the pointer it was meant to cover.
inline constexpr int32_t kCursorHiderSlop = 16;

struct CursorHiderRect
{
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
};

// A window centered on the pointer, large enough that rounding still lands
// inside it. The leave edge (for example x=2519) is not covered by a 1x1
// window left at the screen center.
inline CursorHiderRect cursorHiderRect(int32_t pointerX, int32_t pointerY, int32_t slop = kCursorHiderSlop)
{
  if (slop < 0)
    slop = 0;
  const int32_t size = slop * 2 + 1;
  return CursorHiderRect{pointerX - slop, pointerY - slop, size, size};
}

inline bool pointerOverHider(int32_t pointerX, int32_t pointerY, const CursorHiderRect &hider)
{
  if (hider.w < 1 || hider.h < 1)
    return false;
  return pointerX >= hider.x && pointerY >= hider.y && pointerX < hider.x + hider.w && pointerY < hider.y + hider.h;
}

// True when ShowCursor on the desk thread cannot hide this pointer: the
// hider does not claim the hit, or the pointer is not inside it. The system
// cursor sprite then has to be replaced or it stays on screen.
inline bool showCursorCannotHide(bool hiderClaimsHitTest, bool pointerIsOverHider)
{
  return !hiderClaimsHitTest || !pointerIsOverHider;
}

// Local motion that leaves the hider means the user grabbed the physical
// mouse. The park warp lands inside the hider and must not dismiss it.
inline bool dismissHiderOnLocalMove(int32_t clientX, int32_t clientY, int32_t width, int32_t height)
{
  if (width < 1 || height < 1)
    return true;
  return clientX < 0 || clientY < 0 || clientX >= width || clientY >= height;
}
