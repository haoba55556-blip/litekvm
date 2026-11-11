// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only

#include "NeoHome.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on
#endif

namespace deskflow::gui {

namespace {

// Shadow and corner radius track the two-thirds stage so the card, not only
// the drawings, is the smaller window. The resize border stays wide enough to grab.
constexpr int kShadow = 11;
constexpr int kRadius = 15;
constexpr int kBorder = 6;
constexpr qreal kDesignW = 1248.0;
constexpr qreal kDesignH = 688.0;
// Design-pixel gap between 本机, 笔记本, and the 添加电脑 slot.
// At the default 2/3 scale this is 120px. The previous row used a 70px arrow
// slot and a 26px gap in front of the add slot.
constexpr qreal kDeviceGap = 180.0;

const QColor kPage(244, 235, 224);
const QColor kRaised(253, 248, 242);
const QColor kWell(214, 198, 178);
const QColor kInk(156, 134, 110);
const QColor kInkShadow(98, 76, 56);
const QColor kInkLight(255, 252, 247);
const QColor kBlue(59, 124, 245);
const QColor kDash(168, 146, 122);
const QColor kChrome(146, 126, 104);

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
  const qreal px = font.pixelSize() > 0 ? font.pixelSize() : 16.0;
  const qreal offset = qBound(1.05, px * 0.072, 2.15);
  painter.setPen(QColor(kInkShadow.red(), kInkShadow.green(), kInkShadow.blue(), 72));
  painter.drawText(rect.translated(offset * 1.05, offset * 1.45), flags, text);
  painter.setPen(QColor(kInkShadow.red(), kInkShadow.green(), kInkShadow.blue(), 168));
  painter.drawText(rect.translated(offset * 0.42, offset * 0.62), flags, text);
  painter.setPen(QColor(kInkLight.red(), kInkLight.green(), kInkLight.blue(), 235));
  painter.drawText(rect.translated(-offset * 0.7, -offset * 0.88), flags, text);
  painter.setPen(kInk);
  painter.drawText(rect, flags, text);
}

void paintCastShadow(QPainter &painter, const QRectF &rect, qreal radius, qreal depth)
{
  painter.setPen(Qt::NoPen);
  for (int i = 11; i >= 1; --i) {
    const qreal spread = i * 1.05 * depth;
    const int alpha = 3 + (11 - i) * 5;
    painter.setBrush(QColor(108, 82, 58, alpha));
    const qreal dx = 1.55 * depth;
    const qreal dy = 2.15 * depth;
    painter.drawRoundedRect(
        rect.adjusted(dx * 0.15, dy * 0.2, spread * 0.95 + dx, spread * 1.2 + dy), radius + spread * 0.22,
        radius + spread * 0.22
    );
  }
  for (int i = 7; i >= 1; --i) {
    const qreal spread = i * 0.72 * depth;
    const int alpha = 12 + (7 - i) * 12;
    painter.setBrush(QColor(255, 253, 248, alpha));
    const qreal dx = -1.25 * depth;
    const qreal dy = -1.35 * depth;
    painter.drawRoundedRect(
        rect.adjusted(-spread + dx, -spread + dy, dx * 0.15, dy * 0.1), radius + spread * 0.2, radius + spread * 0.2
    );
  }
}

void paintRaised(QPainter &painter, const QRectF &rect, qreal radius, bool pressed, qreal depth)
{
  painter.setPen(Qt::NoPen);
  if (!pressed)
    paintCastShadow(painter, rect, radius, depth);
  painter.setBrush(pressed ? QColor(236, 226, 214) : kRaised);
  painter.drawRoundedRect(rect, radius, radius);

  painter.save();
  QPainterPath clip;
  clip.addRoundedRect(rect, radius, radius);
  painter.setClipPath(clip);
  QLinearGradient sheen(rect.topLeft(), QPointF(rect.left(), rect.top() + rect.height() * 0.48));
  sheen.setColorAt(0.0, QColor(255, 255, 255, pressed ? 26 : 128));
  sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
  painter.fillRect(rect, sheen);
  QLinearGradient shade(QPointF(rect.left(), rect.bottom() - rect.height() * 0.34), rect.bottomLeft());
  shade.setColorAt(0.0, QColor(160, 132, 102, 0));
  shade.setColorAt(1.0, QColor(148, 118, 88, pressed ? 70 : 42));
  painter.fillRect(rect, shade);
  if (pressed) {
    QLinearGradient inset(rect.topLeft(), QPointF(rect.left(), rect.top() + qMax(12.0, rect.height() * 0.45)));
    inset.setColorAt(0.0, QColor(120, 96, 72, 90));
    inset.setColorAt(1.0, QColor(120, 96, 72, 0));
    painter.fillRect(rect, inset);
  }
  painter.restore();

  QLinearGradient rim(rect.topLeft(), rect.bottomRight());
  rim.setColorAt(0.0, QColor(255, 255, 255, pressed ? 70 : 220));
  rim.setColorAt(0.38, QColor(255, 252, 247, 40));
  rim.setColorAt(1.0, QColor(132, 106, 80, pressed ? 50 : 130));
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(QBrush(rim), qMax(1.05, qMin(rect.width(), rect.height()) * 0.035)));
  const qreal inset = 0.7;
  painter.drawRoundedRect(
      rect.adjusted(inset, inset, -inset, -inset), qMax(1.0, radius - inset), qMax(1.0, radius - inset)
  );
}

