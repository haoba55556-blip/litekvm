/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfig.h"

#include "Hotkey.h"
#include "common/Settings.h"

#include <QAbstractButton>
#include <QPushButton>

using enum ScreenConfig::Modifier;
using enum ScreenConfig::SwitchCorner;
using enum ScreenConfig::Fix;

static const struct
{
  int x;
  int y;
  const char *name;
} neighbourDirs[] = {
    {1, 0, "right"},
    {-1, 0, "left"},
    {0, -1, "up"},
    {0, 1, "down"},

};

const int serverDefaultIndex = 7;

ServerConfig::ServerConfig(int columns, int rows) : m_Screens(columns), m_columns(columns), m_rows(rows)
{
  recall();
}

bool ServerConfig::save(const QString &fileName) const
{
  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;

  save(file);
  file.close();

  return true;
}

bool ServerConfig::operator==(const ServerConfig &sc) const
{
  return m_Screens == sc.m_Screens && //
         m_Hotkeys == sc.m_Hotkeys;   //
}

void ServerConfig::save(QFile &file) const
{
  QTextStream outStream(&file);
  outStream << *this;
}

void ServerConfig::setupScreens()
{
  screens().clear();
  hotkeys().clear();

  // There must always be screen objects for each cell in the screens QList.
  // Unused screens are identified by having an empty name.
  for (int i = 0; i < m_columns * m_rows; i++)
    addScreen(Screen());
}

void ServerConfig::commit()
{
  qDebug("committing server config");

  settings().beginGroup("internalConfig");
  settings().remove("");

  settings().beginWriteArray("screens");
  for (int i = 0; i < screens().size(); i++) {
    settings().setArrayIndex(i);
    const auto &screen = screens()[i];
    screen.saveSettings(settings());
    auto screenName = Settings::value(Settings::Core::ComputerName).toString();
    if (screen.isServer() && screenName != screen.name()) {
      Settings::setValue(Settings::Core::ComputerName, screen.name());
    }
  }
  settings().endArray();

  settings().beginWriteArray("hotkeys");
  for (int i = 0; i < hotkeys().size(); i++) {
    settings().setArrayIndex(i);
    hotkeys()[i].saveSettings(settings().get());
  }
  settings().endArray();

  settings().endGroup();
}

void ServerConfig::recall()
{
  qDebug("recalling server config");

  settings().beginGroup("internalConfig");

  m_columns = Settings::value(Settings::Server::GridWidth).toInt();
  m_rows = Settings::value(Settings::Server::GridHeight).toInt();

  // we need to know the number of columns and rows before we can set up
  // ourselves
  setupScreens();

  int numScreens = settings().beginReadArray("screens");
  Q_ASSERT(numScreens <= screens().size());
  for (int i = 0; i < numScreens; i++) {
    settings().setArrayIndex(i);
    screens()[i].loadSettings(settings());
    if (getServerName() == screens()[i].name()) {
      screens()[i].markAsServer();
    }
  }
  settings().endArray();

  int numHotkeys = settings().beginReadArray("hotkeys");
  for (int i = 0; i < numHotkeys; i++) {
    settings().setArrayIndex(i);
    Hotkey h;
    h.loadSettings(settings().get());
    hotkeys().append(h);
  }
  settings().endArray();

  settings().endGroup();
}

int ServerConfig::adjacentScreenIndex(int idx, int deltaColumn, int deltaRow) const
{
  if (screens()[idx].isNull())
    return -1;

  // if we're at the left or right end of the table, don't find results going
  // further left or right
  if ((deltaColumn > 0 && (idx + 1) % m_columns == 0) || (deltaColumn < 0 && idx % m_columns == 0))
    return -1;

  int arrayPos = idx + deltaColumn + deltaRow * m_columns;

  if (arrayPos >= screens().size() || arrayPos < 0)
    return -1;

  return arrayPos;
}

int ServerConfig::neighbourIndex(int idx, int deltaColumn, int deltaRow) const
{
  const int adjacent = adjacentScreenIndex(idx, deltaColumn, deltaRow);
  if (adjacent != -1 && !screens()[adjacent].isNull())
    return adjacent;

  return dualSideNeighbourIndex(idx, deltaColumn, deltaRow);
}

