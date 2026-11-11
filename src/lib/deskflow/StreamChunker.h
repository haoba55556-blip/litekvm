/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardTypes.h"

#include <deque>
#include <memory>
#include <string>
#include <string_view>

class IEventQueue;
class EventQueueTimer;
namespace deskflow {
class IStream;
}

// Sends clipboard bytes as separate protocol messages, one per event-loop
// turn, so a mouse message can be written before the rest of a large
// clipboard. The timer is only the idle gap; queued mouse events run first.
class ClipboardYieldPump
{
public:
  ClipboardYieldPump();
  ~ClipboardYieldPump();

  ClipboardYieldPump(const ClipboardYieldPump &) = delete;
  ClipboardYieldPump &operator=(const ClipboardYieldPump &) = delete;

  void bind(IEventQueue *events, deskflow::IStream *stream);
  void stop();
  void enqueue(std::string data, ClipboardID id, uint32_t sequence);

private:
  struct Job;
  void writeOneAndArm();
  void onTimer();

  IEventQueue *m_events = nullptr;
  deskflow::IStream *m_stream = nullptr;
  EventQueueTimer *m_timer = nullptr;
  std::deque<std::unique_ptr<Job>> m_jobs;
};

class StreamChunker
{
public:
  static void
  sendClipboard(const std::string_view &data, size_t size, ClipboardID id, uint32_t sequence, ClipboardYieldPump &pump);
};