void paintScreen(QPainter &painter, const QRectF &screen)
{
  const qreal radius = qMax(4.0, qMin(screen.width(), screen.height()) * 0.09);
  QLinearGradient glass(screen.topLeft(), screen.bottomRight());
  glass.setColorAt(0.0, QColor(255, 254, 251));
  glass.setColorAt(0.34, QColor(232, 222, 208));
  glass.setColorAt(1.0, QColor(176, 158, 136));
  painter.setPen(Qt::NoPen);
  painter.setBrush(glass);
  painter.drawRoundedRect(screen, radius, radius);

  painter.save();
  QPainterPath clip;
  clip.addRoundedRect(screen, radius, radius);
  painter.setClipPath(clip);
  QLinearGradient gloss(screen.topLeft(), QPointF(screen.left(), screen.top() + screen.height() * 0.46));
  gloss.setColorAt(0.0, QColor(255, 255, 255, 210));
  gloss.setColorAt(0.55, QColor(255, 255, 255, 24));
  gloss.setColorAt(1.0, QColor(255, 255, 255, 0));
  painter.fillRect(screen, gloss);
  QLinearGradient shade(screen.topRight(), screen.bottomRight());
  shade.setColorAt(0.45, QColor(140, 112, 84, 0));
  shade.setColorAt(1.0, QColor(120, 92, 64, 80));
  painter.fillRect(screen, shade);
  painter.restore();

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(QColor(150, 126, 100, 160), qMax(1.0, screen.height() * 0.018)));
  painter.drawRoundedRect(screen.adjusted(0.4, 0.4, -0.4, -0.4), radius, radius);
}

void paintMonitor(QPainter &painter, const QRectF &box)
{
  const qreal unit = qMin(box.width() / 127.0, box.height() / 105.0);
  const QRectF bezel(box.left(), box.top(), box.width(), box.height() * 0.72);
  paintRaised(painter, bezel, 11.0 * unit, false, 1.45);
  paintScreen(painter, bezel.adjusted(8.0 * unit, 7.0 * unit, -8.0 * unit, -15.0 * unit));

  const QRectF dot(bezel.center().x() - 2.5 * unit, bezel.bottom() - 11.0 * unit, 5.0 * unit, 5.0 * unit);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(120, 98, 76, 90));
  painter.drawEllipse(dot.translated(0.4 * unit, 0.6 * unit));
  painter.setBrush(QColor(176, 156, 134));
  painter.drawEllipse(dot);
  painter.setBrush(kInkLight);
  painter.drawEllipse(QRectF(dot.left() + 1.1 * unit, dot.top() + 0.8 * unit, 1.6 * unit, 1.6 * unit));

  const QRectF neck(box.center().x() - 6.0 * unit, bezel.bottom() - unit, 12.0 * unit, box.height() * 0.15);
  paintRaised(painter, neck, 2.4 * unit, false, 0.6);
  const QRectF foot(box.center().x() - box.width() * 0.24, neck.bottom() - 2.0 * unit, box.width() * 0.48, 7.5 * unit);
  paintRaised(painter, foot, 3.6 * unit, false, 1.05);
}

