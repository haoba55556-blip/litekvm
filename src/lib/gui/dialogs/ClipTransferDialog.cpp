// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
#include "ClipTransferDialog.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace deskflow::gui {

ClipTransferDialog::ClipTransferDialog(QWidget *parent) : QDialog(parent)
{
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setModal(false);
  setMinimumWidth(420);

  auto *layout = new QVBoxLayout(this);

  m_lblTitle = new QLabel(this);
  m_lblTitle->setStyleSheet(QStringLiteral("font-weight: bold;"));
  layout->addWidget(m_lblTitle);

  m_bar = new QProgressBar(this);
  m_bar->setRange(0, 1000);
  m_bar->setTextVisible(false);
  layout->addWidget(m_bar);

  m_lblDetail = new QLabel(this);
  m_lblDetail->setWordWrap(true);
  layout->addWidget(m_lblDetail);

  auto *buttons = new QHBoxLayout();
  buttons->addStretch();
  m_btnCancel = new QPushButton(this);
  m_btnClose = new QPushButton(this);
  m_btnClose->setVisible(false);
  buttons->addWidget(m_btnCancel);
  buttons->addWidget(m_btnClose);
  layout->addLayout(buttons);

  m_tick = new QTimer(this);
  m_tick->setInterval(1000);
  connect(m_tick, &QTimer::timeout, this, &ClipTransferDialog::refreshProgressText);

  connect(m_btnCancel, &QPushButton::clicked, this, [this] {
    if (!m_running)
      return;
    Q_EMIT cancelRequested();
  });
  connect(m_btnClose, &QPushButton::clicked, this, &QDialog::close);

  retranslate();
}

bool ClipTransferDialog::needsProgressDialog(quint64 totalBytes)
{
  return totalBytes > quint64(kProgressDialogThresholdBytes);
}

QString ClipTransferDialog::formatBytes(quint64 bytes)
{
  static const QStringList units = {
      QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB"), QStringLiteral("TB")
  };

  if (bytes < 1024)
    return QStringLiteral("%1 B").arg(bytes);

  double value = double(bytes) / 1024.0;
  int unit = 0;
  while (value >= 1024.0 && unit < units.size() - 1) {
    value /= 1024.0;
    ++unit;
  }
  return QStringLiteral("%1 %2").arg(QString::number(value, 'f', value >= 100.0 ? 0 : 1), units.at(unit));
}

void ClipTransferDialog::begin(Direction direction, const QString &peerName, quint64 totalBytes)
{
  m_direction = direction;
  m_peerName = peerName;
  m_total = totalBytes;
  m_done = 0;
  m_running = true;
  m_endMessage.clear();

  m_elapsed.start();
  m_tick->start();

  retranslate();
  refreshProgressText();
}

void ClipTransferDialog::setProgress(quint64 bytesDone, quint64 bytesTotal)
{
  if (!m_running)
    return;
  m_done = bytesDone;
  if (bytesTotal > 0)
    m_total = bytesTotal;
  refreshProgressText();
}

void ClipTransferDialog::finishCompleted(const QString &message)
{
  m_running = false;
  m_tick->stop();
  if (m_total == 0)
    m_total = m_done;
  m_done = m_total;
  m_endMessage = message;
  retranslate();
  refreshProgressText();
}

void ClipTransferDialog::finishCancelled(const QString &message)
{
  m_running = false;
  m_tick->stop();
  m_endMessage = message;
  retranslate();
  refreshProgressText();
}

void ClipTransferDialog::finishFailed(const QString &message)
{
  m_running = false;
  m_tick->stop();
  m_endMessage = message;
  retranslate();
  refreshProgressText();
}

void ClipTransferDialog::changeEvent(QEvent *event)
{
  QDialog::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    retranslate();
    refreshProgressText();
  }
}

void ClipTransferDialog::closeEvent(QCloseEvent *event)
{
  // 传输中关窗口等于取消：先告诉调用方，等它真的取消完再关（finishCancelled）。
  if (m_running) {
    Q_EMIT cancelRequested();
    event->ignore();
    return;
  }
  QDialog::closeEvent(event);
}

void ClipTransferDialog::retranslate()
{
  const QString peer = m_peerName.isEmpty() ? tr("对端") : m_peerName;

  setWindowTitle(m_direction == Direction::Send ? tr("发送文件") : tr("接收文件"));
  m_lblTitle->setText(
      m_direction == Direction::Send ? tr("正在发送文件到「%1」").arg(peer) : tr("正在接收来自「%1」的文件").arg(peer)
  );
  m_btnCancel->setText(tr("取消"));
  m_btnClose->setText(tr("关闭"));

  const bool running = m_running;
  m_btnCancel->setVisible(running);
  m_btnClose->setVisible(!running);
  if (!running)
    m_btnClose->setFocus();
}

void ClipTransferDialog::refreshProgressText()
{
  const double ratio = m_total > 0 ? double(m_done) / double(m_total) : (m_running ? 0.0 : 1.0);
  const int permille = qBound(0, int(ratio * 1000.0 + 0.5), 1000);
  m_bar->setValue(permille);

  if (m_running) {
    m_lblDetail->setText(tr("%1% · %2 / %3%4").arg(permille / 10).arg(formatBytes(m_done), formatBytes(m_total), speedText()));
  } else {
    m_lblDetail->setText(m_endMessage);
  }
}

QString ClipTransferDialog::speedText() const
{
  const qint64 ms = m_elapsed.elapsed();
  if (ms < 250 || m_done == 0)
    return {};

  const double bytesPerSecond = double(m_done) / (double(ms) / 1000.0);
  QString text = QStringLiteral(" · ") + tr("%1/s").arg(formatBytes(quint64(bytesPerSecond)));

  const quint64 remaining = m_total > m_done ? m_total - m_done : 0;
  if (bytesPerSecond > 1.0 && remaining > 0) {
    const int eta = int(double(remaining) / bytesPerSecond);
    text += QStringLiteral(" · ") + tr("剩余约 %1 秒").arg(eta < 1 ? 1 : eta);
  }
  return text;
}

} // namespace deskflow::gui
