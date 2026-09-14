// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// ClipTransferDialog — 跨机文件剪贴板的传输进度窗口（进度条 + 速度/剩余时间 + 取消）。
//
// 计划（docs/plans/2026-08-25-litekvm-mvp.md Phase 4.1）要求 >100MB 的传输显示
// 进度条且可以取消。小于阈值的传输没必要挡用户，MainWindow 用托盘提示代替本窗口。
//
// 只依赖 Qt，不认识 litekvm 的类型 —— 调用方把 litekvm::TransferProgress 拆成
// bytesDone/bytesTotal 再喂进来，这样底层类改名也不影响本文件。
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QString>

class QCloseEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace deskflow::gui {

class ClipTransferDialog : public QDialog
{
  Q_OBJECT

public:
  enum class Direction
  {
    Send,
    Receive
  };

  /// 弹进度窗口的阈值：100 MiB
  static constexpr qint64 kProgressDialogThresholdBytes = qint64(100) * 1024 * 1024;

  /// @return true 当 totalBytes 超过阈值、值得弹进度窗口
  static bool needsProgressDialog(quint64 totalBytes);
  /// @return 人类可读的字节数，例如 "1.2 MB"
  static QString formatBytes(quint64 bytes);

  explicit ClipTransferDialog(QWidget *parent = nullptr);

  /// 开始一次传输：重置进度、启动计时与刷新定时器。
  void begin(Direction direction, const QString &peerName, quint64 totalBytes);
  /// 传输过程中刷新进度。
  void setProgress(quint64 bytesDone, quint64 bytesTotal);
  /// 传输结束状态（保留百分比，按钮变成「关闭」）。
  void finishCompleted(const QString &message);
  void finishCancelled(const QString &message);
  void finishFailed(const QString &message);

  [[nodiscard]] bool transferRunning() const
  {
    return m_running;
  }

Q_SIGNALS:
  /// 用户点了「取消」，或传输中试图关掉窗口。
  void cancelRequested();

protected:
  void changeEvent(QEvent *event) override;
  void closeEvent(QCloseEvent *event) override;

private:
  void retranslate();
  void refreshProgressText();
  QString speedText() const;

  QLabel *m_lblTitle = nullptr;
  QLabel *m_lblDetail = nullptr;
  QProgressBar *m_bar = nullptr;
  QPushButton *m_btnCancel = nullptr;
  QPushButton *m_btnClose = nullptr;

  Direction m_direction = Direction::Send;
  QString m_peerName;
  quint64 m_total = 0;
  quint64 m_done = 0;
  bool m_running = false;
  QString m_endMessage;
  QElapsedTimer m_elapsed;
  QTimer *m_tick = nullptr;
};

} // namespace deskflow::gui