void paintLaptop(QPainter &painter, const QRectF &box)
{
  const qreal unit = qMin(box.width() / 143.0, box.height() / 99.0);
  const QRectF screen(box.left() + box.width() * 0.08, box.top(), box.width() * 0.84, box.height() * 0.66);
  paintRaised(painter, screen, 8.0 * unit, false, 1.35);
  paintScreen(painter, screen.adjusted(6.5 * unit, 5.5 * unit, -6.5 * unit, -8.5 * unit));

  const QRectF base(box.left(), screen.bottom() - 3.5 * unit, box.width(), box.height() * 0.24);
  paintRaised(painter, base, 5.5 * unit, false, 1.15);
  painter.setPen(QPen(QColor(255, 252, 247, 180), qMax(1.0, 1.1 * unit)));
  painter.drawLine(
      QPointF(base.left() + 8.0 * unit, base.top() + 0.4 * unit),
      QPointF(base.right() - 8.0 * unit, base.top() + 0.4 * unit)
  );
  painter.setPen(QPen(QColor(150, 126, 102, 170), qMax(1.0, 1.05 * unit)));
  painter.drawLine(
      QPointF(base.left() + 8.0 * unit, base.top() + 1.5 * unit),
      QPointF(base.right() - 8.0 * unit, base.top() + 1.5 * unit)
  );
}

void paintArrow(QPainter &painter, const QRectF &rect)
{
  const qreal y = rect.center().y();
  const qreal x0 = rect.left() + 4.0;
  const qreal x1 = rect.right() - 4.0;
  auto stroke = [&](const QColor &color, qreal dy, qreal width) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(x0, y + dy), QPointF(x1, y + dy));
    const qreal wing = 7.2;
    painter.drawLine(QPointF(x0, y + dy), QPointF(x0 + wing, y + dy - 3.8));
    painter.drawLine(QPointF(x0, y + dy), QPointF(x0 + wing, y + dy + 3.8));
    painter.drawLine(QPointF(x1, y + dy), QPointF(x1 - wing, y + dy - 3.8));
    painter.drawLine(QPointF(x1, y + dy), QPointF(x1 - wing, y + dy + 3.8));
  };
  stroke(QColor(255, 252, 247, 220), -1.15, 1.7);
  stroke(QColor(110, 86, 64, 100), 1.35, 1.8);
  stroke(QColor(146, 122, 96), 0.0, 1.65);
}

enum class ControlGlyph
{
  Min,
  Max,
  Restore,
  Close
};

void paintGear(QPainter &painter, const QRectF &bounds)
{
  const QPointF center = bounds.center();
  const qreal unit = bounds.width() / 50.0;
  auto draw = [&](const QColor &color, const QPointF &offset) {
    painter.save();
    painter.translate(center + offset);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (int i = 0; i < 8; ++i) {
      painter.rotate(45.0);
      painter.drawRoundedRect(QRectF(-2.3 * unit, -13.6 * unit, 4.6 * unit, 5.4 * unit), 1.2 * unit, 1.2 * unit);
    }
    painter.drawEllipse(QPointF(0, 0), 7.4 * unit, 7.4 * unit);
    painter.restore();
  };
  draw(QColor(kInkShadow.red(), kInkShadow.green(), kInkShadow.blue(), 130), QPointF(1.15 * unit, 1.45 * unit));
  draw(QColor(255, 252, 247, 210), QPointF(-0.85 * unit, -1.0 * unit));
  draw(QColor(176, 156, 134), QPointF(0, 0));
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(140, 116, 92, 70));
  painter.drawEllipse(QRectF(center.x() - 3.2 * unit, center.y() - 2.6 * unit, 6.4 * unit, 5.4 * unit));
  painter.setBrush(kRaised);
  painter.drawEllipse(QRectF(center.x() - 2.7 * unit, center.y() - 2.7 * unit, 5.4 * unit, 5.4 * unit));
}

