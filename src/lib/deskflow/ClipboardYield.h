// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Clipboard messages share the pointer socket. One protocol piece is small
// enough that the event loop can write a mouse move before the next piece.
// File transfer does not use this path (it has its own connection).
inline constexpr size_t kClipboardChunkBytes = 32 * 1024;

// Larger than one piece: a switch defers these until the landing settle
// ends, so the park window is not spent pushing a megabyte.
inline constexpr size_t kLargeClipboardBytes = 64 * 1024;

// Gap before the next clipboard piece when nothing else is queued.
// Mouse messages already in the queue run first; timers wait for an
// empty queue. Kept well under a frame so the clipboard still arrives.
inline constexpr double kClipboardYieldSeconds = 0.001;

// Pull one clipboard protocol message at a time. A mouse move can be
// written after any piece, including the size header, before the payload
// has been sent.
class ClipboardPieceCursor
{
public:
  explicit ClipboardPieceCursor(size_t payloadBytes) : m_size(payloadBytes)
  {
  }

  size_t payloadBytes() const
  {
    return m_size;
  }

  bool finished() const
  {
    return m_phase == Phase::Done;
  }

  // @p piece is 0 (size header), 1 (payload), or 2 (end).
  // Returns false when the send is already finished.
  bool takePiece(int &piece, size_t &offset, size_t &size)
  {
    if (m_phase == Phase::Start) {
      piece = 0;
      offset = 0;
      size = m_size;
      m_phase = (m_size == 0) ? Phase::EmptyData : Phase::Data;
      return true;
    }
    if (m_phase == Phase::EmptyData) {
      piece = 1;
      offset = 0;
      size = 0;
      m_phase = Phase::End;
      return true;
    }
    if (m_phase == Phase::Data) {
      piece = 1;
      offset = m_offset;
      size_t n = kClipboardChunkBytes;
      if (m_offset + n > m_size)
        n = m_size - m_offset;
      size = n;
      m_offset += n;
      if (m_offset >= m_size)
        m_phase = Phase::End;
      return true;
    }
    if (m_phase == Phase::End) {
      piece = 2;
      offset = 0;
      size = 0;
      m_phase = Phase::Done;
      return true;
    }
    return false;
  }

private:
  enum class Phase
  {
    Start,
    Data,
    EmptyData,
    End,
    Done
  };

  size_t m_size = 0;
  size_t m_offset = 0;
  Phase m_phase = Phase::Start;
};

// A remote screen that is still inside its landing settle should not take
// a megabyte on the pointer socket. Small clipboards, local screens, and
// sends after the settle go out immediately (still one piece per turn).
inline bool deferLargeClipboardDuringSettle(size_t bytes, bool settleActive, bool remoteActive)
{
  return settleActive && remoteActive && bytes > kLargeClipboardBytes;
}

// What to do when the active screen is remote and the primary grabs the
// clipboard. The same bytes must not be pushed again: a grab empties the
// screen that already has them, and the refill is the stall. A send that
// was deferred across the landing still has to go out, so it is kept.
struct ClipboardGrabAction
{
  bool grabActive = false;
  bool sendNow = false;
  bool keepDeferred = false;
};

inline ClipboardGrabAction clipboardGrabResend(bool sameBytes, bool deferred, bool activeIsRemote)
{
  ClipboardGrabAction action;
  if (!activeIsRemote) {
    action.keepDeferred = deferred;
    return action;
  }
  if (sameBytes) {
    action.keepDeferred = deferred;
    return action;
  }
  action.grabActive = true;
  action.sendNow = true;
  action.keepDeferred = false;
  return action;
}

// Bytes of a clipboard that has finished arriving. The read loop records
// these and keeps parsing; the OS clipboard is updated only from takePending(),
// after mouse messages already in the buffer have been applied.
struct DeferredClipboard
{
  uint8_t id = 0;
  std::string bytes;
};

class ClipboardApplyGate
{
public:
  void defer(uint8_t id, std::string bytes)
  {
    m_pending.push_back(DeferredClipboard{id, std::move(bytes)});
  }

  bool hasPending() const
  {
    return !m_pending.empty();
  }

  std::vector<DeferredClipboard> takePending()
  {
    std::vector<DeferredClipboard> out;
    out.swap(m_pending);
    return out;
  }

private:
  std::vector<DeferredClipboard> m_pending;
};

// Read-loop order. 'M' is a mouse message (applied immediately). 'C' is a
// clipboard piece still in progress. 'F' is the last piece: the bytes are
// queued, and every mouse in this buffer is traced before "apply".
inline std::vector<std::string>
replayClipboardAndMouse(const std::vector<char> &ops, const std::string &payload)
{
  ClipboardApplyGate gate;
  std::vector<std::string> trace;
  for (char op : ops) {
    if (op == 'M')
      trace.emplace_back("mouse");
    else if (op == 'F')
      gate.defer(0, payload);
  }
  for (const auto &item : gate.takePending()) {
    (void)item;
    trace.emplace_back("apply");
  }
  return trace;
}
