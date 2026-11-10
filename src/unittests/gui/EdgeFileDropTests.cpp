// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Edge strips stay unbuilt until setEnabled(true). MainWindow used to apply
// the file-clipboard setting before EdgeFileDrop existed, so startup left
// m_enabled false and no HWND was created for OLE DragEnter.
#include "widgets/EdgeFileDrop.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QtTest>

using deskflow::gui::EdgeFileDrop;

namespace {

int expectedStrips()
{
  int screens = 0;
  for (QScreen *screen : QGuiApplication::screens()) {
    if (!screen)
      continue;
    const QRect g = screen->geometry();
    if (g.width() < EdgeFileDrop::kStripPx * 4 || g.height() < EdgeFileDrop::kStripPx * 4)
      continue;
    ++screens;
  }
  return screens * 4;
}

} // namespace

class EdgeFileDropTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void stripsStayDownUntilEnabled()
  {
    EdgeFileDrop drop;
    QVERIFY(!drop.isEnabled());
    QCOMPARE(drop.stripCount(), 0);
    QVERIFY(drop.dragSource() == nullptr);

    const int expected = expectedStrips();
    if (expected == 0)
      QSKIP("no screen large enough for an edge strip");

    drop.setEnabled(true);
    QVERIFY(drop.isEnabled());
    QCOMPARE(drop.stripCount(), expected);
    QVERIFY(drop.dragSource() != nullptr);

    // A second apply (settings saved again) must not stack another set of strips.
    drop.setEnabled(true);
    QCOMPARE(drop.stripCount(), expected);
  }

  void disablingRemovesStrips()
  {
    EdgeFileDrop drop;
    if (expectedStrips() == 0)
      QSKIP("no screen large enough for an edge strip");

    drop.setEnabled(true);
    drop.setEnabled(false);
    QVERIFY(!drop.isEnabled());
    QCOMPARE(drop.stripCount(), 0);
    QVERIFY(drop.dragSource() == nullptr);
  }

  void stripsStayHiddenUntilArmed()
  {
    EdgeFileDrop drop;
    if (expectedStrips() == 0)
      QSKIP("no screen large enough for an edge strip");

    drop.setEnabled(true);
    QVERIFY(drop.dragSource() != nullptr);
    QVERIFY(!drop.stripsArmed());
    QVERIFY(!drop.passThrough());
    QVERIFY(!drop.dragSource()->isVisible());

    drop.setStripsArmed(true);
    QVERIFY(drop.dragSource()->isVisible());

    // A remote drop hides the strips so they are not the window under the cursor.
    drop.setPassThrough(true);
    QVERIFY(!drop.dragSource()->isVisible());
    drop.setPassThrough(false);
    QVERIFY(drop.dragSource()->isVisible());

    drop.setStripsArmed(false);
    QVERIFY(!drop.stripsArmed());
    QVERIFY(!drop.dragSource()->isVisible());
  }
};

QTEST_MAIN(EdgeFileDropTests)
#include "EdgeFileDropTests.moc"
