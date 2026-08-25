// SPDX-License-Identifier: GPL-2.0
// LiteKVM mesh layout sync — control-plane TCP/25902.
//
// Spec: docs/mesh-layout-sync-spec.md §3-§4 (litekvm-design repo)
#pragma once

#include "DeviceIdentity.h"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <optional>
#include <vector>

namespace litekvm {

struct ScreenRect {
  QString deviceId;
  int x = 0;
  int y = 0;
  int w = 1920;
  int h = 1080;
};

struct ScreenLayout {
  qint64 rev = 0;        // monotonic, LWW key
  QString updatedBy;     // device_id of last writer
  std::vector<ScreenRect> screens;
};

/**
 * @brief Full-mesh control plane: floods LAYOUT_UPDATE frames, converges by
 * rev (last-write-wins), pulls newcomers up to date on connect.
 */
class LayoutSync : public QObject {
  Q_OBJECT

public:
  explicit LayoutSync(const DeviceIdentity &self, QObject *parent = nullptr);

  bool start(quint16 port = 25902);
  void stop();
  void connectToPeer(const QString &host, quint16 port = 25902);

  /// publish a new layout (rev auto-incremented)
  void publish(ScreenLayout layout);

  const std::optional<ScreenLayout> &layout() const
  {
    return m_layout;
  }
  qint64 lastRev() const
  {
    return m_lastRev;
  }

  static QByteArray encodeFrame(const QJsonObject &obj);
  static std::optional<QJsonObject> decodeFrame(const QByteArray &payload);
  static QJsonObject layoutToJson(const ScreenLayout &layout);
  static std::optional<ScreenLayout> layoutFromJson(const QJsonObject &obj);

Q_SIGNALS:
  void layoutChanged(const litekvm::ScreenLayout &layout);

private Q_SLOTS:
  void onReadyRead();

private:
  void applyIncoming(const ScreenLayout &incoming);
  void broadcast(const QJsonObject &msg);
  void startHeartbeat();

  const DeviceIdentity &m_self;
  QTcpServer m_server;
  QSet<QTcpSocket *> m_peers;
  QMap<QTcpSocket *, QByteArray> m_buffers;

  std::optional<ScreenLayout> m_layout;
  qint64 m_lastRev = 0;
  QTimer *m_heartbeat = nullptr;
};

} // namespace litekvm
