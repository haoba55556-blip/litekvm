// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0-only
// Place a finished file or folder without leaving a locked or empty stub.
//
// A file is written to a sibling .litekvm-part and renamed only after the
// size matches. A folder is copied as its own folder (children stay inside
// it) into a part directory and renamed only after that tree matches. A
// failure deletes the part path, so the destination never keeps a half tree.
#pragma once

#include "ClipFileChunk.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace litekvm {

struct LandedFile
{
  bool ok = false;
  QString path;
  /// Empty when @c ok. A stable token such as "not-a-file" or "open-failed".
  QString reason;
};

struct PlacedFiles
{
  /// Native paths that landed, joined with ';'. Empty when nothing landed.
  QString landed;
  /// Set only when @c landed is empty.
  QString failReason;
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
  if (sourcePath.endsWith(QLatin1String(kClipPartSuffix), Qt::CaseInsensitive)) {
    out.reason = QStringLiteral("part-file");
    return out;
  }

  const QFileInfo srcInfo(sourcePath);
  if (!srcInfo.exists() || !srcInfo.isFile()) {
    out.reason = srcInfo.exists() ? QStringLiteral("not-a-file") : QStringLiteral("missing");
    return out;
  }
  const qint64 size = srcInfo.size();
  if (size < 0 || (expectedBytes >= 0 && size != expectedBytes)) {
    out.reason = QStringLiteral("size-mismatch");
    return out;
  }

  QDir dir(directory);
  if (!dir.exists()) {
    out.reason = QStringLiteral("dest-missing");
    return out;
  }

  const QString sourceAbs = srcInfo.absoluteFilePath();
  if (QFileInfo(dir.filePath(srcInfo.fileName())).absoluteFilePath() == sourceAbs && size == expectedBytes) {
    QFile check(sourceAbs);
    if (!check.open(QIODevice::ReadOnly) || check.size() != size) {
      out.reason = QStringLiteral("verify-failed");
      return out;
    }
    check.close();
    out.ok = true;
    out.path = QDir::toNativeSeparators(sourceAbs);
    return out;
  }

  QFile src(sourcePath);
  if (!src.open(QIODevice::ReadOnly)) {
    out.reason = QStringLiteral("open-failed");
    return out;
  }

  const QString finalPath = landedFinalPath(directory, srcInfo.fileName());
  const QString partPath = finalPath + QString::fromLatin1(kClipPartSuffix);
  QFile::remove(partPath);

  QFile dst(partPath);
  if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    src.close();
    out.reason = QStringLiteral("open-failed");
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
    out.reason = QStringLiteral("write-failed");
    return out;
  }

  {
    QFile check(partPath);
    if (!check.open(QIODevice::ReadOnly) || check.size() != size) {
      check.close();
      QFile::remove(partPath);
      out.reason = QStringLiteral("verify-failed");
      return out;
    }
    check.close();
  }

  if (!QFile::rename(partPath, finalPath)) {
    QFile::remove(partPath);
    out.reason = QStringLiteral("rename-failed");
    return out;
  }

  QFile finalCheck(finalPath);
  if (!finalCheck.open(QIODevice::ReadOnly) || finalCheck.size() != size) {
    finalCheck.close();
    QFile::remove(finalPath);
    out.reason = QStringLiteral("verify-failed");
    return out;
  }
  finalCheck.close();
  out.ok = true;
  out.path = QDir::toNativeSeparators(QFileInfo(finalPath).absoluteFilePath());
  return out;
}

inline Qt::CaseSensitivity landedPathCase()
{
#if defined(Q_OS_WIN)
  return Qt::CaseInsensitive;
#else
  return Qt::CaseSensitive;
#endif
}

/// True when @p child is @p parent or a path inside it. A sibling whose name
/// merely starts with the parent name ("folder" vs "folder-extra") is not.
inline bool pathIsNestedIn(const QString &parent, const QString &child)
{
  const Qt::CaseSensitivity cs = landedPathCase();
  const QString root = QDir::cleanPath(parent);
  const QString candidate = QDir::cleanPath(child);
  if (root.compare(candidate, cs) == 0)
    return true;
  const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
  return candidate.startsWith(prefix, cs);
}

inline QString landedEntryName(const QFileInfo &info)
{
  QString name = info.fileName();
  if (name.isEmpty())
    name = QDir(info.absoluteFilePath()).dirName();
  return name;
}

inline bool removeLandedTree(const QString &path)
{
  const QFileInfo info(path);
  if (!info.exists())
    return true;
  if (info.isDir() && !info.isSymLink())
    return QDir(path).removeRecursively();
  return QFile::remove(path);
}