void paintWindowGlyph(QPainter &painter, ControlGlyph glyph, const QRectF &rect, bool hovered)
{
  if (hovered) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 140));
    painter.drawEllipse(rect.adjusted(4, 3, -4, -3));
  }
  const QPointF c = rect.center();
  auto stroke = [&](const QColor &color, const QPointF &shift, qreal width) {
    painter.save();
    painter.translate(shift);
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (glyph == ControlGlyph::Min) {
      painter.drawLine(QPointF(c.x() - 6.0, c.y() + 1.0), QPointF(c.x() + 6.0, c.y() + 1.0));
    } else if (glyph == ControlGlyph::Close) {
      painter.drawLine(QPointF(c.x() - 5.2, c.y() - 5.2), QPointF(c.x() + 5.2, c.y() + 5.2));
      painter.drawLine(QPointF(c.x() + 5.2, c.y() - 5.2), QPointF(c.x() - 5.2, c.y() + 5.2));
    } else if (glyph == ControlGlyph::Restore) {
      painter.drawRoundedRect(QRectF(c.x() - 6.2, c.y() - 3.2, 8.2, 8.2), 1.4, 1.4);
      painter.drawLine(QPointF(c.x() - 2.4, c.y() - 3.2), QPointF(c.x() - 2.4, c.y() - 6.0));
      painter.drawLine(QPointF(c.x() - 2.4, c.y() - 6.0), QPointF(c.x() + 6.0, c.y() - 6.0));
      painter.drawLine(QPointF(c.x() + 6.0, c.y() - 6.0), QPointF(c.x() + 6.0, c.y() + 2.2));
      painter.drawLine(QPointF(c.x() + 6.0, c.y() + 2.2), QPointF(c.x() + 2.0, c.y() + 2.2));
    } else {
      painter.drawRoundedRect(QRectF(c.x() - 5.5, c.y() - 5.5, 11.0, 11.0), 1.6, 1.6);
    }
    painter.restore();
  };
  stroke(QColor(kInkShadow.red(), kInkShadow.green(), kInkShadow.blue(), 120), QPointF(0.9, 1.15), 1.5);
  stroke(QColor(255, 252, 247, 210), QPointF(-0.7, -0.85), 1.55);
  stroke(kChrome, QPointF(0, 0), 1.5);
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

// Runs the platform move loop while the left button is still down. Windows uses
// a synchronous caption drag so a frameless, translucent window actually moves;
// the posted startSystemMove path returns before Qt has finished the press.
bool beginWindowMove(QWidget *top)
{
  if (!top)
    return false;
  QWindow *handle = top->windowHandle();
  if (!handle)
    return false;
#if defined(Q_OS_WIN)
  const auto hwnd = reinterpret_cast<HWND>(handle->winId());
  if (!hwnd)
    return false;
  // Synchronous SC_DRAGMOVE. Qt's startSystemMove posts this and returns, and
  // the frameless window then never picks the drag up. ReleaseCapture only
  // drops this window's mouse grab so the system move loop can run; it is not
  // a leave-screen capture.
  ReleaseCapture();
  SendMessage(hwnd, WM_SYSCOMMAND, 0xF012 /* SC_DRAGMOVE */, 0);
  return true;
#else
  return handle->startSystemMove();
#endif
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
  // Two thirds of the previous 1280×720 home stage. 854×481 keeps the card
  // inside the shadow at exactly 832×459, which is 2/3 of the 1248×688 design.
  return {854, 481};
}

