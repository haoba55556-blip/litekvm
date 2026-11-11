// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// The receiving side used to spin the GUI thread for up to 2 s, then call
// DoDragDrop, which blocks until the button comes up. The shell drop was
// cancelled anyway and the file was copied on button-up. Poll instead, and
// ignore a second land of the same drag. DoDragDrop stays off this thread.
#pragma once

#include <QStringList>

namespace litekvm {

class RemoteDragWait
{
public:
  // Long enough for the edge-grace cross (~0.4 s) to inject the button after
  // a small file has already arrived while the pointer was still remote
  // (deferred-pointer-remote / mid-cross). Short of the old 2 s GUI stall.
  // A landing that is already pointer-local skips this grace: the button
  // state is real, and already-up means the drop has happened.
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

  /// Pointer was already on this machine when the file finished. A button that
  /// is already up copies on the next tick. Leave this off for a deferred or
  /// mid-cross landing so the ~0.4 s button injection can still be seen.
  void setSkipAppearGrace(bool skip)
  {
    m_skipAppearGrace = skip;
  }

  bool skipAppearGrace() const
  {
    return m_skipAppearGrace;
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
    // pointer-local + already-up: the drop already happened. Do not burn
    // kAppearMs waiting for a button injection that only follows a cross.
    if (!m_sawButton && !buttonDown && m_skipAppearGrace)
      return Decision::CopyAlreadyUp;
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
  bool m_skipAppearGrace = false;
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
