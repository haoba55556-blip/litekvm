/*
 * LiteKVM — zero-config keyboard/mouse sharing
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0
 *
 * ThemeLoader — installs the LiteKVM dark theme (see litekvm-theme-tokens.md).
 */

#include "ThemeLoader.h"

#include "common/Constants.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QPalette>
#include <QStringList>

namespace {

// Resource path produced by qt_add_resources() in
// src/apps/deskflow-gui/CMakeLists.txt (PREFIX "/res").
const auto kThemeResource = QStringLiteral(":/res/litekvm-theme-dark.qss");
// Fallback resource path in case the file gets registered in deskflow.qrc instead.
const auto kThemeResourceAlt = QStringLiteral(":/litekvm-theme-dark.qss");
const auto kThemeFileName = QStringLiteral("litekvm-theme-dark.qss");

// Plain C strings: qEnvironmentVariable()/qEnvironmentVariableIsSet() take const char*.
constexpr auto kDisableThemeEnv = "LITEKVM_DISABLE_THEME";
constexpr auto kThemeOverrideEnv = "LITEKVM_THEME_QSS";

// Value returned by liteKvmThemeSource(); set by applyLiteKvmTheme().
QString s_themeSource;

/**
 * @brief Token values QSS cannot apply — the glyphs QStyle draws itself
 *        (combo-box drop-down arrow, spin-box arrows, …).
 * Keep in sync with litekvm-theme-tokens.md §2.
 */
namespace tokens {
// surfaces: canvas / surface / sunken / row-alt / hover
const QColor canvas(0x0D, 0x0F, 0x12);
const QColor surface(0x14, 0x17, 0x1C);
const QColor sunken(0x0A, 0x0C, 0x0E);
const QColor altBase(0x10, 0x13, 0x17);
const QColor hover(0x1B, 0x20, 0x27);
// borders: subtle / default / strong
const QColor borderSubtle(0x1F, 0x24, 0x2B);
const QColor borderDefault(0x2A, 0x30, 0x38);
const QColor borderStrong(0x3A, 0x42, 0x4C);
// text: primary / muted / disabled
const QColor textPrimary(0xE7, 0xEA, 0xEE);
const QColor textMuted(0x7A, 0x84, 0x94);
const QColor textDisabled(0x6E, 0x77, 0x83);
// accent: default / as-text-on-dark
const QColor accent(0x5E, 0x6A, 0xD2);
const QColor accentText(0x8B, 0x95, 0xF0);
} // namespace tokens

bool readTextFile(const QString &path, QString *out)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;

  *out = QString::fromUtf8(file.readAll());
  return !out->isEmpty();
}

/**
 * @brief Resolves the stylesheet: env override → Qt resource → next to the exe
 *        → source tree (dev builds without the resource registered).
 */
QString resolveStyleSheetPath()
{
  const auto envOverride = qEnvironmentVariable(kThemeOverrideEnv);
  if (!envOverride.isEmpty() && QFile::exists(envOverride))
    return envOverride;

  const auto appDir = QCoreApplication::applicationDirPath();
  const QStringList candidates{
      kThemeResource,
      kThemeResourceAlt,
      appDir + QLatin1Char('/') + kThemeFileName,
      appDir + QStringLiteral("/resources/") + kThemeFileName,
      appDir + QStringLiteral("/../src/apps/res/") + kThemeFileName,
      appDir + QStringLiteral("/../../src/apps/res/") + kThemeFileName,
  };

  for (const auto &candidate : candidates) {
    if (QFile::exists(candidate))
      return candidate;
  }
  return {};
}

void applyAppFont()
{
  // CJK-first stack: the primary audience runs Chinese Windows.
  static const QStringList kUiFontFamilies{
      QStringLiteral("Microsoft YaHei UI"),
      QStringLiteral("Microsoft YaHei"),
      QStringLiteral("Segoe UI"),
      QStringLiteral("Inter"),
      QStringLiteral("Noto Sans CJK SC"),
      QStringLiteral("Source Han Sans SC"),
  };

  QFont font = QApplication::font();
  font.setFamilies(kUiFontFamilies);
  QApplication::setFont(font);
}

