#pragma once

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace deskflow::gui {

/**
 * @brief A collapsible panel: clickable header bar + expandable content area.
 *
 *          ┌────────────────────────────────┐
 *          │  ▼ 附近设备           [count]  │ ← click to toggle
 *          ├────────────────────────────────┤
 *          │  (content widget)              │ ← shown/hidden on toggle
 *          │  ...                           │
 *          └────────────────────────────────┘
 */
class CollapsiblePanel : public QWidget
{
  Q_OBJECT

public:
  explicit CollapsiblePanel(const QString &title, QWidget *content,
                            QWidget *parent = nullptr, int badgeCount = -1);

  void setExpanded(bool expanded);
  bool isExpanded() const { return m_expanded; }
  void setBadgeCount(int count);

  QWidget *contentWidget() const { return m_content; }

Q_SIGNALS:
  void expandedChanged(bool expanded);

private:
  void toggle();
  void updateArrow();

  QWidget *m_content = nullptr;
  QLabel *m_arrowLabel = nullptr;
  QLabel *m_titleLabel = nullptr;
  QLabel *m_badgeLabel = nullptr;
  QPushButton *m_headerBtn = nullptr;
  QWidget *m_contentArea = nullptr;
  bool m_expanded = true;
};

} // namespace deskflow::gui