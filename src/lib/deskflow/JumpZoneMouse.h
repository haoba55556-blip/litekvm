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
