// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Place a finished file into a folder without leaving a locked or empty stub.
//
// The final name is created only after the bytes are written, the handle is
// closed, and the size matches. A failure deletes the temporary file.
#pragma once

#include "ClipFileChunk.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace litekvm {

struct LandedFile
{
  bool ok = false;
  QString path;
};

inline QString landedFinalPath(const QString &directory, const QString &fileName)
{
  const QString desired = QDir(directory).filePath(fileName);
  if (!QFileInfo::exists(desired))
    return desired;
  const QFileInfo info(desired);
  const QString suffix = info.suffix().isEmpty() ? QString() : QStringLiteral(".") + info.suffix();
  for (int i = 1; i < 1000; ++i) {
    const QString candidate =
        QDir(directory).filePath(info.completeBaseName() + QStringLiteral(" (%1)").arg(i) + suffix);
    if (!QFileInfo::exists(candidate))
      return candidate;
  }
  return desired;
}

/// Copy @p sourcePath into @p directory. @p expectedBytes is the size the
/// transfer finished at; a mismatch or a still-open part file is not landed.
inline LandedFile landFinishedFile(const QString &sourcePath, const QString &directory, qint64 expectedBytes)
{
  LandedFile out;
  if (sourcePath.endsWith(QLatin1String(kClipPartSuffix), Qt::CaseInsensitive))
    return out;

  const QFileInfo srcInfo(sourcePath);
  if (!srcInfo.exists() || !srcInfo.isFile())
    return out;
  const qint64 size = srcInfo.size();
  if (size < 0 || (expectedBytes >= 0 && size != expectedBytes))
    return out;

  QDir dir(directory);
  if (!dir.exists())
    return out;

  const QString sourceAbs = srcInfo.absoluteFilePath();
  if (QFileInfo(dir.filePath(srcInfo.fileName())).absoluteFilePath() == sourceAbs && size == expectedBytes) {
    QFile check(sourceAbs);
    if (!check.open(QIODevice::ReadOnly) || check.size() != size)
      return out;
    check.close();
    out.ok = true;
    out.path = QDir::toNativeSeparators(sourceAbs);
    return out;
  }

  QFile src(sourcePath);
  if (!src.open(QIODevice::ReadOnly))
    return out;

  const QString finalPath = landedFinalPath(directory, srcInfo.fileName());
  const QString partPath = finalPath + QString::fromLatin1(kClipPartSuffix);
  QFile::remove(partPath);

  QFile dst(partPath);
  if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    src.close();
    return out;
  }

  bool failed = false;
  while (!src.atEnd()) {
    const QByteArray chunk = src.read(256 * 1024);
    if (chunk.isEmpty()) {
      failed = true;
      break;
    }
    if (dst.write(chunk) != chunk.size()) {
      failed = true;
      break;
    }
  }
  dst.flush();
  src.close();
  dst.close();

  if (failed || QFileInfo(partPath).size() != size) {
    QFile::remove(partPath);
    return out;
  }

  {
    QFile check(partPath);
    if (!check.open(QIODevice::ReadOnly) || check.size() != size) {
      check.close();
      QFile::remove(partPath);
      return out;
    }
    check.close();
  }

  if (!QFile::rename(partPath, finalPath)) {
    QFile::remove(partPath);
    return out;
  }

  QFile finalCheck(finalPath);
  if (!finalCheck.open(QIODevice::ReadOnly) || finalCheck.size() != size) {
    finalCheck.close();
    QFile::remove(finalPath);
    return out;
  }
  finalCheck.close();
  out.ok = true;
  out.path = QDir::toNativeSeparators(QFileInfo(finalPath).absoluteFilePath());
  return out;
}

} // namespace litekvm
