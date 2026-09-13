/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "I18N.h"

#include "common/Constants.h"
#include "common/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QMap>
#include <QObject>
#include <QTranslator>

I18N *I18N::instance()
{
  static I18N m;
  return &m;
}

I18N::I18N(QObject *parent) : QObject{parent}
{
  const auto appDir = QCoreApplication::applicationDirPath();
  const auto homeDir = QDir::homePath();

  const QList<QDir> appTrDirs{
      {QStringLiteral("%1/%2").arg(appDir, QStringLiteral("translations"))},
      {QStringLiteral("%1/../translations").arg(appDir)},
      {QStringLiteral("%1/../Resources/translations").arg(appDir)},
      {QStringLiteral("%1/../share/%2/translations").arg(appDir, kAppId)},
      {QStringLiteral("%1/.local/share/%2/translations").arg(homeDir, kAppId)},
      {QStringLiteral("/usr/local/share/%1/translations").arg(kAppId)},
      {QStringLiteral("/usr/share/%1/translations").arg(kAppId)}
  };
  const QStringList appTrFilter{QStringLiteral("%1*.qm").arg(kAppId)};

  for (const auto &dir : appTrDirs) {
    if (!dir.entryList(appTrFilter, QDir::Files, QDir::Name).isEmpty()) {
      m_appTrPath = dir.absolutePath();
      break;
    }
  }

  if (m_appTrPath.isEmpty()) {
    qInfo() << "no app translations found";
  }

  const auto qt = QStringLiteral("qt");
  const auto qt6 = QStringLiteral("qt6");

  const QList<QDir> qtTrDirs{
      {QStringLiteral("%1/%2").arg(appDir, QStringLiteral("translations"))},
      {QStringLiteral("%1/../Resources/translations").arg(appDir)},
      {QStringLiteral("%1/../qt-depends/translations").arg(appDir)},
      {QStringLiteral("%1/../share/%2/translations").arg(appDir, qt6)},
      {QStringLiteral("%1/../share/%2/translations").arg(appDir, qt)},
      {QStringLiteral("%1/.local/share/%2/translations").arg(homeDir, qt6)},
      {QStringLiteral("%1/.local/share/%2/translations").arg(homeDir, qt)},
      {QStringLiteral("/usr/local/share/%2/translations").arg(qt6)},
      {QStringLiteral("/usr/local/share/%2/translations").arg(qt)},
      {QStringLiteral("/usr/share/%2/translations").arg(qt6)},
      {QStringLiteral("/usr/share/%2/translations").arg(qt)}
  };
  const QStringList qtTrFilter{QStringLiteral("qt_*.qm")};

  for (const auto &dir : qtTrDirs) {
    if (!dir.entryList(qtTrFilter, QDir::Files, QDir::Name).isEmpty()) {
      m_qtTrPath = dir.absolutePath();
      break;
    }
  }

  if (m_qtTrPath.isEmpty()) {
    qInfo() << "no qt translations found";
  }

  detectLanguages();

  // A stored language means the user pinned one in the settings, an empty value (or
  // SystemLanguage) means we keep following the system locale.
  const auto storedLang = Settings::value(Settings::Core::Language).toString();
  if (storedLang.isEmpty() || storedLang == SystemLanguage) {
    loadSystemTranslations();
  } else {
    m_followSystemLanguage = false;
    m_currentLang = storedLang;
    loadTranslations(storedLang);
  }
}

void I18N::loadSystemTranslations()
{
  m_followSystemLanguage = true;

  static const auto s_prefix = QStringLiteral("_");

  auto appTranslator = new QTranslator(this);
  if (appTranslator->load(QLocale(), kAppId, s_prefix, m_appTrPath)) {
    m_currentTranslations.append(appTranslator);
    QCoreApplication::installTranslator(appTranslator);

    // QTranslator returns the source text when a message has no translation, that is
    // the case for english (the plural only catalog), do not use it as a name.
    const auto nativeName = appTranslator->translate("i18n", "LocalizedName");
    m_currentLang =
        m_nameMap.key(nativeName == QStringLiteral("LocalizedName") ? QString() : nativeName, m_currentLang);
  }

  if (m_currentLang.isEmpty())
    m_currentLang = QStringLiteral("en");

  auto qtTranslator = new QTranslator(this);
  if (qtTranslator->load(QLocale(), QStringLiteral("qt"), s_prefix, m_qtTrPath)) {
    m_currentTranslations.append(qtTranslator);
    QCoreApplication::installTranslator(qtTranslator);
  }
}