inline bool copyRegularFile(const QString &from, const QString &to, QString *reason)
{
  const QFileInfo srcInfo(from);
  const qint64 size = srcInfo.size();
  if (!srcInfo.isFile() || size < 0) {
    *reason = QStringLiteral("copy-failed");
    return false;
  }

  QFile src(from);
  if (!src.open(QIODevice::ReadOnly)) {
    *reason = QStringLiteral("open-failed");
    return false;
  }

  QFile::remove(to);
  QFile dst(to);
  if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    src.close();
    *reason = QStringLiteral("open-failed");
    return false;
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

  if (failed || QFileInfo(to).size() != size) {
    QFile::remove(to);
    *reason = QStringLiteral("write-failed");
    return false;
  }

  QFile check(to);
  if (!check.open(QIODevice::ReadOnly) || check.size() != size) {
    check.close();
    QFile::remove(to);
    *reason = QStringLiteral("verify-failed");
    return false;
  }
  check.close();
  return true;
}

/// A dragged tree is shallow. Stop rather than overflow the stack, and let
/// the caller delete the part directory.
inline bool copyTreeInto(const QString &sourceDir, const QString &destDir, QString *reason, int depth = 0)
{
  if (depth > 256) {
    *reason = QStringLiteral("copy-failed");
    return false;
  }
  if (!QDir().mkpath(destDir)) {
    *reason = QStringLiteral("mkdir-failed");
    return false;
  }

  const QFileInfo srcInfo(sourceDir);
  const QDir src(sourceDir);
  const QFileInfoList entries =
      src.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
  // An unreadable directory lists as empty. That must not land as an empty
  // folder and drop the files we could not see.
  if (!srcInfo.isReadable() && entries.isEmpty()) {
    *reason = QStringLiteral("open-failed");
    return false;
  }
  for (const QFileInfo &info : entries) {
    const QString childName = info.fileName();
    if (childName.isEmpty() || childName == QLatin1String(".") || childName == QLatin1String(".."))
      continue;
    const QString target = QDir(destDir).filePath(childName);
    if (info.isSymLink() || (!info.isDir() && !info.isFile())) {
      *reason = QStringLiteral("unsupported-entry");
      return false;
    }
    if (info.isDir()) {
      if (!copyTreeInto(info.absoluteFilePath(), target, reason, depth + 1))
        return false;
      continue;
    }
    if (!copyRegularFile(info.absoluteFilePath(), target, reason))
      return false;
  }
  return true;
}

inline bool treeMatches(const QString &sourceDir, const QString &destDir, QString *reason, int depth = 0)
{
  if (depth > 256) {
    *reason = QStringLiteral("verify-failed");
    return false;
  }
  const QDir src(sourceDir);
  const QFileInfoList entries =
      src.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
  for (const QFileInfo &info : entries) {
    const QString childName = info.fileName();
    if (childName.isEmpty() || childName == QLatin1String(".") || childName == QLatin1String(".."))
      continue;
    const QFileInfo dst(QDir(destDir).filePath(childName));
    if (info.isDir() && !info.isSymLink()) {
      const bool dirOk = dst.isDir() && !dst.isSymLink();
      if (!dirOk || !treeMatches(info.absoluteFilePath(), dst.absoluteFilePath(), reason, depth + 1)) {
        if (reason->isEmpty())
          *reason = QStringLiteral("verify-failed");
        return false;
      }
      continue;
    }
    if (info.isFile() && !info.isSymLink()) {
      if (!dst.isFile() || dst.isSymLink() || dst.size() != info.size()) {
        *reason = QStringLiteral("verify-failed");
        return false;
      }
      continue;
    }
    *reason = QStringLiteral("verify-failed");
    return false;
  }
  return true;
}

