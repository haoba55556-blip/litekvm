// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// When an OLE/Qt drag enters the screen edge, start the transfer immediately.
//
// The pointer crosses into the other computer in well under 150 ms. A dwell
// timer is cancelled by dragLeave as soon as the screen switch warps the
// cursor, so the files never leave. Leaving the strip must not undo an enter
// that already started a transfer. A short debounce stops the same drag from
// being offered twice if enter/leave flicker.
#pragma once

#include <QStringList>

namespace litekvm {

class DragEdgeGate {
public:
  static constexpr qint64 kDebounceMs = 1500;

  /// @return true when this enter should start a file transfer now.
  bool onEnter(const QStringList &paths, qint64 nowMs)
  {
    if (paths.isEmpty())
      return false;
    if (paths == m_lastPaths && nowMs - m_lastMs < kDebounceMs)
      return false;
    m_lastPaths = paths;
    m_lastMs = nowMs;
    m_started = true;
    return true;
  }

  /// The cursor left the strip (usually because the screen switched).
  /// Does not revoke a transfer onEnter() already accepted.
  void onLeave()
  {
  }

  bool transferStarted() const
  {
    return m_started;
  }

private:
  QStringList m_lastPaths;
  qint64 m_lastMs = -100000;
  bool m_started = false;
};

} // namespace litekvm