void I18N::loadTranslations(const QString &langName)
{
  for (const auto &translation : m_translations.value(langName)) {
    if (translation.isEmpty())
      continue;

    auto translator = new QTranslator(this);
    if (translator->load(translation)) {
      m_currentTranslations.append(translator);
      QCoreApplication::installTranslator(translator);
    }
  }
}

void I18N::clearTranslations()
{
  for (const auto &translation : std::as_const(m_currentTranslations))
    QCoreApplication::removeTranslator(translation);

  qDeleteAll(m_currentTranslations);
  m_currentTranslations.clear();
}

QStringList I18N::detectedLanguages()
{
  return instance()->m_nameMap.values();
}

QString I18N::nativeTo639Name(QString nativeName)
{
  return instance()->m_nameMap.key(nativeName);
}

QString I18N::toNativeName(QString shortName)
{
  return instance()->m_nameMap.value(shortName);
}

QString I18N::currentLanguage()
{
  return instance()->m_currentLang;
}

QString I18N::systemLanguageName()
{
  return tr("Follow system");
}

QStringList I18N::selectableLanguages()
{
  QStringList languages{systemLanguageName()};
  languages.append(detectedLanguages());
  return languages;
}

bool I18N::followsSystemLanguage()
{
  return instance()->m_followSystemLanguage;
}

void I18N::setLanguage(const QString &langName)
{
  auto *self = instance();

  const bool followSystem = langName.isEmpty() || langName == SystemLanguage;

  if (!followSystem && !self->m_translations.contains(langName))
    return;

  if (followSystem) {
    if (self->m_followSystemLanguage)
      return;
    Settings::setValue(Settings::Core::Language, SystemLanguage);
  } else {
    if (langName == self->m_currentLang)
      return;
    Settings::setValue(Settings::Core::Language, langName);
  }

  self->clearTranslations();

  if (followSystem) {
    // forget the pinned language so the system locale is detected again
    self->m_currentLang.clear();
    self->loadSystemTranslations();
  } else {
    self->m_followSystemLanguage = false;
    self->m_currentLang = langName;
    self->loadTranslations(langName);
  }

  Q_EMIT self->languageChanged(self->m_currentLang);
}

void I18N::reDetectLanguages()
{
  instance()->detectLanguages();
}

void I18N::detectLanguages()
{
  const auto oldList = m_translations;
  m_translations.clear();
  m_nameMap.clear();

  QStringList nameFilter = {QStringLiteral("%1_*.qm").arg(kAppId)};
  QMap<QString, QString> appTranslations;
  QStringList detectedLangCodes;
  QDir dir(m_appTrPath);
  QStringList langList = dir.entryList(nameFilter, QDir::Files, QDir::Name);

  for (const QString &translation : std::as_const(langList)) {
    QTranslator translator;
    std::ignore = translator.load(translation, dir.absolutePath());
    const auto longCode = translator.language();
    //: Replace with your Language name
    //: This is a required string
    QString nativeLang = translator.translate("i18n", "LocalizedName");
    // QTranslator hands back the source text when the message has no translation,
    // which is the case for the english (plural only) catalog.
    if (nativeLang.isEmpty() || nativeLang == QStringLiteral("LocalizedName"))
      nativeLang = QStringLiteral("English");

    QString shortCode;
    if (longCode.startsWith(QStringLiteral("zh")) || longCode.startsWith(QStringLiteral("pt")))
      shortCode = longCode;
    else
      shortCode = longCode.mid(0, 2);

    appTranslations.insert(shortCode, translator.filePath());
    m_nameMap.insert(shortCode, nativeLang);
    detectedLangCodes.append(QStringLiteral("qt_%1.qm").arg(shortCode));
  }

  dir.setPath(m_qtTrPath);
  const static auto qtTrNameLen = 3; // length of qt_
  langList = dir.entryList(detectedLangCodes, QDir::Files, QDir::Name);

  QMap<QString, QString> qtTranslations;
  for (const QString &translation : std::as_const(langList)) {
    QString lang = translation.mid(qtTrNameLen, translation.lastIndexOf(QLatin1Char('.')) - qtTrNameLen);
    qtTranslations.insert(lang, QStringLiteral("%1/%2").arg(m_qtTrPath, translation));
  }

  const QStringList keys = appTranslations.keys();
  for (const QString &lang : keys)
    m_translations.insert(lang, {appTranslations.value(lang), qtTranslations.value(lang)});

  if (oldList != m_translations)
    Q_EMIT languagesChanged(m_translations.keys());
}
