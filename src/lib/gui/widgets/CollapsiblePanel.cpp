// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// CollapsiblePanel — clickable header + expandable content, instant toggle.

#include "CollapsiblePanel.h"

namespace deskflow::gui {

CollapsiblePanel::CollapsiblePanel(const QString &title, QWidget *content,
                                   QWidget *parent, int badgeCount)
    : QWidget(parent), m_content(content)
{
  // --- outer vertical layout ---
  auto *outerLayout = new QVBoxLayout(this);
  outerLayout->setContentsMargins(0, 0, 0, 0);
  outerLayout->setSpacing(0);

  // --- header button (clickable entire bar) ---
  m_headerBtn = new QPushButton(this);
  m_headerBtn->setFixedHeight(32);
  m_headerBtn->setCursor(Qt::PointingHandCursor);

  // Header internal horizontal layout (QPushButton can host a QHBoxLayout)
  auto *headerLayout = new QHBoxLayout(m_headerBtn);
  headerLayout->setContentsMargins(10, 0, 10, 0);
  headerLayout->setSpacing(6);

  m_arrowLabel = new QLabel(QStringLiteral("\u25BC"), m_headerBtn); // ▼ = expanded
  m_arrowLabel->setStyleSheet(QStringLiteral("color: #A2ABB8; font-size: 10px;"));
  headerLayout->addWidget(m_arrowLabel);

  m_titleLabel = new QLabel(title, m_headerBtn);
  m_titleLabel->setStyleSheet(
      QStringLiteral("color: #E7EAEE; font-weight: bold; font-size: 13px;"));
  headerLayout->addWidget(m_titleLabel);

  headerLayout->addStretch();

  m_badgeLabel = new QLabel(m_headerBtn);
  m_badgeLabel->setStyleSheet(
      QStringLiteral("color: #8E95A2; font-size: 11px; padding: 1px 6px;"
                     "background-color: #1F242B; border-radius: 8px;"));
  if (badgeCount >= 0)
    m_badgeLabel->setText(QString::number(badgeCount));
  else
    m_badgeLabel->hide();
  headerLayout->addWidget(m_badgeLabel);

  // Style the header bar (FlatToolButton look)
  m_headerBtn->setStyleSheet(
      QStringLiteral("QPushButton { background-color: #14171C; border: none;"
                     "  border-radius: 8px; padding: 0px; text-align: left; }"
                     "QPushButton:hover { background-color: #1B2027; }"));

  outerLayout->addWidget(m_headerBtn);

  // --- content area wrapper ---
  m_contentArea = new QWidget(this);
  auto *contentLayout = new QVBoxLayout(m_contentArea);
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(0);
  contentLayout->addWidget(m_content);
  outerLayout->addWidget(m_contentArea);

  // --- connections ---
  connect(m_headerBtn, &QPushButton::clicked, this, &CollapsiblePanel::toggle);
}

void CollapsiblePanel::setExpanded(bool expanded)
{
  if (m_expanded == expanded)
    return;
  m_expanded = expanded;
  updateArrow();
  m_contentArea->setVisible(m_expanded);
  Q_EMIT expandedChanged(m_expanded);
}

void CollapsiblePanel::toggle()
{
  setExpanded(!m_expanded);
}

void CollapsiblePanel::setBadgeCount(int count)
{
  if (count < 0) {
    m_badgeLabel->hide();
    return;
  }
  m_badgeLabel->setText(QString::number(count));
  m_badgeLabel->show();
}

void CollapsiblePanel::updateArrow()
{
  m_arrowLabel->setText(m_expanded ? QStringLiteral("\u25BC") : QStringLiteral("\u25B6"));
}

} // namespace deskflow::gui