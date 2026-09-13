/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QString>
#include <QStringList>

#include "gui/config/ScreenList.h"

class ScreenSetupView;
class ServerConfigDialog;

class ScreenSetupModel : public QAbstractTableModel
{
  Q_OBJECT

  friend class ScreenSetupView;
  friend class ServerConfigDialog;

public:
  ScreenSetupModel(ScreenList &screens, int numColumns, int numRows);

  static const QString &mimeType()
  {
    return m_MimeType;
  }
  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
  int rowCount() const
  {
    return m_NumRows;
  }
  int columnCount() const
  {
    return m_NumColumns;
  }
  int rowCount(const QModelIndex &) const override
  {
    return rowCount();
  }
  int columnCount(const QModelIndex &) const override
  {
    return columnCount();
  }
  Qt::DropActions supportedDropActions() const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  QStringList mimeTypes() const override;
  QMimeData *mimeData(const QModelIndexList &indexes) const override;
  bool isFull() const;

  /**
   * @brief 找出「已勾选双侧切入但当前布局下无效」的客户端屏名。
   *
   * 双侧切入要求该客户端在网格里与服务器同行且左右相邻（服务器用 left/right 同时指向它）。
   * 勾了但摆错位置（例如放在服务器上方/下方）时不会生成任何 links，这里用于在布局对话框
   * 里给出提示，避免用户以为设置没生效。
   */
  QStringList misconfiguredDualSideScreens() const;

Q_SIGNALS:
  void screensChanged();

protected:
  bool
  dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) override;
  const Screen &screen(const QModelIndex &index) const
  {
    return screen(index.column(), index.row());
  }
  Screen &screen(const QModelIndex &index)
  {
    return screen(index.column(), index.row());
  }
  const Screen &screen(int column, int row) const
  {
    return m_Screens[row * m_NumColumns + column];
  }
  Screen &screen(int column, int row)
  {
    return m_Screens[row * m_NumColumns + column];
  }
  void addScreen(const Screen &newScreen);

private:
  static constexpr int kMaxGridSize = 100;

  ScreenList &m_Screens;
  int m_NumColumns;
  int m_NumRows;

  static const QString m_MimeType;
};
