// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <QWidget>

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

namespace deskflow::gui {

/**
 * @brief Neomorphic LiteKVM home stage (scheme C).
 * Paints the signed-off main window. It does not own KVM sessions; MainWindow
 * wires the toggle, disconnect, and settings gear to the existing actions.
 */
class NeoHome : public QWidget
{
  Q_OBJECT

public:
  enum class FrameHit
  {
    Outside,
    Client,
    Caption,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
  };

  explicit NeoHome(QWidget *parent = nullptr);

  void setAutoConnectOn(bool on);
  void setLinkStatus(bool linked, const QString &text);
  void setDisconnectEnabled(bool enabled);

  [[nodiscard]] QRect cardRect() const;
  FrameHit frameHitAt(const QPoint &pos);

  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

Q_SIGNALS:
  void autoConnectToggled(bool on);
  void disconnectRequested();
  void settingsRequested();
  void minimizeRequested();
  void maximizeRequested();
  void closeRequested();

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;

private:
  enum class Control
  {
    None,
    Min,
    Max,
    Close,
    Gear,
    Toggle,
    Disconnect,
    Add,
    NoticeOk
  };

  void layoutChrome();
  [[nodiscard]] Control controlAt(const QPoint &pos) const;
  [[nodiscard]] bool maximized() const;

  bool m_autoOn = false;
  bool m_linked = false;
  bool m_disconnectEnabled = false;
  bool m_notice = false;
  QString m_statusText;
  Control m_pressed = Control::None;
  Control m_hover = Control::None;

  QRect m_card;
  QRect m_wordmark;
  QRect m_min;
  QRect m_max;
  QRect m_close;
  QRect m_gear;
  QRect m_monitor;
  QRect m_laptop;
  QRect m_arrow;
  QRect m_add;
  QRect m_toggle;
  QRect m_status;
  QRect m_dot;
  QRect m_disconnect;
  QRect m_noticeCard;
  QRect m_noticeOk;
  qreal m_scale = 1.0;
};

} // namespace deskflow::gui
