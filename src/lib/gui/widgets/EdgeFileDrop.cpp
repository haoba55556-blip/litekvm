/*
 * LiteKVM -- file drag across the screen edge.
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "widgets/EdgeFileDrop.h"

#include "litekvm/ClipboardFileBridge.h"
#include "litekvm/DragEdgeGate.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QMimeData>
#include <QScreen>
#include <QShowEvent>
#include <QWidget>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace deskflow::gui {

namespace {

class EdgeStrip : public QWidget
{
public:
  EdgeStrip(const QRect &geom, EdgeFileDrop *owner) : QWidget(nullptr), m_owner(owner)
  {
    setWindowFlags(
        Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus |
        Qt::NoDropShadowWindowHint
    );
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_AlwaysStackOnTop, true);
#ifdef Q_OS_WIN
    // Per-pixel translucency (WA_TranslucentBackground) is skipped by
    // WindowFromPoint, so OLE never delivers DragEnter. showEvent() applies a
    // uniform layered alpha instead: the whole rect hit-tests, the pixels do not.
    setAttribute(Qt::WA_TranslucentBackground, false);
    setStyleSheet(QStringLiteral("background: black;"));
#else
    setAttribute(Qt::WA_TranslucentBackground, true);
    setStyleSheet(QStringLiteral("background-color: rgba(0, 0, 0, 1);"));
#endif
    setAcceptDrops(true);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(geom);
  }

protected:
  void showEvent(QShowEvent *event) override
  {
    QWidget::showEvent(event);
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
      return;
    const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST);
    SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
    SetWindowPos(
        hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW
    );
#endif
  }

  void dragEnterEvent(QDragEnterEvent *event) override
  {
    if (m_owner != nullptr && m_owner->passThrough()) {
      event->ignore();
      return;
    }
    const DragRead read = readDrag(event->mimeData());
    litekvm::ClipboardFileBridge::traceStage(QStringLiteral("DragEnter"), read.hint);
    if (m_owner == nullptr) {
      event->ignore();
      return;
    }
    if (!read.paths.isEmpty()) {
      event->acceptProposedAction();
      // Start now. dragLeave follows within milliseconds when the screen switches.
      m_owner->beginDrag(read.paths);
      return;
    }
    // Keep the drop target so a later DragMove can read CF_HDROP. Ignoring
    // here is what made a 7s edge hold produce no transfer.
    if (read.delayed) {
      event->acceptProposedAction();
      return;
    }
    event->ignore();
  }

  void dragMoveEvent(QDragMoveEvent *event) override
  {
    if (m_owner != nullptr && m_owner->passThrough()) {
      event->ignore();
      return;
    }
    event->acceptProposedAction();
    if (m_owner == nullptr)
      return;
    const QStringList paths = pathsFrom(event->mimeData());
    if (!paths.isEmpty())
      m_owner->beginDrag(paths);
  }

  void dragLeaveEvent(QDragLeaveEvent *event) override
  {
    Q_UNUSED(event)
    if (m_owner)
      m_owner->noteDragLeft();
  }

  void dropEvent(QDropEvent *event) override
  {
    if (m_owner != nullptr && m_owner->passThrough()) {
      event->ignore();
      return;
    }
    const QStringList paths = pathsFrom(event->mimeData());
    if (paths.isEmpty() || m_owner == nullptr) {
      event->ignore();
      return;
    }
    event->acceptProposedAction();
    m_owner->beginDrag(paths);
  }

private:
  struct DragRead
  {
    QStringList paths;
    bool delayed = false;
    QString hint;
  };

  static DragRead readDrag(const QMimeData *mime)
  {
    DragRead out;
    if (mime == nullptr) {
      out.hint = QStringLiteral("mime=null");
      return out;
    }
    out.paths = pathsFrom(mime);
    QStringList formats;
    for (const QString &format : mime->formats()) {
      if (format.contains(QLatin1String("CF_HDROP"), Qt::CaseInsensitive) ||
          format.contains(QLatin1String("FileName"), Qt::CaseInsensitive) ||
          format.contains(QLatin1String("uri-list"), Qt::CaseInsensitive)) {
        formats.append(format);
        if (out.paths.isEmpty())
          out.delayed = true;
      }
    }
    // hasUrls() is true for an empty text/uri-list while OLE is still rendering.
    if (out.paths.isEmpty() && mime->hasUrls() && mime->urls().isEmpty())
      out.delayed = true;
    out.hint = QStringLiteral("hasUrls=%1 urls=%2 paths=%3 delayed=%4 formats=%5")
                   .arg(mime->hasUrls() ? 1 : 0)
                   .arg(mime->urls().size())
                   .arg(out.paths.size())
                   .arg(out.delayed ? 1 : 0)
                   .arg(formats.join(QLatin1Char('|')));
    return out;
  }

  static QStringList pathsFrom(const QMimeData *mime)
  {
    const auto captured =
        litekvm::ClipboardFileBridge::capture(mime, litekvm::ClipboardFileBridge::imageDropDirectory());
    return captured.paths;
  }

  EdgeFileDrop *m_owner = nullptr;
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

void EdgeFileDrop::beginDrag(const QStringList &paths)
{
  const bool start = m_gate.onEnter(paths, QDateTime::currentMSecsSinceEpoch());
  litekvm::ClipboardFileBridge::traceStage(
      QStringLiteral("beginDrag"),
      QStringLiteral("paths=%1 start=%2 first=%3").arg(paths.size()).arg(start ? "yes" : "no").arg(paths.value(0))
  );
  if (start)
    Q_EMIT filesDragged(paths);
}

void EdgeFileDrop::noteDragLeft()
{
  m_gate.onLeave();
}

void EdgeFileDrop::setPassThrough(bool on)
{
  m_passThrough = on;
  for (const auto &strip : m_strips) {
    if (!strip)
      continue;
    strip->setAcceptDrops(!on);
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(strip->winId());
    if (hwnd == nullptr)
      continue;
    LONG_PTR extra = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (on)
      extra |= WS_EX_TRANSPARENT;
    else
      extra &= ~LONG_PTR(WS_EX_TRANSPARENT);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, extra);
#endif
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

int EdgeFileDrop::stripCount() const
{
  int count = 0;
  for (const auto &strip : m_strips) {
    if (strip)
      ++count;
  }
  return count;
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
      // Do not QObject::setParent() onto this QObject. These are top-level
      // tool windows; a non-widget QObject parent makes QWidget::show() crash
      // in createWinId() (Qt rejects that parent). destroyStrips() owns them.
      strip->show();
      m_strips.append(strip);
    }
  }
  litekvm::ClipboardFileBridge::traceStage(QStringLiteral("strips"), QStringLiteral("count=%1").arg(m_strips.size()));
}

} // namespace deskflow::gui
