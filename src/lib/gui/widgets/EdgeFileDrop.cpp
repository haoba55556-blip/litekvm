/*
 * LiteKVM -- file drag across the screen edge.
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0
 */
#include "widgets/EdgeFileDrop.h"

#include "litekvm/ClipboardFileBridge.h"

#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QMimeData>
#include <QScreen>
#include <QTimer>
#include <QWidget>

namespace deskflow::gui {

namespace {

class EdgeStrip : public QWidget {
public:
  EdgeStrip(const QRect &geom, EdgeFileDrop *owner) : QWidget(nullptr), m_owner(owner)
  {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus |
                   Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setWindowOpacity(0.01);
    setAcceptDrops(true);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(geom);

    m_hold.setSingleShot(true);
    m_hold.setInterval(EdgeFileDrop::kHoldMs);
    connect(&m_hold, &QTimer::timeout, this, [this] {
      if (!m_pending.isEmpty() && m_owner)
        Q_EMIT m_owner->filesDragged(m_pending);
      m_pending.clear();
    });
  }

protected:
  void dragEnterEvent(QDragEnterEvent *event) override
  {
    m_pending = pathsFrom(event->mimeData());
    if (m_pending.isEmpty()) {
      event->ignore();
      return;
    }
    event->acceptProposedAction();
    m_hold.start();
  }

  void dragMoveEvent(QDragMoveEvent *event) override
  {
    if (m_pending.isEmpty())
      event->ignore();
    else
      event->acceptProposedAction();
  }

  void dragLeaveEvent(QDragLeaveEvent *event) override
  {
    Q_UNUSED(event)
    m_hold.stop();
    m_pending.clear();
  }

  void dropEvent(QDropEvent *event) override
  {
    m_hold.stop();
    if (m_pending.isEmpty())
      m_pending = pathsFrom(event->mimeData());
    if (m_pending.isEmpty()) {
      event->ignore();
      return;
    }
    event->acceptProposedAction();
    if (m_owner)
      Q_EMIT m_owner->filesDragged(m_pending);
    m_pending.clear();
  }

private:
  static QStringList pathsFrom(const QMimeData *mime)
  {
    const auto captured = litekvm::ClipboardFileBridge::capture(mime, litekvm::ClipboardFileBridge::imageDropDirectory());
    return captured.paths;
  }

  EdgeFileDrop *m_owner = nullptr;
  QTimer m_hold;
  QStringList m_pending;
};

} // namespace

EdgeFileDrop::EdgeFileDrop(QObject *parent) : QObject(parent)
{
}

EdgeFileDrop::~EdgeFileDrop()
{
  destroyStrips();
}

void EdgeFileDrop::setEnabled(bool on)
{
  if (m_enabled == on)
    return;
  m_enabled = on;
  if (!on) {
    destroyStrips();
    return;
  }
  rebuild();
  if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
    connect(app, &QGuiApplication::screenAdded, this, [this](QScreen *) { rebuild(); }, Qt::UniqueConnection);
    connect(app, &QGuiApplication::screenRemoved, this, [this](QScreen *) { rebuild(); }, Qt::UniqueConnection);
  }
}

QWidget *EdgeFileDrop::dragSource() const
{
  for (const auto &strip : m_strips) {
    if (strip)
      return strip;
  }
  return nullptr;
}

void EdgeFileDrop::destroyStrips()
{
  const auto strips = m_strips;
  m_strips.clear();
  for (const auto &strip : strips) {
    if (!strip)
      continue;
    strip->hide();
    delete strip.data();
  }
}

void EdgeFileDrop::rebuild()
{
  destroyStrips();
  if (!m_enabled)
    return;

  const auto screens = QGuiApplication::screens();
  for (QScreen *screen : screens) {
    if (!screen)
      continue;
    const QRect g = screen->geometry();
    if (g.width() < kStripPx * 4 || g.height() < kStripPx * 4)
      continue;
    const QList<QRect> edges = {
        QRect(g.left(), g.top(), kStripPx, g.height()),
        QRect(g.right() - kStripPx + 1, g.top(), kStripPx, g.height()),
        QRect(g.left(), g.top(), g.width(), kStripPx),
        QRect(g.left(), g.bottom() - kStripPx + 1, g.width(), kStripPx),
    };
    for (const QRect &edge : edges) {
      auto *strip = new EdgeStrip(edge, this);
      strip->QObject::setParent(this);
      strip->show();
      m_strips.append(strip);
    }
  }
}

} // namespace deskflow::gui
