// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// The receiving side used to spin the GUI thread for up to 2 s, then call
// DoDragDrop, which blocks until the button comes up. The shell drop was
// cancelled anyway and the file was copied on button-up. Poll instead, and
// ignore a second land of the same drag.
#pragma once

#include <QStringList>

namespace litekvm {

class RemoteDragWait
{
public:
  // Long enough for the edge-grace cross (~0.4 s) to inject the button after
  // a small file has already arrived. Short of the old 2 s GUI stall.
  static constexpr qint64 kAppearMs = 600;
  static constexpr qint64 kGiveUpMs = 8000;

  enum class Decision
  {
    KeepWaiting,
    CopyOnRelease,
    CopyAlreadyUp,
    CopyTimeout,
  };

  void reset()
  {
    m_sawButton = false;
  }

  bool sawButton() const
  {
    return m_sawButton;
  }

  Decision tick(bool buttonDown, qint64 elapsedMs)
  {
    if (buttonDown)
      m_sawButton = true;
    if (m_sawButton && !buttonDown)
      return Decision::CopyOnRelease;
    if (!m_sawButton && elapsedMs >= kAppearMs)
      return Decision::CopyAlreadyUp;
    if (elapsedMs >= kGiveUpMs)
      return Decision::CopyTimeout;
    return Decision::KeepWaiting;
  }

  static const char *reason(Decision decision)
  {
    switch (decision) {
    case Decision::CopyOnRelease:
      return "release";
    case Decision::CopyAlreadyUp:
      return "already-up";
    case Decision::CopyTimeout:
      return "timeout";
    case Decision::KeepWaiting:
      return "wait";
    }
    return "wait";
  }

private:
  bool m_sawButton = false;
};

class RemoteDropLatch
{
public:
  static constexpr qint64 kDuplicateMs = 3000;

  /// @return false when this is the same drag landing again.
  bool accept(const QStringList &paths, qint64 nowMs)
  {
    if (!paths.isEmpty() && paths == m_paths && nowMs - m_at < kDuplicateMs)
      return false;
    m_paths = paths;
    m_at = nowMs;
    return true;
  }

private:
  QStringList m_paths;
  qint64 m_at = -100000;
};

} // namespace litekvm
