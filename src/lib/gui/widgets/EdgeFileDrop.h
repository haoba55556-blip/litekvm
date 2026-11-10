/*
 * LiteKVM -- file drag across the screen edge.
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "litekvm/DragEdgeGate.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QTimer;
class QWidget;

namespace deskflow::gui {

/**
 * Strips along every screen edge that receive an OLE/Qt file drag.
 *
 * On Windows Qt's drag-and-drop is OLE: the strip is an IDropTarget. The
 * transfer starts on DragEnter. A dwell timer does not work — the core
 * switches screens (and warps the cursor) in a few milliseconds, which
 * delivers DragLeave and used to cancel the offer before it was sent.
 *
 * The strips stay hidden unless a left-button drag has moved and is near an
 * edge. An always-shown strip is a wide top bar: Windows often ignores the
 * layered alpha, so the bar is opaque black and steals clicks on a maximized
 * browser. Hidden windows are neither visible nor hit-testable.
 *
 * While shown, a uniform layered alpha of 1 keeps WindowFromPoint (and
 * therefore OLE) on the strip. Per-pixel WA_TranslucentBackground does not.
 */
class EdgeFileDrop : public QObject
{
  Q_OBJECT

public:
  static constexpr int kStripPx = 16;

  explicit EdgeFileDrop(QObject *parent = nullptr);
  ~EdgeFileDrop() override;

  void setEnabled(bool on);
  bool isEnabled() const
  {
    return m_enabled;
  }

  /// Widget to use as the QDrag source (a strip, or nullptr). May be hidden.
  QWidget *dragSource() const;

  /// Edge strips currently built. Zero until setEnabled(true).
  int stripCount() const;

  /// Show the strips so OLE can hit-test them. Hidden again when @p on is false.
  void setStripsArmed(bool on);
  bool stripsArmed() const
  {
    return m_armed;
  }

  /// DragEnter/Drop. Debounced for one gesture; does not wait for a dwell.
  void beginDrag(const QStringList &paths);
  void noteDragLeft();

  /// While a remote drop is being placed, the strips must not be the window
  /// under the cursor. They are Qt::Tool and otherwise hide the desktop
  /// (Progman/WorkerW) from WindowFromPoint.
  void setPassThrough(bool on);
  bool passThrough() const
  {
    return m_passThrough;
  }

Q_SIGNALS:
  void filesDragged(const QStringList &paths);

private:
  void rebuild();
  void destroyStrips();
  void pollDragArm();
  void applyStripMode();

  bool m_enabled = false;
  bool m_armed = false;
  bool m_passThrough = false;
  QList<QPointer<QWidget>> m_strips;
  litekvm::DragEdgeGate m_gate;
  litekvm::EdgeStripArm m_arm;
  QTimer *m_armTimer = nullptr;
};

} // namespace deskflow::gui