/**
 * @brief Dark palette for the few glyphs QSS cannot reach.
 * Combo-box drop-down arrows and spin-box arrows are drawn by the platform
 * style from palette colours — on a light system theme they would be
 * dark-on-dark and effectively invisible on our dark surfaces.
 */
void applyDarkPalette()
{
  QPalette p;

  p.setColor(QPalette::Window, tokens::canvas);
  p.setColor(QPalette::WindowText, tokens::textPrimary);
  p.setColor(QPalette::Base, tokens::sunken);
  p.setColor(QPalette::AlternateBase, tokens::altBase);
  p.setColor(QPalette::Text, tokens::textPrimary);
  p.setColor(QPalette::PlaceholderText, tokens::textMuted);
  p.setColor(QPalette::Button, tokens::hover);
  p.setColor(QPalette::ButtonText, tokens::textPrimary);
  p.setColor(QPalette::BrightText, Qt::white);
  p.setColor(QPalette::Light, tokens::borderStrong);
  p.setColor(QPalette::Midlight, tokens::borderDefault);
  p.setColor(QPalette::Mid, tokens::borderSubtle);
  p.setColor(QPalette::Dark, tokens::sunken);
  p.setColor(QPalette::Shadow, Qt::black);
  p.setColor(QPalette::Highlight, tokens::accent);
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::Link, tokens::accentText);
  p.setColor(QPalette::LinkVisited, tokens::accentText);
  p.setColor(QPalette::ToolTipBase, tokens::hover);
  p.setColor(QPalette::ToolTipText, tokens::textPrimary);

  // Disabled group — legible but clearly inert.
  p.setColor(QPalette::Disabled, QPalette::WindowText, tokens::textDisabled);
  p.setColor(QPalette::Disabled, QPalette::Text, tokens::textDisabled);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, tokens::textDisabled);
  p.setColor(QPalette::Disabled, QPalette::Base, tokens::surface);
  p.setColor(QPalette::Disabled, QPalette::Highlight, tokens::borderDefault);
  p.setColor(QPalette::Disabled, QPalette::HighlightedText, tokens::textDisabled);

  QApplication::setPalette(p);
}

/**
 * @brief Re-pins the icon theme to the dark variant.
 * StyleUtils::updateIconTheme() picks dark/light from the *system* scheme; the
 * LiteKVM UI is always dark, so the light (dark-glyph) icon set would disappear.
 */
void applyDarkIconTheme()
{
  const auto themeName = QStringLiteral("%1-dark").arg(kAppId);
  QIcon::setThemeName(themeName);
  QIcon::setFallbackThemeName(themeName);
  QIcon::setFallbackSearchPaths({QStringLiteral(":/icons/%1").arg(themeName)});
}

} // namespace

namespace deskflow::gui {

bool applyLiteKvmTheme()
{
  if (qEnvironmentVariableIsSet(kDisableThemeEnv))
    return false;

  auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
  if (!app) {
    qWarning("LiteKVM theme: no QApplication yet, skipped");
    return false;
  }

  applyAppFont();
  applyDarkPalette();
  applyDarkIconTheme();

  const auto path = resolveStyleSheetPath();
  if (path.isEmpty()) {
    qWarning("LiteKVM theme: %s not found, keeping the default look", qUtf8Printable(kThemeFileName));
    return false;
  }

  QString styleSheet;
  if (!readTextFile(path, &styleSheet)) {
    qWarning("LiteKVM theme: cannot read stylesheet %s", qUtf8Printable(path));
    return false;
  }

  s_themeSource = path;
  app->setStyleSheet(styleSheet);
  return true;
}

QString liteKvmThemeSource()
{
  return s_themeSource;
}

} // namespace deskflow::gui
