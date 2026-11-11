/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/StreamChunker.h"

#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ClipboardYield.h"
#include "io/IStream.h"

#include <string>
#include <utility>

namespace {

void writeClipboardPiece(
    deskflow::IStream *stream, ClipboardID id, uint32_t sequence, const std::string &payload, int piece, size_t offset,
    size_t size
)
{
  ClipboardChunk *chunk = nullptr;
  if (piece == 0) {
    chunk = ClipboardChunk::start(id, sequence, std::to_string(payload.size()));
  } else if (piece == 1) {
    chunk = ClipboardChunk::data(id, sequence, payload.substr(offset, size));
  } else {
    chunk = ClipboardChunk::end(id, sequence);
  }
  ClipboardChunk::send(stream, chunk);
  delete chunk;
}

} // namespace

struct ClipboardYieldPump::Job
{
  std::string data;
  ClipboardID id = 0;
  uint32_t sequence = 0;
  ClipboardPieceCursor cursor;

  Job(std::string bytes, ClipboardID clipboardId, uint32_t seq)
      : data(std::move(bytes)),
        id(clipboardId),
        sequence(seq),
        cursor(data.size())
  {
  }
};

// Defined here, after Job is complete. An inline default constructor makes
// libstdc++ instantiate unique_ptr's deleter in every including TU, and
// MinGW 13.1 rejects sizeof on the incomplete Job.
ClipboardYieldPump::ClipboardYieldPump() = default;

ClipboardYieldPump::~ClipboardYieldPump()
{
  stop();
}

void ClipboardYieldPump::bind(IEventQueue *events, deskflow::IStream *stream)
{
  stop();
  m_events = events;
  m_stream = stream;
}

void ClipboardYieldPump::stop()
{
  if (m_events != nullptr && m_timer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_timer);
    m_events->deleteTimer(m_timer);
  }
  m_timer = nullptr;
  m_jobs.clear();
}

void ClipboardYieldPump::enqueue(std::string data, ClipboardID id, uint32_t sequence)
{
  if (m_events == nullptr || m_stream == nullptr) {
    LOG_WARN("clipboard %d dropped; yield pump is not bound", id);
    return;
  }
  LOG_DEBUG("clipboard %d queued (%zu bytes), one %zu-byte piece per turn", id, data.size(), kClipboardChunkBytes);
  const bool idle = m_timer == nullptr;
  m_jobs.push_back(std::make_unique<Job>(std::move(data), id, sequence));
  if (idle)
    writeOneAndArm();
}

void ClipboardYieldPump::writeOneAndArm()
{
  if (m_stream == nullptr || m_events == nullptr || m_timer != nullptr)
    return;

  while (!m_jobs.empty()) {
    Job &job = *m_jobs.front();
    int piece = 0;
    size_t offset = 0;
    size_t size = 0;
    if (!job.cursor.takePiece(piece, offset, size)) {
      m_jobs.pop_front();
      continue;
    }

    writeClipboardPiece(m_stream, job.id, job.sequence, job.data, piece, offset, size);
    if (job.cursor.finished()) {
      LOG_DEBUG("clipboard %d send finished", job.id);
      m_jobs.pop_front();
    }
    break;
  }
  if (m_jobs.empty())
    return;

  // One piece, then back to the event loop. A mouse message queued during
  // this write, or that arrives before the timer, is written first.
  m_timer = m_events->newOneShotTimer(kClipboardYieldSeconds, nullptr);
  m_events->addHandler(EventTypes::Timer, m_timer, [this](const Event &) { onTimer(); });
}

void ClipboardYieldPump::onTimer()
{
  if (m_events != nullptr && m_timer != nullptr) {
    EventQueueTimer *timer = m_timer;
    m_timer = nullptr;
    m_events->removeHandler(EventTypes::Timer, timer);
    m_events->deleteTimer(timer);
  }
  writeOneAndArm();
}

void StreamChunker::sendClipboard(
    const std::string_view &data, size_t size, ClipboardID id, uint32_t sequence, ClipboardYieldPump &pump
)
{
  if (size > data.size())
    size = data.size();
  pump.enqueue(std::string(data.substr(0, size)), id, sequence);
}