int ServerConfig::dualSideNeighbourIndex(int idx, int deltaColumn, int deltaRow) const
{
  // 双侧切入（dual side）：服务器左右两侧接到同一台客户端。
  // deskflow 的 links 段允许同一台屏幕的 left 与 right 指向同一目标屏幕，但 screens 段
  // 要求屏名唯一（重名时服务器拒绝启动），所以这里只补边、绝不新增屏幕。
  // 只有左侧/右侧（deltaColumn != 0）开放本特性；上下两侧的语义与网格不一致，暂不开放。
  if (deltaColumn == 0 || idx < 0 || idx >= screens().size() || screens()[idx].isNull() ||
      !screens()[idx].isServer())
    return -1;

  // 反向的网格邻居 = 本屏的「锚点」屏；只有锚点屏自己勾选了双侧切入才镜像。
  const int anchor = adjacentScreenIndex(idx, -deltaColumn, deltaRow);
  if (anchor == -1 || screens()[anchor].isNull() || !screens()[anchor].dualSide())
    return -1;

  return anchor;
}

QTextStream &operator<<(QTextStream &outStream, const ServerConfig &config)
{
  outStream << "section: screens" << Qt::endl;

  for (const Screen &s : config.screens()) {
    if (!s.isNull())
      outStream << s.screensSection();
  }

  outStream << "end" << Qt::endl << Qt::endl;

  outStream << "section: links" << Qt::endl;

  for (int i = 0; const auto &screen : config.screens()) {
    if (!screen.isNull()) {
      outStream << "\t" << screen.name() << ":\n";
      for (const auto &neighbour : std::as_const(neighbourDirs)) {
        // neighbourIndex 在网格相邻之外还会补上双侧切入的镜像边；
        // 同一目标屏可以在两个不同方向各出现一次（links 段允许，screens 段不受影响）。
        const int idx = config.neighbourIndex(i, neighbour.x, neighbour.y);
        if (idx != -1)
          outStream << "\t\t" << neighbour.name << " = " << config.screens()[idx].name() << Qt::endl;
      }
    }
    i++;
  }

  outStream << "end" << Qt::endl << Qt::endl;

  outStream << "section: options" << Qt::endl;

  for (const Hotkey &hotkey : config.hotkeys())
    outStream << hotkey;

  outStream << "end" << Qt::endl << Qt::endl;

  return outStream;
}

int ServerConfig::numScreens() const
{
  int rval = 0;

  for (const Screen &s : screens()) {
    if (!s.isNull())
      rval++;
  }

  return rval;
}

QString ServerConfig::getServerName() const
{
  return Settings::value(Settings::Core::ComputerName).toString();
}

void ServerConfig::updateServerName()
{
  for (auto &screen : screens()) {
    if (screen.isServer()) {
      screen.setName(Settings::value(Settings::Core::ComputerName).toString());
      break;
    }
  }
}

QString ServerConfig::configFile() const
{
  return Settings::value(Settings::Server::ExternalConfigFile).toString();
}

bool ServerConfig::useExternalConfig() const
{
  return Settings::value(Settings::Server::ExternalConfig).toBool();
}

bool ServerConfig::isFull() const
{
  bool isFull = true;

  for (const auto &screen : screens()) {
    if (screen.isNull()) {
      isFull = false;
      break;
    }
  }

  return isFull;
}

bool ServerConfig::screenExists(const QString &screenName) const
{
  bool isExists = false;

  for (const auto &screen : screens()) {
    if (!screen.isNull() && screen.name() == screenName) {
      isExists = true;
      break;
    }
  }

  return isExists;
}

void ServerConfig::addClient(const QString &clientName)
{
  int serverIndex = -1;
  const auto screenName = Settings::value(Settings::Core::ComputerName).toString();

  if (findScreenName(screenName, serverIndex)) {
    m_Screens[serverIndex].markAsServer();
  } else {
    fixNoServer(screenName, serverIndex);
  }

  m_Screens.addScreenByPriority(Screen(clientName));
}

void ServerConfig::setConfigFile(const QString &configFile) const
{
  Settings::setValue(Settings::Server::ExternalConfigFile, configFile);
}

void ServerConfig::setUseExternalConfig(bool useExternalConfig) const
{
  Settings::setValue(Settings::Server::ExternalConfig, useExternalConfig);
}

bool ServerConfig::findScreenName(const QString &name, int &index)
{
  bool found = false;
  for (int i = 0; i < screens().size(); i++) {
    if (!screens()[i].isNull() && screens()[i].name().compare(name) == 0) {
      index = i;
      found = true;
      break;
    }
  }
  return found;
}

bool ServerConfig::fixNoServer(const QString &name, int &index)
{
  bool fixed = false;
  if (screens()[serverDefaultIndex].isNull()) {
    m_Screens[serverDefaultIndex].setName(name);
    m_Screens[serverDefaultIndex].markAsServer();
    index = serverDefaultIndex;
    fixed = true;
  }

  return fixed;
}

QSettingsProxy &ServerConfig::settings()
{
  return Settings::proxy();
}
