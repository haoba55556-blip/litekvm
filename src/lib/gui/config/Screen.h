/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ScreenConfig.h"

#include "common/QSettingsProxy.h"

#include <QIcon>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QStringList>

class QSettings;
class QTextStream;
class ScreenSettingsDialog;

class Screen : public ScreenConfig
{
  friend class ScreenSettingsDialog;
  friend class ScreenSetupModel;
  friend class ScreenSetupView;

  friend QDataStream &operator<<(QDataStream &outStream, const Screen &screen)
  {
    return outStream << screen.name() << screen.switchCornerSize() << screen.aliases() << screen.modifiers()
                     << screen.switchCorners() << screen.fixes() << screen.isServer() << screen.dualSide();
  }

  friend QDataStream &operator>>(QDataStream &inStream, Screen &screen)
  {
    return inStream >> screen.m_Name >> screen.m_SwitchCornerSize >> screen.m_Aliases >> screen.m_Modifiers >>
           screen.m_SwitchCorners >> screen.m_Fixes >> screen.m_isServer >> screen.m_DualSide;
  }

public:
  explicit Screen(const QString &name = QString());

  [[nodiscard]] const QPixmap &pixmap() const
  {
    return m_Pixmap;
  }
  [[nodiscard]] const QString &name() const
  {
    return m_Name;
  }
  [[nodiscard]] const QStringList &aliases() const
  {
    return m_Aliases;
  }

  [[nodiscard]] bool isNull() const
  {
    return m_Name.isEmpty();
  }
  [[nodiscard]] int modifier(int m) const
  {
    return m_Modifiers[m] == static_cast<int>(ScreenConfig::Modifier::DefaultMod) ? m : m_Modifiers[m];
  }
  [[nodiscard]] const QList<int> &modifiers() const
  {
    return m_Modifiers;
  }
  [[nodiscard]] bool switchCorner(int c) const
  {
    return m_SwitchCorners[c];
  }
  [[nodiscard]] const QList<bool> &switchCorners() const
  {
    return m_SwitchCorners;
  }
  [[nodiscard]] int switchCornerSize() const
  {
    return m_SwitchCornerSize;
  }
  [[nodiscard]] bool fix(const Fix f) const
  {
    return m_Fixes[static_cast<int8_t>(f)];
  }
  [[nodiscard]] const QList<bool> &fixes() const
  {
    return m_Fixes;
  }

  void loadSettings(QSettingsProxy &settings);
  void saveSettings(QSettingsProxy &settings) const;
  [[nodiscard]] QString screensSection() const;
  [[nodiscard]] QString aliasesSection() const;

  [[nodiscard]] bool swapped() const
  {
    return m_Swapped;
  }

  void setName(const QString &name)
  {
    m_Name = name;
  }
  [[nodiscard]] bool isServer() const
  {
    return m_isServer;
  }
  /**
   * @brief dualSide 双侧切入：本屏同时接入「与服务器相邻的那一侧」的对面一侧。
   *
   * 只有客户端屏幕（非服务器）需要设置本标记。开启后，服务器在 links 段里会在左右
   * 两个方向都指向本屏——screens 段仍然只定义一次本屏（屏名必须唯一，服务器会拒绝
   * 重名）。鼠标可以从服务器的任意一侧滑入本屏。
   */
  [[nodiscard]] bool dualSide() const
  {
    return m_DualSide;
  }
  void setDualSide(const bool on)
  {
    m_DualSide = on;
  }
  void markAsServer()
  {
    m_isServer = true;
  }

  bool operator==(const Screen &screen) const;

protected:
  QStringList &aliases()
  {
    return m_Aliases;
  }
  void setModifier(const Modifier m, const int n)
  {
    m_Modifiers[static_cast<int8_t>(m)] = n;
  }
  QList<int> &modifiers()
  {
    return m_Modifiers;
  }
  void addAlias(const QString &alias)
  {
    m_Aliases.append(alias);
  }
  void setSwitchCorner(const SwitchCorner c, const bool on)
  {
    m_SwitchCorners[static_cast<int8_t>(c)] = on;
  }
  QList<bool> &switchCorners()
  {
    return m_SwitchCorners;
  }
  void setSwitchCornerSize(const int val)
  {
    m_SwitchCornerSize = val;
  }
  void setFix(const Fix f, const bool on)
  {
    m_Fixes[static_cast<int8_t>(f)] = on;
  }
  QList<bool> &fixes()
  {
    return m_Fixes;
  }
  void setSwapped(const bool on)
  {
    m_Swapped = on;
  }

private:
  QPixmap m_Pixmap = QIcon::fromTheme("video-display").pixmap(QSize(96, 96));
  QString m_Name = {};
  QStringList m_Aliases = {};
  QList<int> m_Modifiers = {0, 1, 2, 3, 4, 5, 6};
  QList<bool> m_SwitchCorners = {false, false, false, false};
  int m_SwitchCornerSize = 0;
  QList<bool> m_Fixes{false, false, false, false};
  bool m_Swapped = false;
  bool m_isServer = false;
  /// @brief 双侧切入标记，见 dualSide()。持久化在 internalConfig/screens[i]/dualSide。
  bool m_DualSide = false;
};
