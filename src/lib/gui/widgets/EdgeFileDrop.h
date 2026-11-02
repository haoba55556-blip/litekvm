/*
 * LiteKVM -- file drag across the screen edge.
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0
 */
#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QWidget;

namespace deskflow::gui {

/**
 * Thin always-on-top strips along every screen edge.
 *
 * On Windows Qt implements QWidget drag-and-drop with OLE: the strip is an
 * IDropTarget, and a later QDrag::exec() is DoDragDrop. Dragging a file onto
 * the edge (the same edge the pointer uses to change computers) captures the
 * CF_HDROP / file URL list and emits filesDragged(). The caller sends those
 * paths on the clipboard-file channel; the other computer starts a QDrag so
 * the drop can finish in Explorer.
 *
 * A short hold (kHoldMs) ignores a pointer that only brushes the edge.
 */
class EdgeFileDrop : public QObject {
  Q_OBJECT

public:
  static constexpr int kStripPx = 4;
  static constexpr int kHoldMs = 150;

  explicit EdgeFileDrop(QObject *parent = nullptr);
  ~EdgeFileDrop() override;

  void setEnabled(bool on);
  bool isEnabled() const
  {
    return m_enabled;
  }

  /// Widget to use as the QDrag source (a visible strip, or nullptr).
  QWidget *dragSource() const;

Q_SIGNALS:
  void filesDragged(const QStringList &paths);

private:
  void rebuild();
  void destroyStrips();

  bool m_enabled = false;
  QList<QPointer<QWidget>> m_strips;
};

} // namespace deskflow::gui
