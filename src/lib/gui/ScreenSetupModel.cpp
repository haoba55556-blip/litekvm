/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ScreenSetupModel.h"

#include "common/Constants.h"
#include "gui/config/Screen.h"

#include <QIODevice>
#include <QIcon>
#include <QMimeData>

const QString ScreenSetupModel::m_MimeType = "application/x-deskflow-screen";

ScreenSetupModel::ScreenSetupModel(ScreenList &screens, int numColumns, int numRows)
    : QAbstractTableModel(nullptr),
      m_Screens(screens),
      m_NumColumns(numColumns),
      m_NumRows(numRows)
{
  // bound the grid so that multiplying columns by rows cannot overflow.
  if (m_NumColumns < 1 || m_NumColumns > kMaxGridSize || m_NumRows < 1 || m_NumRows > kMaxGridSize) {
    qCritical("grid size out of bounds: %d columns x %d rows", m_NumColumns, m_NumRows);
    m_NumColumns = kServerGridWidth;
    m_NumRows = kServerGridHeight;
  }

  const int span = m_NumColumns * m_NumRows;
  if (span > m_Screens.size()) {
    qCritical(
        "screen list too small for grid, screens: %lld, cells: %d", static_cast<long long>(m_Screens.size()), span
    );
    m_Screens.resize(span);
  }
}

QVariant ScreenSetupModel::data(const QModelIndex &index, int role) const
{
  if (!index.isValid() || index.row() > m_NumRows || index.row() < 0 || index.column() < 0 ||
      index.column() > m_NumColumns)
    return QVariant();

  if (screen(index).isNull())
    return QVariant();

  switch (role) {
  case Qt::DecorationRole:
    return screen(index).pixmap();

  case Qt::ToolTipRole: {
    QString tip = QString(tr("<center>Screen: <b>%1</b></center>"
                             "<br>Double click to edit settings"
                             "<br>Drag screen to the trashcan to remove it"))
                      .arg(screen(index).name());
    // 双侧切入：该屏同时接在网格相邻屏（通常是服务器）的左右两侧，鼠标可从任一侧滑入。
    if (screen(index).dualSide() && !screen(index).isServer())
      tip.append(tr("<br><b>Dual side</b>: linked to both sides of the neighbouring screen"));
    return tip;
  }

  case Qt::DisplayRole:
    return screen(index).name();

  default:
    break;
  }
  return QVariant();
}

Qt::ItemFlags ScreenSetupModel::flags(const QModelIndex &index) const
{
  if (!index.isValid() || index.row() >= m_NumRows || index.column() >= m_NumColumns)
    return Qt::NoItemFlags;

  if (!screen(index).isNull())
    return Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;

  return Qt::ItemIsDropEnabled;
}

Qt::DropActions ScreenSetupModel::supportedDropActions() const
{
  return Qt::MoveAction | Qt::CopyAction;
}

QStringList ScreenSetupModel::mimeTypes() const
{
  return QStringList() << m_MimeType;
}

QMimeData *ScreenSetupModel::mimeData(const QModelIndexList &indexes) const
{
  auto *pMimeData = new QMimeData();
  QByteArray encodedData;

  QDataStream stream(&encodedData, QIODevice::WriteOnly);

  for (const QModelIndex &index : indexes) {
    if (index.isValid())
      stream << index.column() << index.row() << screen(index);
  }

  pMimeData->setData(m_MimeType, encodedData);

  return pMimeData;
}

bool ScreenSetupModel::dropMimeData(
    const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent
)
{
  if (action == Qt::IgnoreAction)
    return true;

  if (!data->hasFormat(m_MimeType))
    return false;

  if (!parent.isValid() || row != -1 || column != -1)
    return false;

  QByteArray encodedData = data->data(m_MimeType);
  QDataStream stream(&encodedData, QIODevice::ReadOnly);

  int sourceColumn = -1;
  int sourceRow = -1;

  stream >> sourceColumn;
  stream >> sourceRow;

  const auto pColumn = parent.column();
  const auto pRow = parent.row();

  // don't drop screen onto itself
  if (sourceColumn == pColumn && sourceRow == pRow)
    return false;

  Screen droppedScreen;
  stream >> droppedScreen;

  if (auto oldScreen = Screen(screen(pColumn, pRow)); !oldScreen.isNull() && sourceColumn != -1 && sourceRow != -1) {
    // mark the screen so it isn't deleted after the dragndrop succeeded
    // see ScreenSetupView::startDrag()
    oldScreen.setSwapped(true);
    screen(sourceColumn, sourceRow) = oldScreen;
  }

  screen(pColumn, pRow) = droppedScreen;

  Q_EMIT screensChanged();

  return true;
}

void ScreenSetupModel::addScreen(const Screen &newScreen)
{
  m_Screens.addScreenByPriority(newScreen);
  Q_EMIT screensChanged();
}

bool ScreenSetupModel::isFull() const
{
  auto emptyScreen = std::ranges::find_if(m_Screens, [](const Screen &item) { return item.isNull(); });
  return (static_cast<QList<Screen>::const_iterator>(emptyScreen) == m_Screens.cend());
}

QStringList ScreenSetupModel::misconfiguredDualSideScreens() const
{
  QStringList misconfigured;

  for (int row = 0; row < m_NumRows; row++) {
    for (int column = 0; column < m_NumColumns; column++) {
      const Screen &candidate = screen(column, row);

      // 双侧切入只对客户端屏幕生效，服务器屏上的标记会被忽略。
      if (candidate.isNull() || candidate.isServer() || !candidate.dualSide())
        continue;

      const bool serverOnTheLeft = column > 0 && screen(column - 1, row).isServer();
      const bool serverOnTheRight = column + 1 < m_NumColumns && screen(column + 1, row).isServer();

      if (!serverOnTheLeft && !serverOnTheRight)
        misconfigured.append(candidate.name());
    }
  }

  return misconfigured;
}