QSize NeoHome::minimumSizeHint() const
{
  return {720, 420};
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
  m_scale = qBound(0.5, qMin(sx, sy), 1.2);
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
  const int deviceGap = qRound(kDeviceGap * s);
  const int arrowW = deviceGap;
  const int laptopW = qRound(214 * s);
  const int laptopH = qRound(148 * s);
  const int addW = qRound(160 * s);
  const int addH = qRound(168 * s);
  const int clusterW = monitorW + arrowW + laptopW + deviceGap + addW;
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
  m_add = QRect(m_laptop.right() + deviceGap, rowBottom - addH, addW, addH);

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

  if (!full) {
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
  }

  // Header, empty cream, and the non-interactive device art move the window.
  // Buttons stay client area so the toggle, disconnect, gear, add slot, and
  // caption buttons still click. The notice overlay keeps its own clicks.
  if (m_notice || controlAt(pos) != Control::None || m_disconnect.contains(pos))
    return FrameHit::Client;
  return FrameHit::Caption;
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
    for (int i = 16; i >= 1; --i) {
      QColor shadow(112, 86, 60, 6 + (16 - i) * 2);
      painter.setPen(Qt::NoPen);
      painter.setBrush(shadow);
      const qreal grow = i * 0.9;
      painter.drawRoundedRect(card.adjusted(-grow * 0.12, grow * 0.2, grow * 0.5, grow * 0.95), radius, radius);
    }
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(kPage);
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

  const QRectF addBox = QRectF(m_add).adjusted(1, 1, -1, -1);
  paintCastShadow(painter, addBox, 14, 0.85);
  auto dash = [&](const QColor &color, const QPointF &shift, qreal width) {
    painter.save();
    painter.translate(shift);
    QPen pen(color, width, Qt::CustomDashLine, Qt::RoundCap, Qt::RoundJoin);
    pen.setDashPattern({2.4, 2.1});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(addBox, 14, 14);
    painter.restore();
  };
  dash(QColor(255, 252, 247, 210), QPointF(-0.8, -0.9), qMax(1.35, 1.55 * s));
  dash(QColor(kInkShadow.red(), kInkShadow.green(), kInkShadow.blue(), 110), QPointF(0.7, 0.95), qMax(1.35, 1.55 * s));
  dash(kDash, QPointF(0, 0), qMax(1.35, 1.6 * s));
  const qreal plusD = m_add.height() * 0.30;
  const QRectF plusCircle(m_add.center().x() - plusD / 2.0, m_add.top() + m_add.height() * 0.20, plusD, plusD);
  paintRaised(painter, plusCircle, plusCircle.width() / 2.0, m_pressed == Control::Add, 0.95);
  painter.setPen(Qt::NoPen);
  const QPointF plusCenter = plusCircle.center();
  const qreal arm = plusD * 0.18;
  const qreal thick = qMax(2.0, plusD * 0.075);
  auto plusBar = [&](const QColor &color, const QPointF &shift) {
    painter.setBrush(color);
    painter.drawRoundedRect(
        QRectF(plusCenter.x() - thick / 2 + shift.x(), plusCenter.y() - arm + shift.y(), thick, arm * 2), 1.1, 1.1
    );
    painter.drawRoundedRect(
        QRectF(plusCenter.x() - arm + shift.x(), plusCenter.y() - thick / 2 + shift.y(), arm * 2, thick), 1.1, 1.1
    );
  };
  plusBar(QColor(kInkShadow.red(), kInkShadow.green(), kInkShadow.blue(), 140), QPointF(0.7, 0.9));
  plusBar(QColor(255, 252, 247, 220), QPointF(-0.55, -0.7));
  plusBar(QColor(176, 154, 130), QPointF(0, 0));
  const QRect addLabel(m_add.left() + 8, int(plusCircle.bottom()) + qRound(8 * s), m_add.width() - 16, qRound(28 * s));
  drawEmbossedText(painter, addLabel, tr("添加电脑"), labelFont, Qt::AlignHCenter | Qt::AlignTop);

  paintRaised(painter, m_gear, m_gear.width() / 2.0, m_pressed == Control::Gear, 1.15);
  paintGear(painter, m_gear);

  if (m_autoOn) {
    QLinearGradient blue(m_toggle.topLeft(), m_toggle.bottomLeft());
    blue.setColorAt(0.0, QColor(92, 150, 255));
    blue.setColorAt(1.0, QColor(47, 111, 236));
    painter.setPen(Qt::NoPen);
    painter.setBrush(blue);
    painter.drawRoundedRect(m_toggle, m_toggle.height() / 2.0, m_toggle.height() / 2.0);
    painter.setPen(QPen(QColor(255, 255, 255, 90), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(
        QRectF(m_toggle).adjusted(1.0, 1.0, -1.0, -1.2), m_toggle.height() / 2.0, m_toggle.height() / 2.0
    );
  } else {
    painter.setPen(Qt::NoPen);
    painter.setBrush(kWell);
    painter.drawRoundedRect(m_toggle, m_toggle.height() / 2.0, m_toggle.height() / 2.0);
    painter.save();
    QPainterPath track;
    track.addRoundedRect(m_toggle, m_toggle.height() / 2.0, m_toggle.height() / 2.0);
    painter.setClipPath(track);
    QLinearGradient inset(m_toggle.topLeft(), QPointF(m_toggle.left(), m_toggle.top() + m_toggle.height() * 0.55));
    inset.setColorAt(0.0, QColor(110, 86, 62, 110));
    inset.setColorAt(1.0, QColor(110, 86, 62, 0));
    painter.fillRect(m_toggle, inset);
    painter.restore();
  }
  const qreal knobD = m_toggle.height() - qRound(6 * s);
  const qreal knobX = m_autoOn ? m_toggle.right() - knobD - qRound(3 * s) : m_toggle.left() + qRound(3 * s);
  const QRectF knob(knobX, m_toggle.top() + (m_toggle.height() - knobD) / 2.0, knobD, knobD);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(120, 96, 72, 80));
  painter.drawEllipse(knob.translated(0.5, 1.3));
  painter.setBrush(QColor(255, 253, 250));
  painter.drawEllipse(knob);
  painter.setBrush(QColor(255, 255, 255, 180));
  painter.drawEllipse(knob.adjusted(knobD * 0.22, knobD * 0.16, -knobD * 0.48, -knobD * 0.55));

  drawEmbossedText(painter, m_status, m_statusText, labelFont, Qt::AlignVCenter | Qt::AlignLeft);
  if (m_linked) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(90, 70, 50, 55));
    painter.drawEllipse(QRectF(m_dot).adjusted(-0.6, 0.5, 1.0, 1.8));
    painter.setBrush(kBlue);
    painter.drawEllipse(m_dot);
  }

  const bool disconnectPressed = m_pressed == Control::Disconnect && m_disconnectEnabled;
  paintRaised(painter, m_disconnect, qMax(10.0, 12.0 * s), disconnectPressed, m_disconnectEnabled ? 1.05 : 0.55);
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
  const FrameHit frame = frameHitAt(pos);
  if (QWindow *handle = window() ? window()->windowHandle() : nullptr) {
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
      if (beginWindowMove(window()))
        return;
      if (QWidget *top = window()) {
        m_draggingWindow = true;
        m_dragOffset = event->globalPosition().toPoint() - top->frameGeometry().topLeft();
      }
      return;
    default:
      break;
    }
    if (edges != Qt::Edges{}) {
      handle->startSystemResize(edges);
      return;
    }
  } else if (frame == FrameHit::Caption) {
    if (QWidget *top = window()) {
      m_draggingWindow = true;
      m_dragOffset = event->globalPosition().toPoint() - top->frameGeometry().topLeft();
    }
    return;
  }
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
  if (m_draggingWindow && (event->buttons() & Qt::LeftButton)) {
    if (QWidget *top = window())
      top->move(event->globalPosition().toPoint() - m_dragOffset);
    return;
  }
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
  const bool wasDragging = m_draggingWindow;
  m_draggingWindow = false;
  if (wasDragging)
    return;
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
  layoutChrome();
  const QPoint pos = event->position().toPoint();
  const bool header = pos.y() <= m_card.top() + qRound(52 * m_scale);
  if (header && frameHitAt(pos) == FrameHit::Caption) {
    Q_EMIT maximizeRequested();
    return;
  }
  QWidget::mouseDoubleClickEvent(event);
}

void NeoHome::leaveEvent(QEvent *event)
{
  m_hover = Control::None;
  m_pressed = Control::None;
  m_draggingWindow = false;
  update();
  QWidget::leaveEvent(event);
}

} // namespace deskflow::gui
