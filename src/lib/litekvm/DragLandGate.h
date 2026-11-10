// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// When a drag file finishes arriving, drop it only once the pointer is on
// this computer. The laptop receive path sees the pointer already here.
// The host can accept the offer while the pointer is still on the laptop;
// the bytes stay on disk until the pointer switches back.
#pragma once

#include <QString>
#include <QStringList>

namespace litekvm {

class DragLandGate
{
public:
  void setPointerLocal(bool local)
  {
    m_local = local;
  }

  bool pointerLocal() const
  {
    return m_local;
  }

  QStringList waiting() const
  {
    return m_waiting;
  }

  /// A drag payload is on disk. @return paths to drop now.
  QStringList receiveDrag(const QStringList &paths, QString *reason)
  {
    if (paths.isEmpty()) {
      if (reason != nullptr)
        *reason = QStringLiteral("no-paths");
      return {};
    }
    if (!m_local) {
      m_waiting = paths;
      if (reason != nullptr)
        *reason = QStringLiteral("deferred-pointer-remote");
      return {};
    }
    m_waiting.clear();
    if (reason != nullptr)
      *reason = QStringLiteral("pointer-local");
    return paths;
  }

  /// Pointer state changed. @return a deferred drop that can run now.
  QStringList takeIfPointerLocal(QString *reason)
  {
    if (!m_local) {
      if (reason != nullptr)
        *reason = QStringLiteral("still-remote");
      return {};
    }
    if (m_waiting.isEmpty()) {
      if (reason != nullptr)
        *reason = QStringLiteral("nothing-waiting");
      return {};
    }
    const QStringList out = m_waiting;
    m_waiting.clear();
    if (reason != nullptr)
      *reason = QStringLiteral("pointer-arrived");
    return out;
  }

private:
  // No screen-switch message yet: the pointer is on this machine. The server
  // GUI flips this off when the core says the active screen is the peer.
  bool m_local = true;
  QStringList m_waiting;
};

} // namespace litekvm