/// Copy the directory @p sourcePath into @p directory as one folder.
/// Children stay inside that folder. The final name is published only after
/// the tree matches; a failure deletes the `.litekvm-part` directory.
inline LandedFile landFinishedDirectory(const QString &sourcePath, const QString &directory)
{
  LandedFile out;
  const QFileInfo srcInfo(sourcePath);
  if (!srcInfo.exists() || !srcInfo.isDir() || srcInfo.isSymLink()) {
    out.reason = srcInfo.exists() ? QStringLiteral("not-a-directory") : QStringLiteral("missing");
    return out;
  }

  const QString name = landedEntryName(srcInfo);
  if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
    out.reason = QStringLiteral("not-a-directory");
    return out;
  }

  QDir dir(directory);
  if (!dir.exists()) {
    out.reason = QStringLiteral("dest-missing");
    return out;
  }

  const QString sourceAbs = QDir::cleanPath(srcInfo.absoluteFilePath());
  const QString destAbs = QDir::cleanPath(dir.absolutePath());
  if (pathIsNestedIn(sourceAbs, destAbs)) {
    out.reason = QStringLiteral("dest-inside-source");
    return out;
  }

  const QString finalPath = QDir::cleanPath(QFileInfo(landedFinalPath(directory, name)).absoluteFilePath());
  if (QFileInfo::exists(finalPath)) {
    out.reason = QStringLiteral("name-exhausted");
    return out;
  }
  const QString stagePath = finalPath + QString::fromLatin1(kClipPartSuffix);
  if (pathIsNestedIn(sourceAbs, stagePath) || pathIsNestedIn(sourceAbs, finalPath)) {
    out.reason = QStringLiteral("dest-inside-source");
    return out;
  }
  if (QFileInfo::exists(stagePath) && !removeLandedTree(stagePath)) {
    out.reason = QStringLiteral("stage-busy");
    return out;
  }

  QString failReason;
  if (!copyTreeInto(sourceAbs, stagePath, &failReason)) {
    if (!removeLandedTree(stagePath))
      out.reason = QStringLiteral("cleanup-failed");
    else
      out.reason = failReason.isEmpty() ? QStringLiteral("copy-failed") : failReason;
    return out;
  }

  if (!QFile::rename(stagePath, finalPath)) {
    removeLandedTree(stagePath);
    out.reason = QStringLiteral("rename-failed");
    return out;
  }

  QString verifyReason;
  if (!QFileInfo(finalPath).isDir() || !treeMatches(sourceAbs, finalPath, &verifyReason)) {
    removeLandedTree(finalPath);
    removeLandedTree(stagePath);
    out.reason = verifyReason.isEmpty() ? QStringLiteral("verify-failed") : verifyReason;
    return out;
  }

  out.ok = true;
  out.path = QDir::toNativeSeparators(QFileInfo(finalPath).absoluteFilePath());
  return out;
}

/// Land each received path. A directory is copied as a folder. Anything else
/// uses landFinishedFile, including its part-file and size checks.
inline PlacedFiles placeReceivedFiles(const QStringList &paths, const QString &directory)
{
  PlacedFiles out;
  if (paths.isEmpty()) {
    out.failReason = QStringLiteral("no-paths");
    return out;
  }
  if (directory.isEmpty()) {
    out.failReason = QStringLiteral("no-folder");
    return out;
  }

  QStringList landed;
  QString firstFail;
  for (const QString &path : paths) {
    const QFileInfo info(path);
    LandedFile file;
    // A staged drag is a real file or directory. Following a link would copy
    // something outside the drop, so leave it unlanded.
    if (info.isSymLink()) {
      file.reason = QStringLiteral("unsupported-entry");
    } else if (info.isDir()) {
      file = landFinishedDirectory(path, directory);
    } else {
      file = landFinishedFile(path, directory, info.size());
    }
    if (file.ok)
      landed.append(file.path);
    else if (firstFail.isEmpty())
      firstFail = file.reason.isEmpty() ? QStringLiteral("land-failed") : file.reason;
  }

  out.landed = landed.join(QLatin1Char(';'));
  if (out.landed.isEmpty())
    out.failReason = firstFail.isEmpty() ? QStringLiteral("land-failed") : firstFail;
  return out;
}

/// @p landed empty must not be reported as result=copy. On success the
/// detail carries the real destination path.
inline QString remoteDragOutcomeDetail(
    const char *waitReason, int pathCount, bool skipAppearGrace, const QString &landed, const QString &failReason
)
{
  const int grace = skipAppearGrace ? 0 : 1;
  if (landed.isEmpty()) {
    const QString why = failReason.isEmpty() ? QStringLiteral("land-failed") : failReason;
    return QStringLiteral("result=none reason=land-failed paths=") + QString::number(pathCount) +
           QStringLiteral(" appear_grace=") + QString::number(grace) + QStringLiteral(" detail=") + why;
  }
  const char *wait = (waitReason != nullptr && waitReason[0] != '\0') ? waitReason : "copy";
  return QStringLiteral("result=copy reason=") + QString::fromLatin1(wait) + QStringLiteral(" paths=") +
         QString::number(pathCount) + QStringLiteral(" appear_grace=") + QString::number(grace) +
         QStringLiteral(" landed=") + landed;
}

inline QString dropFileTraceDetail(
    const QString &windowClass, const QString &folder, const PlacedFiles &placed, const char *waitReason
)
{
  const QString cls = windowClass.isEmpty() ? QStringLiteral("-") : windowClass;
  const QString dir = folder.isEmpty() ? QStringLiteral("-") : folder;
  const QString landed = placed.landed.isEmpty() ? QStringLiteral("-") : placed.landed;
  QString why;
  if (placed.landed.isEmpty()) {
    const QString detail = placed.failReason.isEmpty() ? QStringLiteral("land-failed") : placed.failReason;
    why = QStringLiteral("land-failed detail=") + detail;
  } else {
    const char *wait = (waitReason != nullptr && waitReason[0] != '\0') ? waitReason : "copy";
    why = QString::fromLatin1(wait);
  }
  return QStringLiteral("class=") + cls + QStringLiteral(" dir=") + dir + QStringLiteral(" landed=") + landed +
         QStringLiteral(" reason=") + why;
}

} // namespace litekvm
