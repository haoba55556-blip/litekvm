/*
 * LiteKVM -- file drag across the screen edge.
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0
 */
#pragma once

#include "litekvm/DragEdgeGate.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QWidget;

namespace deskflow::gui {

/**
 * Always-on-top strips along every screen edge.
 *
 * On Windows Qt's drag-and-drop is OLE: the strip is an IDropTarget, and a
 * later QDrag::exec() is DoDragDrop. The transfer starts on DragEnter. A
 * dwell timer does not work — the core switches screens (and warps the
 * cursor) in a few milliseconds, which delivers DragLeave and used to cancel
 * the offer before it was sent.
 *
 * The strip is wider than the 1 px jump zone so OLE can hit-test it before
 * the cursor leaves this computer. It is not using setWindowOpacity(); that
 * flag makes some Windows builds skip hit-testing.
 */
class EdgeFileDrop : public QObject {
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

  /// Widget to use as the QDrag source (a visible strip, or nullptr).
  QWidget *dragSource() const;

  /// DragEnter/Drop. Debounced; does not wait for a dwell.
  void beginDrag(const QStringList &paths);
  void noteDragLeft();

Q_SIGNALS:
  void filesDragged(const QStringList &paths);

private:
  void rebuild();
  void destroyStrips();

  bool m_enabled = false;
  QList<QPointer<QWidget>> m_strips;
  litekvm::DragEdgeGate m_gate;
};

} // namespace deskflow::gui
