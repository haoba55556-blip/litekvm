// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only

#include "NeoHome.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>

namespace deskflow::gui {

namespace {

constexpr int kShadow = 16;
constexpr int kRadius = 22;
constexpr int kBorder = 8;
constexpr qreal kDesignW = 1248.0;
constexpr qreal kDesignH = 688.0;

const QColor kCream(243, 238, 230);
const QColor kCreamDeep(232, 224, 212);
const QColor kFace(248, 244, 237);
const QColor kBlue(59, 124, 245);
const QColor kDash(206, 194, 176);
const QColor kChrome(158, 148, 134);

QFont uiFont(int pixel, QFont::Weight weight, qreal spacing = 0.0)
{
  QFont font;
  font.setFamilies(
      {QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
       QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Source Han Sans SC")}
  );
  font.setPixelSize(qMax(12, pixel));
  font.setWeight(weight);
  font.setStyleStrategy(QFont::PreferAntialias);
  if (spacing != 0.0)
    font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
  return font;
}

void drawEmbossedText(QPainter &painter, const QRectF &rect, const QString &text, const QFont &font, int flags)
{
  painter.setFont(font);
  painter.setPen(QColor(156, 138, 114, 120));
  painter.drawText(rect.translated(0.7, 1.25), flags, text);
  painter.setPen(QColor(255, 253, 249, 230));
  painter.drawText(rect.translated(-0.7, -0.8), flags, text);
  painter.setPen(kFace);
  painter.drawText(rect, flags, text);
}

void paintSoftShadow(QPainter &painter, const QRectF &rect, qreal radius, qreal depth, bool light)
{
  const int layers = light ? 5 : 8;
  for (int i = layers; i >= 1; --i) {
    const qreal grow = i * (light ? 0.42 : 0.55) * depth;
    QColor color = light ? QColor(255, 255, 255, 14 + (layers - i) * 8) : QColor(148, 128, 104, 7 + (layers - i) * 4);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const qreal dx = light ? -0.9 : 1.4;
    const qreal dy = light ? -0.9 : 1.8;
    painter.drawRoundedRect(
        rect.adjusted(-grow + dx, -grow + dy, grow * (light ? 0.25 : 1.0) + dx, grow * (light ? 0.2 : 1.05) + dy),
        radius + grow * 0.35, radius + grow * 0.35
    );
  }
}

void paintRaised(QPainter &painter, const QRectF &rect, qreal radius, bool pressed, qreal depth)
{
  painter.setPen(Qt::NoPen);
  if (!pressed) {
    paintSoftShadow(painter, rect, radius, depth, false);
    paintSoftShadow(painter, rect, radius, depth * 0.8, true);
  }
  painter.setBrush(kCream);
  painter.drawRoundedRect(rect, radius, radius);

  painter.save();
  QPainterPath clip;
  clip.addRoundedRect(rect, radius, radius);
  painter.setClipPath(clip);
  QLinearGradient sheen(rect.topLeft(), QPointF(rect.left(), rect.top() + rect.height() * 0.55));
  sheen.setColorAt(0.0, QColor(255, 255, 255, pressed ? 18 : 78));
  sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
  painter.fillRect(rect, sheen);
  if (pressed) {
    QLinearGradient inset(rect.topLeft(), QPointF(rect.left(), rect.top() + 16.0));
    inset.setColorAt(0.0, QColor(160, 140, 116, 80));
    inset.setColorAt(1.0, QColor(160, 140, 116, 0));
    painter.fillRect(rect, inset);
  }
  painter.restore();
}

void paintScreen(QPainter &painter, const QRectF &screen)
{
  QLinearGradient glass(screen.topLeft(), screen.bottomRight());
  glass.setColorAt(0.0, QColor(255, 255, 255, 245));
  glass.setColorAt(0.42, QColor(244, 239, 230));
  glass.setColorAt(1.0, QColor(214, 204, 188));
  painter.setPen(Qt::NoPen);
  painter.setBrush(glass);
  painter.drawRoundedRect(screen, 7, 7);

  painter.save();
  QPainterPath clip;
  clip.addRoundedRect(screen, 7, 7);
  painter.setClipPath(clip);
  QLinearGradient gloss(screen.topLeft(), screen.bottomLeft());
  gloss.setColorAt(0.0, QColor(255, 255, 255, 150));
  gloss.setColorAt(0.38, QColor(255, 255, 255, 0));
  painter.fillRect(screen, gloss);
  QLinearGradient shade(screen.topRight(), screen.bottomRight());
  shade.setColorAt(0.55, QColor(180, 164, 142, 0));
  shade.setColorAt(1.0, QColor(170, 152, 128, 55));
  painter.fillRect(screen, shade);
  painter.restore();
}

void paintMonitor(QPainter &painter, const QRectF &box)
{
  const QRectF bezel(box.left(), box.top(), box.width(), box.height() * 0.74);
  paintRaised(painter, bezel, 16, false, 1.15);
  paintScreen(painter, bezel.adjusted(12, 11, -12, -20));

  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(214, 204, 190));
  painter.drawEllipse(QRectF(bezel.center().x() - 3.2, bezel.bottom() - 14.0, 6.4, 6.4));

  const QRectF neck(box.center().x() - 8.0, bezel.bottom() - 2.0, 16.0, box.height() * 0.13);
  paintRaised(painter, neck, 3.0, false, 0.45);
  const QRectF foot(box.center().x() - box.width() * 0.22, neck.bottom() - 3.0, box.width() * 0.44, 11.0);
  paintRaised(painter, foot, 5.0, false, 0.7);
}

void paintLaptop(QPainter &painter, const QRectF &box)
{
  const QRectF screen(box.left() + box.width() * 0.06, box.top(), box.width() * 0.88, box.height() * 0.70);
  paintRaised(painter, screen, 12, false, 1.05);
  paintScreen(painter, screen.adjusted(10, 9, -10, -12));

  const QRectF base(box.left(), screen.bottom() - 6.0, box.width(), box.height() * 0.20);
  paintRaised(painter, base, 8, false, 0.85);
  painter.setPen(QPen(QColor(186, 172, 152, 140), 1.15));
  painter.drawLine(QPointF(base.left() + 10.0, base.top() + 1.0), QPointF(base.right() - 10.0, base.top() + 1.0));
}

void paintArrow(QPainter &painter, const QRectF &rect)
{
  const qreal y = rect.center().y();
  const qreal x0 = rect.left() + 6.0;
  const qreal x1 = rect.right() - 6.0;
  const QColor ink(196, 184, 166);
  auto head = [&](qreal tipX, qreal dir) {
    QPainterPath path;
    path.moveTo(tipX, y);
    path.lineTo(tipX - dir * 8.0, y - 4.2);
    path.moveTo(tipX, y);
    path.lineTo(tipX - dir * 8.0, y + 4.2);
    return path;
  };

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(QColor(255, 255, 255, 200), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawLine(QPointF(x0, y - 0.7), QPointF(x1, y - 0.7));
  painter.drawPath(head(x0, -1.0));
  painter.drawPath(head(x1, 1.0));
  painter.setPen(QPen(ink, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawLine(QPointF(x0, y), QPointF(x1, y));
  painter.drawPath(head(x0, -1.0));
  painter.drawPath(head(x1, 1.0));
}

enum class ControlGlyph
{
  Min,
  Max,
  Restore,
  Close
};

void paintGear(QPainter &painter, const QPointF &center)
{
  auto draw = [&](const QColor &color, const QPointF &offset) {
    painter.save();
    painter.translate(center + offset);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (int i = 0; i < 8; ++i) {
      painter.rotate(45.0);
      painter.drawRoundedRect(QRectF(-2.7, -15.2, 5.4, 6.2), 1.4, 1.4);
    }
    painter.drawEllipse(QPointF(0, 0), 8.4, 8.4);
    painter.restore();
  };
  draw(QColor(160, 142, 118, 90), QPointF(0.8, 1.1));
  draw(kFace, QPointF(0, 0));
  painter.setPen(Qt::NoPen);
  painter.setBrush(kCream);
  painter.drawEllipse(QRectF(center.x() - 3.6, center.y() - 3.6, 7.2, 7.2));
}

void paintWindowGlyph(QPainter &painter, ControlGlyph glyph, const QRectF &rect, bool hovered)
{
  if (hovered) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 110));
    painter.drawEllipse(rect.adjusted(5, 4, -5, -4));
  }
  painter.setPen(QPen(kChrome, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  const QPointF c = rect.center();
  if (glyph == ControlGlyph::Min) {
    painter.drawLine(QPointF(c.x() - 6.0, c.y() + 1.0), QPointF(c.x() + 6.0, c.y() + 1.0));
    return;
  }
  if (glyph == ControlGlyph::Close) {
    painter.drawLine(QPointF(c.x() - 5.2, c.y() - 5.2), QPointF(c.x() + 5.2, c.y() + 5.2));
    painter.drawLine(QPointF(c.x() + 5.2, c.y() - 5.2), QPointF(c.x() - 5.2, c.y() + 5.2));
    return;
  }
  if (glyph == ControlGlyph::Restore) {
    painter.drawRoundedRect(QRectF(c.x() - 6.2, c.y() - 3.2, 8.2, 8.2), 1.4, 1.4);
    painter.drawLine(QPointF(c.x() - 2.4, c.y() - 3.2), QPointF(c.x() - 2.4, c.y() - 6.0));
    painter.drawLine(QPointF(c.x() - 2.4, c.y() - 6.0), QPointF(c.x() + 6.0, c.y() - 6.0));
    painter.drawLine(QPointF(c.x() + 6.0, c.y() - 6.0), QPointF(c.x() + 6.0, c.y() + 2.2));
    painter.drawLine(QPointF(c.x() + 6.0, c.y() + 2.2), QPointF(c.x() + 2.0, c.y() + 2.2));
    return;
  }
  painter.drawRoundedRect(QRectF(c.x() - 5.5, c.y() - 5.5, 11.0, 11.0), 1.6, 1.6);
}

bool roundedContains(const QRect &rect, const QPoint &pos, int radius)
{
  if (!rect.contains(pos))
    return false;
  if (radius <= 0)
    return true;
  const int left = rect.left();
  const int top = rect.top();
  const int right = rect.right();
  const int bottom = rect.bottom();
  QPointF center;
  if (pos.x() < left + radius && pos.y() < top + radius)
    center = QPointF(left + radius, top + radius);
  else if (pos.x() > right - radius && pos.y() < top + radius)
    center = QPointF(right - radius, top + radius);
  else if (pos.x() < left + radius && pos.y() > bottom - radius)
    center = QPointF(left + radius, bottom - radius);
  else if (pos.x() > right - radius && pos.y() > bottom - radius)
    center = QPointF(right - radius, bottom - radius);
  else
    return true;
  const qreal dx = pos.x() - center.x();
  const qreal dy = pos.y() - center.y();
  return (dx * dx + dy * dy) <= qreal(radius) * qreal(radius);
}

} // namespace

NeoHome::NeoHome(QWidget *parent) : QWidget(parent)
{
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAutoFillBackground(false);
  setMouseTracking(true);
  setFocusPolicy(Qt::NoFocus);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_statusText = tr("未连接");
}

void NeoHome::setAutoConnectOn(bool on)
{
  if (m_autoOn == on)
    return;
  m_autoOn = on;
  update();
}

void NeoHome::setLinkStatus(bool linked, const QString &text)
{
  if (m_linked == linked && m_statusText == text)
    return;
  m_linked = linked;
  m_statusText = text;
  update();
}

void NeoHome::setDisconnectEnabled(bool enabled)
{
  if (m_disconnectEnabled == enabled)
    return;
  m_disconnectEnabled = enabled;
  update();
}

QSize NeoHome::sizeHint() const
{
  return {1280, 720};
}

QSize NeoHome::minimumSizeHint() const
{
  return {1000, 600};
}

bool NeoHome::maximized() const
{
  return window() && window()->isMaximized();
}

QRect NeoHome::cardRect() const
{
  if (maximized())
    return rect();
  return rect().adjusted(kShadow, kShadow, -kShadow, -kShadow);
}

void NeoHome::layoutChrome()
{
  m_card = cardRect();
  const qreal sx = m_card.width() / kDesignW;
  const qreal sy = m_card.height() / kDesignH;
  m_scale = qBound(0.72, qMin(sx, sy), 1.2);
  const qreal s = m_scale;
  const int left = m_card.left();
  const int top = m_card.top();
  const int right = m_card.right();

  const int btn = qRound(34 * s);
  const int btnTop = top + qRound(16 * s);
  const int btnGap = qRound(6 * s);
  m_close = QRect(right - qRound(16 * s) - btn, btnTop, btn, btn);
  m_max = QRect(m_close.left() - btnGap - btn, btnTop, btn, btn);
  m_min = QRect(m_max.left() - btnGap - btn, btnTop, btn, btn);
  m_wordmark = QRect(left + qRound(32 * s), btnTop - qRound(2 * s), qRound(240 * s), btn + qRound(4 * s));

  const int gear = qRound(50 * s);
  m_gear = QRect(right - qRound(22 * s) - gear, m_close.bottom() + qRound(12 * s), gear, gear);

  const int rowH = qRound(168 * s);
  const int monitorW = qRound(190 * s);
  const int monitorH = qRound(158 * s);
  const int arrowW = qRound(78 * s);
  const int laptopW = qRound(214 * s);
  const int laptopH = qRound(148 * s);
  const int gap = qRound(32 * s);
  const int addW = qRound(160 * s);
  const int addH = qRound(168 * s);
  const int clusterW = monitorW + arrowW + laptopW + gap + addW;
  const int labelH = qRound(30 * s);
  const int statusH = qRound(48 * s);
  const int blockH = labelH + qRound(6 * s) + rowH + qRound(32 * s) + statusH;
  const int freeTop = m_gear.bottom() + qRound(8 * s);
  const int freeBottom = m_card.bottom() - qRound(20 * s);
  const int blockTop = freeTop + qMax(0, (freeBottom - freeTop - blockH) / 2);
  const int iconTop = blockTop + labelH + qRound(6 * s);
  const int rowBottom = iconTop + rowH;
  const int clusterLeft = left + qMax(qRound(24 * s), (m_card.width() - clusterW) / 2);

  m_monitor = QRect(clusterLeft, rowBottom - monitorH, monitorW, monitorH);
  m_arrow = QRect(m_monitor.right(), m_monitor.top(), arrowW, monitorH);
  m_laptop = QRect(m_arrow.right(), rowBottom - laptopH, laptopW, laptopH);
  m_add = QRect(m_laptop.right() + gap, rowBottom - addH, addW, addH);

  const QFont statusFont = uiFont(qRound(18 * s), QFont::Normal);
  const QFontMetrics metrics(statusFont);
  const int labelW = metrics.horizontalAdvance(m_statusText.isEmpty() ? tr("已连接") : m_statusText);
  const int toggleW = qRound(62 * s);
  const int toggleH = qRound(32 * s);
  const int dot = qRound(8 * s);
  const int buttonW = qMax(qRound(88 * s), metrics.horizontalAdvance(tr("断开")) + qRound(36 * s));
  const int buttonH = qRound(42 * s);
  const int groupGap = qRound(12 * s);
  const int afterLabel = m_linked ? qRound(8 * s) + dot + qRound(16 * s) : qRound(16 * s);
  const int groupW = toggleW + groupGap + labelW + afterLabel + buttonW;
  const int pairW = monitorW + arrowW + laptopW;
  const int groupLeft = m_monitor.left() + qMax(0, (pairW - groupW) / 2);
  const int statusTop = rowBottom + qRound(28 * s);
  const int midY = statusTop + statusH / 2;
  m_toggle = QRect(groupLeft, midY - toggleH / 2, toggleW, toggleH);
  m_status = QRect(m_toggle.right() + groupGap, statusTop, labelW, statusH);
  m_dot = QRect(m_status.right() + qRound(8 * s), midY - dot / 2, dot, dot);
  const int buttonLeft = m_linked ? m_dot.right() + qRound(16 * s) : m_status.right() + qRound(16 * s);
  m_disconnect = QRect(buttonLeft, midY - buttonH / 2, buttonW, buttonH);

  const int noticeW = qRound(340 * s);
  const int noticeH = qRound(188 * s);
  m_noticeCard = QRect(
      m_card.left() + (m_card.width() - noticeW) / 2, m_card.top() + (m_card.height() - noticeH) / 2, noticeW, noticeH
  );
  const int okW = qRound(108 * s);
  const int okH = qRound(40 * s);
  m_noticeOk = QRect(m_noticeCard.center().x() - okW / 2, m_noticeCard.bottom() - qRound(24 * s) - okH, okW, okH);
}

NeoHome::FrameHit NeoHome::frameHitAt(const QPoint &pos)
{
  layoutChrome();
  const bool full = maximized();
  const int radius = full ? 0 : kRadius;
  if (!roundedContains(m_card, pos, radius))
    return FrameHit::Outside;
  if (full)
    return (pos.y() <= m_card.top() + qRound(52 * m_scale) && controlAt(pos) == Control::None) ? FrameHit::Caption
                                                                                               : FrameHit::Client;

  const bool left = pos.x() <= m_card.left() + kBorder;
  const bool right = pos.x() >= m_card.right() - kBorder;
  const bool top = pos.y() <= m_card.top() + kBorder;
  const bool bottom = pos.y() >= m_card.bottom() - kBorder;
  if (top && left)
    return FrameHit::TopLeft;
  if (top && right)
    return FrameHit::TopRight;
  if (bottom && left)
    return FrameHit::BottomLeft;
  if (bottom && right)
    return FrameHit::BottomRight;
  if (left)
    return FrameHit::Left;
  if (right)
    return FrameHit::Right;
  if (top)
    return FrameHit::Top;
  if (bottom)
    return FrameHit::Bottom;
  if (pos.y() <= m_card.top() + qRound(52 * m_scale) && controlAt(pos) == Control::None)
    return FrameHit::Caption;
  return FrameHit::Client;
}

NeoHome::Control NeoHome::controlAt(const QPoint &pos) const
{
  if (m_min.contains(pos))
    return Control::Min;
  if (m_max.contains(pos))
    return Control::Max;
  if (m_close.contains(pos))
    return Control::Close;
  if (m_notice) {
    if (m_noticeOk.contains(pos))
      return Control::NoticeOk;
    return Control::None;
  }
  if (m_gear.contains(pos))
    return Control::Gear;
  if (m_toggle.contains(pos))
    return Control::Toggle;
  if (m_disconnectEnabled && m_disconnect.contains(pos))
    return Control::Disconnect;
  if (m_add.contains(pos))
    return Control::Add;
  return Control::None;
}

void NeoHome::resizeEvent(QResizeEvent *event)
{
  QWidget::resizeEvent(event);
  layoutChrome();
}

void NeoHome::paintEvent(QPaintEvent *)
{
  layoutChrome();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(rect(), Qt::transparent);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

  const QRectF card = m_card;
  const qreal radius = maximized() ? 0.0 : kRadius;
  if (!maximized()) {
    for (int i = 14; i >= 1; --i) {
      QColor shadow(120, 98, 72, 5 + (14 - i));
      painter.setPen(Qt::NoPen);
      painter.setBrush(shadow);
      const qreal grow = i * 0.85;
      painter.drawRoundedRect(card.adjusted(-grow * 0.15, grow * 0.15, grow * 0.45, grow * 0.85), radius, radius);
    }
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(kCream);
  painter.drawRoundedRect(card, radius, radius);

  const qreal s = m_scale;
  drawEmbossedText(
      painter, m_wordmark, QStringLiteral("LiteKVM"), uiFont(qRound(26 * s), QFont::Medium, 0.7),
      Qt::AlignLeft | Qt::AlignVCenter
  );

  const bool zoomed = maximized();
  paintWindowGlyph(painter, ControlGlyph::Min, m_min, m_hover == Control::Min);
  paintWindowGlyph(painter, zoomed ? ControlGlyph::Restore : ControlGlyph::Max, m_max, m_hover == Control::Max);
  paintWindowGlyph(painter, ControlGlyph::Close, m_close, m_hover == Control::Close);

  const QFont labelFont = uiFont(qRound(18 * s), QFont::Normal);
  const int labelTop = m_monitor.top() - qRound(40 * s);
  const QRect localLabel(m_monitor.left(), labelTop, m_monitor.width(), qRound(28 * s));
  const QRect peerLabel(m_laptop.left(), labelTop, m_laptop.width(), qRound(28 * s));
  drawEmbossedText(painter, localLabel, tr("本机"), labelFont, Qt::AlignHCenter | Qt::AlignBottom);
  drawEmbossedText(painter, peerLabel, tr("笔记本"), labelFont, Qt::AlignHCenter | Qt::AlignBottom);

  paintMonitor(painter, m_monitor);
  paintArrow(painter, QRectF(m_arrow).adjusted(8, m_arrow.height() * 0.28, -8, -m_arrow.height() * 0.42));
  paintLaptop(painter, m_laptop);

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(kDash, 1.5, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawRoundedRect(QRectF(m_add).adjusted(1, 1, -1, -1), 16, 16);
  const QRectF plusCircle(
      m_add.center().x() - qRound(23 * s), m_add.top() + qRound(36 * s), qRound(46 * s), qRound(46 * s)
  );
  paintRaised(painter, plusCircle, plusCircle.width() / 2.0, m_pressed == Control::Add, 0.8);
  painter.setPen(Qt::NoPen);
  const QPointF plusCenter = plusCircle.center();
  const qreal arm = qRound(8 * s);
  const qreal thick = qMax(2.0, 2.4 * s);
  painter.setBrush(QColor(168, 154, 134, 90));
  painter.drawRoundedRect(QRectF(plusCenter.x() - thick / 2 + 0.6, plusCenter.y() - arm + 0.8, thick, arm * 2), 1, 1);
  painter.drawRoundedRect(QRectF(plusCenter.x() - arm + 0.6, plusCenter.y() - thick / 2 + 0.8, arm * 2, thick), 1, 1);
  painter.setBrush(kFace);
  painter.drawRoundedRect(QRectF(plusCenter.x() - thick / 2, plusCenter.y() - arm, thick, arm * 2), 1.1, 1.1);
  painter.drawRoundedRect(QRectF(plusCenter.x() - arm, plusCenter.y() - thick / 2, arm * 2, thick), 1.1, 1.1);
  const QRect addLabel(m_add.left() + 8, int(plusCircle.bottom()) + qRound(8 * s), m_add.width() - 16, qRound(28 * s));
  drawEmbossedText(painter, addLabel, tr("添加电脑"), labelFont, Qt::AlignHCenter | Qt::AlignTop);

  paintRaised(painter, m_gear, m_gear.width() / 2.0, m_pressed == Control::Gear, 0.9);
  paintGear(painter, m_gear.center());

  if (m_autoOn) {
    QLinearGradient blue(m_toggle.topLeft(), m_toggle.bottomLeft());
    blue.setColorAt(0.0, QColor(92, 150, 255));
    blue.setColorAt(1.0, QColor(47, 111, 236));
    painter.setPen(Qt::NoPen);
    painter.setBrush(blue);
    painter.drawRoundedRect(m_toggle, m_toggle.height() / 2.0, m_toggle.height() / 2.0);
  } else {
    painter.setPen(Qt::NoPen);
    painter.setBrush(kCreamDeep);
    painter.drawRoundedRect(m_toggle, m_toggle.height() / 2.0, m_toggle.height() / 2.0);
    painter.save();
    QPainterPath track;
    track.addRoundedRect(m_toggle, m_toggle.height() / 2.0, m_toggle.height() / 2.0);
    painter.setClipPath(track);
    QLinearGradient inset(m_toggle.topLeft(), QPointF(m_toggle.left(), m_toggle.top() + 10));
    inset.setColorAt(0.0, QColor(160, 142, 118, 70));
    inset.setColorAt(1.0, QColor(160, 142, 118, 0));
    painter.fillRect(m_toggle, inset);
    painter.restore();
  }
  const qreal knobD = m_toggle.height() - qRound(6 * s);
  const qreal knobX = m_autoOn ? m_toggle.right() - knobD - qRound(3 * s) : m_toggle.left() + qRound(3 * s);
  const QRectF knob(knobX, m_toggle.top() + (m_toggle.height() - knobD) / 2.0, knobD, knobD);
  painter.setBrush(QColor(160, 145, 125, 50));
  painter.drawEllipse(knob.translated(0.4, 1.0));
  painter.setBrush(QColor(255, 253, 250));
  painter.drawEllipse(knob);

  drawEmbossedText(painter, m_status, m_statusText, labelFont, Qt::AlignVCenter | Qt::AlignLeft);
  if (m_linked) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(kBlue);
    painter.drawEllipse(m_dot);
  }

  const bool disconnectPressed = m_pressed == Control::Disconnect;
  if (m_disconnectEnabled)
    paintRaised(painter, m_disconnect, 12, disconnectPressed, 0.85);
  else {
    painter.setPen(Qt::NoPen);
    painter.setBrush(kCream);
    painter.drawRoundedRect(m_disconnect, 12, 12);
  }
  drawEmbossedText(painter, m_disconnect, tr("断开"), labelFont, Qt::AlignCenter);

  if (!m_notice)
    return;

  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(120, 100, 78, 46));
  painter.drawRoundedRect(card, radius, radius);
  paintRaised(painter, m_noticeCard, 18, false, 1.0);
  const QRect title(
      m_noticeCard.left() + 16, m_noticeCard.top() + qRound(28 * s), m_noticeCard.width() - 32, qRound(32 * s)
  );
  const QRect body(m_noticeCard.left() + 20, title.bottom() + qRound(4 * s), m_noticeCard.width() - 40, qRound(28 * s));
  drawEmbossedText(
      painter, title, tr("即将支持"), uiFont(qRound(20 * s), QFont::Medium), Qt::AlignHCenter | Qt::AlignVCenter
  );
  drawEmbossedText(painter, body, tr("添加更多电脑还在准备中。"), labelFont, Qt::AlignHCenter | Qt::AlignVCenter);
  paintRaised(painter, m_noticeOk, 12, m_pressed == Control::NoticeOk, 0.8);
  drawEmbossedText(painter, m_noticeOk, tr("知道了"), labelFont, Qt::AlignCenter);
  paintWindowGlyph(painter, ControlGlyph::Min, m_min, m_hover == Control::Min);
  paintWindowGlyph(painter, zoomed ? ControlGlyph::Restore : ControlGlyph::Max, m_max, m_hover == Control::Max);
  paintWindowGlyph(painter, ControlGlyph::Close, m_close, m_hover == Control::Close);
}

void NeoHome::mousePressEvent(QMouseEvent *event)
{
  if (event->button() != Qt::LeftButton) {
    QWidget::mousePressEvent(event);
    return;
  }
  layoutChrome();
  const QPoint pos = event->position().toPoint();
#if !defined(Q_OS_WIN)
  const FrameHit frame = frameHitAt(pos);
  if (auto *handle = window() ? window()->windowHandle() : nullptr) {
    Qt::Edges edges;
    switch (frame) {
    case FrameHit::Left:
      edges = Qt::LeftEdge;
      break;
    case FrameHit::Right:
      edges = Qt::RightEdge;
      break;
    case FrameHit::Top:
      edges = Qt::TopEdge;
      break;
    case FrameHit::Bottom:
      edges = Qt::BottomEdge;
      break;
    case FrameHit::TopLeft:
      edges = Qt::LeftEdge | Qt::TopEdge;
      break;
    case FrameHit::TopRight:
      edges = Qt::RightEdge | Qt::TopEdge;
      break;
    case FrameHit::BottomLeft:
      edges = Qt::LeftEdge | Qt::BottomEdge;
      break;
    case FrameHit::BottomRight:
      edges = Qt::RightEdge | Qt::BottomEdge;
      break;
    case FrameHit::Caption:
      handle->startSystemMove();
      return;
    default:
      break;
    }
    if (edges != Qt::Edges{}) {
      handle->startSystemResize(edges);
      return;
    }
  }
#endif
  const Control hit = controlAt(pos);
  if (m_notice && hit != Control::NoticeOk && hit != Control::Min && hit != Control::Max && hit != Control::Close) {
    if (!m_noticeCard.contains(pos))
      m_notice = false;
    m_pressed = Control::None;
    update();
    return;
  }
  m_pressed = hit;
  update();
}

void NeoHome::mouseMoveEvent(QMouseEvent *event)
{
  layoutChrome();
  const Control hit = controlAt(event->position().toPoint());
  if (hit != m_hover) {
    m_hover = hit;
    update();
  }
}

void NeoHome::mouseReleaseEvent(QMouseEvent *event)
{
  if (event->button() != Qt::LeftButton) {
    QWidget::mouseReleaseEvent(event);
    return;
  }
  layoutChrome();
  const Control hit = controlAt(event->position().toPoint());
  const Control pressed = m_pressed;
  m_pressed = Control::None;
  update();
  if (hit != pressed)
    return;
  switch (pressed) {
  case Control::Min:
    Q_EMIT minimizeRequested();
    break;
  case Control::Max:
    Q_EMIT maximizeRequested();
    break;
  case Control::Close:
    Q_EMIT closeRequested();
    break;
  case Control::Gear:
    Q_EMIT settingsRequested();
    break;
  case Control::Toggle:
    Q_EMIT autoConnectToggled(!m_autoOn);
    break;
  case Control::Disconnect:
    Q_EMIT disconnectRequested();
    break;
  case Control::Add:
    m_notice = true;
    update();
    break;
  case Control::NoticeOk:
    m_notice = false;
    update();
    break;
  default:
    break;
  }
}

void NeoHome::mouseDoubleClickEvent(QMouseEvent *event)
{
#if !defined(Q_OS_WIN)
  layoutChrome();
  if (frameHitAt(event->position().toPoint()) == FrameHit::Caption) {
    Q_EMIT maximizeRequested();
    return;
  }
#else
  Q_UNUSED(event);
#endif
  QWidget::mouseDoubleClickEvent(event);
}

void NeoHome::leaveEvent(QEvent *event)
{
  m_hover = Control::None;
  m_pressed = Control::None;
  update();
  QWidget::leaveEvent(event);
}

} // namespace deskflow::gui
