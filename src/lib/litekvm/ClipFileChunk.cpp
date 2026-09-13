// SPDX-License-Identifier: GPL-2.0
// LiteKVM cross-machine file clipboard — Qt-free wire-format core.
//
// Spec: docs/plans/2026-08-25-litekvm-mvp.md Phase 4 Task 4.1
#include "ClipFileChunk.h"

#include <cstring>
#include <utility>

namespace litekvm {

namespace {

inline std::uint32_t rotl32(std::uint32_t v, int bits)
{
  return (v << bits) | (v >> (32 - bits));
}

bool isAsciiAlpha(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isWindowsForbidden(char c)
{
  switch (c) {
  case '<':
  case '>':
  case ':':
  case '"':
  case '/':
  case '\\':
  case '|':
  case '?':
  case '*':
    return true;
  default:
    return static_cast<unsigned char>(c) < 0x20;
  }
}

} // namespace

// ------------------------------------------------------------------- sha1

namespace clipsha1 {

Sha1::Sha1()
{
  reset();
}

void Sha1::reset()
{
  m_h[0] = 0x67452301u;
  m_h[1] = 0xEFCDAB89u;
  m_h[2] = 0x98BADCFEu;
  m_h[3] = 0x10325476u;
  m_h[4] = 0xC3D2E1F0u;
  m_total = 0;
  m_blockLen = 0;
  m_done = false;
  m_digest.fill(0);
  std::memset(m_block, 0, sizeof(m_block));
}

void Sha1::update(const void *data, std::size_t len)
{
  if (m_done) // a finished object starts a fresh stream, never mixes streams
    reset();
  if (len == 0 || data == nullptr)
    return;
  m_total += len;
  absorb(static_cast<const std::uint8_t *>(data), len);
}

void Sha1::absorb(const std::uint8_t *data, std::size_t len)
{
  std::size_t i = 0;

  if (m_blockLen > 0) {
    while (m_blockLen < BlockBytes && i < len)
      m_block[m_blockLen++] = data[i++];
    if (m_blockLen == BlockBytes) {
      processBlock(m_block);
      m_blockLen = 0;
    }
  }
  while (len - i >= BlockBytes) {
    processBlock(data + i);
    i += BlockBytes;
  }
  while (i < len)
    m_block[m_blockLen++] = data[i++];
}

void Sha1::processBlock(const std::uint8_t *block)
{
  std::uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = be32Read(block + 4 * i);
  for (int i = 16; i < 80; ++i)
    w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  std::uint32_t a = m_h[0];
  std::uint32_t b = m_h[1];
  std::uint32_t c = m_h[2];
  std::uint32_t d = m_h[3];
  std::uint32_t e = m_h[4];

  for (int i = 0; i < 80; ++i) {
    std::uint32_t f = 0;
    std::uint32_t k = 0;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    const std::uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl32(b, 30);
    b = a;
    a = temp;
  }

  m_h[0] += a;
  m_h[1] += b;
  m_h[2] += c;
  m_h[3] += d;
  m_h[4] += e;
}

const Sha1Digest &Sha1::final()
{
  if (m_done)
    return m_digest;

  const std::uint64_t bitLen = m_total * 8;
  std::uint8_t pad[128] = {};
  pad[0] = 0x80;
  const std::size_t rem = static_cast<std::size_t>(m_total % BlockBytes);
  const std::size_t zeroPad = (rem < 56) ? (56 - rem - 1) : (120 - rem - 1);
  be64Write(pad + 1 + zeroPad, bitLen);
  absorb(pad, 1 + zeroPad + 8);

  for (int i = 0; i < 5; ++i) {
    m_digest[static_cast<std::size_t>(i) * 4 + 0] = std::uint8_t((m_h[i] >> 24) & 0xFF);
    m_digest[static_cast<std::size_t>(i) * 4 + 1] = std::uint8_t((m_h[i] >> 16) & 0xFF);
    m_digest[static_cast<std::size_t>(i) * 4 + 2] = std::uint8_t((m_h[i] >> 8) & 0xFF);
    m_digest[static_cast<std::size_t>(i) * 4 + 3] = std::uint8_t(m_h[i] & 0xFF);
  }
  m_done = true;
  return m_digest;
}

std::string Sha1::hex()
{
  return litekvm::clipsha1::hex(final());
}

Sha1Digest digest(const void *data, std::size_t len)
{
  Sha1 h;
  h.update(data, len);
  return h.final();
}

std::string hex(const Sha1Digest &d)
{
  static const char *kDigits = "0123456789abcdef";
  std::string out;
  out.resize(d.size() * 2);
  for (std::size_t i = 0; i < d.size(); ++i) {
    out[i * 2] = kDigits[(d[i] >> 4) & 0x0F];
    out[i * 2 + 1] = kDigits[d[i] & 0x0F];
  }
  return out;
}

std::string hex(const std::uint8_t *data, std::size_t len)
{
  Sha1 h;
  h.update(data, len);
  return hex(h.final());
}

bool fromHex(std::string_view hexText, Sha1Digest *out)
{
  if (!out || hexText.size() != kClipSha1Bytes * 2)
    return false;
  auto value = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < kClipSha1Bytes; ++i) {
    const int hi = value(hexText[i * 2]);
    const int lo = value(hexText[i * 2 + 1]);
    if (hi < 0 || lo < 0)
      return false;
    (*out)[i] = std::uint8_t((hi << 4) | lo);
  }
  return true;
}

} // namespace clipsha1

// -------------------------------------------------------------- endianness

std::uint16_t be16Read(const std::uint8_t *p)
{
  return std::uint16_t((std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]));
}

std::uint32_t be32Read(const std::uint8_t *p)
{
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
         std::uint32_t(p[3]);
}

std::uint64_t be64Read(const std::uint8_t *p)
{
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | std::uint64_t(p[i]);
  return v;
}

void be16Write(std::uint8_t *p, std::uint16_t v)
{
  p[0] = std::uint8_t((v >> 8) & 0xFF);
  p[1] = std::uint8_t(v & 0xFF);
}

void be32Write(std::uint8_t *p, std::uint32_t v)
{
  p[0] = std::uint8_t((v >> 24) & 0xFF);
  p[1] = std::uint8_t((v >> 16) & 0xFF);
  p[2] = std::uint8_t((v >> 8) & 0xFF);
  p[3] = std::uint8_t(v & 0xFF);
}

void be64Write(std::uint8_t *p, std::uint64_t v)
{
  for (int i = 0; i < 8; ++i)
    p[i] = std::uint8_t((v >> (56 - 8 * i)) & 0xFF);
}

// ------------------------------------------------------------------ frames

std::vector<std::uint8_t> encodeFrame(ClipFrameKind kind, const std::uint8_t *payload,
                                     std::size_t payloadLen)
{
  const bool isControl = kind == ClipFrameKind::Control;
  const std::size_t maxPayload = isControl ? kClipMaxControlPayload : kClipMaxDataPayload;

  if (payloadLen == 0 || payloadLen > maxPayload)
    return {};
  if (payload == nullptr)
    return {};

  const std::size_t lenBytes = isControl ? kClipControlLengthBytes : kClipDataLengthBytes;
  std::vector<std::uint8_t> frame;
  frame.resize(kClipKindBytes + lenBytes + payloadLen);
  frame[0] = static_cast<std::uint8_t>(kind);
  if (isControl)
    be16Write(frame.data() + kClipKindBytes, static_cast<std::uint16_t>(payloadLen));
  else
    be32Write(frame.data() + kClipKindBytes, static_cast<std::uint32_t>(payloadLen));
  std::memcpy(frame.data() + kClipKindBytes + lenBytes, payload, payloadLen);
  return frame;
}

std::vector<std::uint8_t> encodeControlFrame(std::string_view jsonUtf8)
{
  return encodeFrame(ClipFrameKind::Control, reinterpret_cast<const std::uint8_t *>(jsonUtf8.data()),
                     jsonUtf8.size());
}

std::vector<std::uint8_t> encodeDataFrame(const std::uint8_t *header, std::size_t headerLen,
                                          const std::uint8_t *data, std::size_t dataLen)
{
  if (header == nullptr || headerLen != kClipChunkHeaderBytes)
    return {};
  if (dataLen > kClipMaxChunkSize)
    return {};
  if (dataLen > 0 && data == nullptr)
    return {};

  const std::size_t payloadLen = headerLen + dataLen;
  std::vector<std::uint8_t> frame;
  frame.resize(kClipKindBytes + kClipDataLengthBytes + payloadLen);
  frame[0] = static_cast<std::uint8_t>(ClipFrameKind::Data);
  be32Write(frame.data() + kClipKindBytes, static_cast<std::uint32_t>(payloadLen));
  std::memcpy(frame.data() + kClipKindBytes + kClipDataLengthBytes, header, headerLen);
  if (dataLen > 0)
    std::memcpy(frame.data() + kClipKindBytes + kClipDataLengthBytes + headerLen, data, dataLen);
  return frame;
}

// ------------------------------------------------------- incremental decoder

FrameDecoder::FrameDecoder(std::size_t maxControlPayload, std::size_t maxDataPayload)
  : m_maxControl(maxControlPayload), m_maxData(maxDataPayload)
{
}

void FrameDecoder::append(const std::uint8_t *data, std::size_t len)
{
  if (data == nullptr || len == 0)
    return;
  if (m_start > 0 && m_start >= m_buffer.size()) {
    m_buffer.clear();
    m_start = 0;
  }
  m_buffer.insert(m_buffer.end(), data, data + len);
}

void FrameDecoder::append(const std::vector<std::uint8_t> &data)
{
  append(data.data(), data.size());
}

bool FrameDecoder::fail(std::string reason)
{
  m_failed = true;
  m_failure = std::move(reason);
  return false;
}

bool FrameDecoder::next(ClipFrame *out)
{
  if (m_failed || out == nullptr)
    return false;

  if (available() < kClipKindBytes)
    return false;

  const std::uint8_t kindByte = m_buffer[m_start];
  const bool isControl = kindByte == static_cast<std::uint8_t>(ClipFrameKind::Control);
  const bool isData = kindByte == static_cast<std::uint8_t>(ClipFrameKind::Data);
  if (!isControl && !isData) {
    return fail("unknown frame kind byte " + std::to_string(unsigned(kindByte)));
  }

  const std::size_t lenBytes = isControl ? kClipControlLengthBytes : kClipDataLengthBytes;
  if (available() < kClipKindBytes + lenBytes)
    return false;

  const std::uint8_t *lenField = m_buffer.data() + m_start + kClipKindBytes;
  const std::size_t payloadLen =
      isControl ? std::size_t(be16Read(lenField)) : std::size_t(be32Read(lenField));
  const std::size_t maxPayload = isControl ? m_maxControl : m_maxData;

  if (payloadLen == 0)
    return fail("zero-length frame payload");
  if (payloadLen > maxPayload) {
    return fail("frame payload " + std::to_string(payloadLen) + " exceeds limit " +
                std::to_string(maxPayload));
  }

  const std::size_t total = kClipKindBytes + lenBytes + payloadLen;
  if (available() < total)
    return false;

  const std::uint8_t *bodyStart = lenField + lenBytes;
  out->kind = isControl ? ClipFrameKind::Control : ClipFrameKind::Data;
  out->payload.assign(bodyStart, bodyStart + payloadLen);

  m_start += total;
  if (m_start >= m_buffer.size()) {
    m_buffer.clear();
    m_start = 0;
  } else if (m_start >= 64 * 1024) {
    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_start));
    m_start = 0;
  }
  return true;
}

void FrameDecoder::reset()
{
  m_buffer.clear();
  m_start = 0;
  m_failed = false;
  m_failure.clear();
}

// ------------------------------------------------------------- chunk header

bool decodeChunkHeader(const std::uint8_t *data, std::size_t len, ChunkHeader *out)
{
  if (data == nullptr || out == nullptr || len < kClipChunkHeaderBytes)
    return false;

  ChunkHeader h;
  h.seq = be32Read(data);
  h.offset = be64Read(data + 4);
  h.dataLen = be32Read(data + 12);
  if (h.dataLen > kClipMaxChunkSize)
    return false;
  std::memcpy(h.digest.data(), data + 16, kClipSha1Bytes);

  *out = h;
  return true;
}

std::vector<std::uint8_t> encodeChunkHeader(const ChunkHeader &header)
{
  if (header.dataLen > kClipMaxChunkSize)
    return {};
  std::vector<std::uint8_t> out(kClipChunkHeaderBytes, 0);
  be32Write(out.data(), header.seq);
  be64Write(out.data() + 4, header.offset);
  be32Write(out.data() + 12, header.dataLen);
  std::memcpy(out.data() + 16, header.digest.data(), kClipSha1Bytes);
  return out;
}

bool makeChunkHeader(std::uint32_t seq, std::uint64_t offset, const std::uint8_t *data,
                     std::size_t dataLen, ChunkHeader *out)
{
  if (out == nullptr || dataLen > kClipMaxChunkSize)
    return false;
  if (dataLen > 0 && data == nullptr)
    return false;

  ChunkHeader h;
  h.seq = seq;
  h.offset = offset;
  h.dataLen = static_cast<std::uint32_t>(dataLen);
  h.digest = clipsha1::digest(data, dataLen);
  *out = h;
  return true;
}

std::vector<std::uint8_t> encodeDataPayload(const ChunkHeader &header, const std::uint8_t *data)
{
  if (header.dataLen > kClipMaxChunkSize)
    return {};
  if (header.dataLen > 0 && data == nullptr)
    return {};

  std::vector<std::uint8_t> out(kClipChunkHeaderBytes + std::size_t(header.dataLen), 0);
  be32Write(out.data(), header.seq);
  be64Write(out.data() + 4, header.offset);
  be32Write(out.data() + 12, header.dataLen);
  std::memcpy(out.data() + 16, header.digest.data(), kClipSha1Bytes);
  if (header.dataLen > 0)
    std::memcpy(out.data() + kClipChunkHeaderBytes, data, std::size_t(header.dataLen));
  return out;
}

bool decodeDataPayload(const std::vector<std::uint8_t> &payload, ChunkHeader *header,
                       const std::uint8_t **data, std::size_t *dataLen)
{
  if (header == nullptr || data == nullptr || dataLen == nullptr)
    return false;
  if (!decodeChunkHeader(payload.data(), payload.size(), header))
    return false;

  const std::size_t expected = kClipChunkHeaderBytes + std::size_t(header->dataLen);
  if (payload.size() != expected)
    return false;

  *data = payload.data() + kClipChunkHeaderBytes;
  *dataLen = std::size_t(header->dataLen);
  return true;
}

bool chunkDigestMatches(const ChunkHeader &header, const std::uint8_t *data)
{
  if (header.dataLen > 0 && data == nullptr)
    return false;
  return clipsha1::digest(data, std::size_t(header.dataLen)) == header.digest;
}

// -------------------------------------------------------------- chunk plan

ChunkPlan::ChunkPlan(std::uint64_t fileSize, std::size_t chunkSize)
  : m_fileSize(fileSize), m_chunkSize(chunkSize)
{
  if (chunkSize < kClipMinChunkSize || chunkSize > kClipMaxChunkSize) {
    m_valid = false;
    m_count = 0;
    return;
  }
  if (fileSize > kClipMaxFileBytes) {
    m_valid = false;
    m_count = 0;
    return;
  }
  m_count = (fileSize == 0) ? 1 : (fileSize + chunkSize - 1) / chunkSize;
  if (m_count > 0xFFFFFFFFull) { // seq is 32-bit on the wire
    m_valid = false;
    m_count = 0;
  }
}

std::uint64_t ChunkPlan::offset(std::uint64_t index) const
{
  if (index >= m_count)
    return m_fileSize;
  return index * m_chunkSize;
}

std::size_t ChunkPlan::length(std::uint64_t index) const
{
  if (index >= m_count)
    return 0;
  const std::uint64_t start = offset(index);
  const std::uint64_t remaining = m_fileSize - start;
  return static_cast<std::size_t>(remaining < m_chunkSize ? remaining : m_chunkSize);
}

std::uint64_t ChunkPlan::bytesCovered() const
{
  std::uint64_t sum = 0;
  for (std::uint64_t i = 0; i < m_count; ++i)
    sum += length(i);
  return sum;
}

bool ChunkPlan::seqForOffset(std::uint64_t byteOffset, std::uint32_t *seq) const
{
  if (!m_valid || seq == nullptr || byteOffset > m_fileSize)
    return false;
  if (byteOffset % m_chunkSize != 0)
    return false;
  // index == m_count only for a resume point exactly at EOF (nothing to send).
  const std::uint64_t index = byteOffset / m_chunkSize;
  if (index > m_count)
    return false;
  *seq = static_cast<std::uint32_t>(index);
  return true;
}

bool ChunkPlan::isChunkAligned(std::uint64_t byteOffset, std::size_t chunkSize)
{
  if (chunkSize == 0)
    return false;
  return byteOffset % chunkSize == 0;
}

// ---------------------------------------------------------------- progress

double TransferProgress::ratio() const
{
  if (bytesTotal == 0)
    return 1.0; // nothing to send: already "done"
  if (bytesDone >= bytesTotal)
    return 1.0;
  return double(bytesDone) / double(bytesTotal);
}

int TransferProgress::percent() const
{
  const double r = ratio();
  int p = int(r * 100.0 + 0.5);
  if (p < 0)
    p = 0;
  if (p > 100)
    p = 100;
  return p;
}

// ------------------------------------------------------------- path helpers

std::string normalizeRelativePath(std::string_view utf8Path)
{
  // split on both separators; drop empty and "." components
  std::string out;
  out.reserve(utf8Path.size());
  std::size_t i = 0;
  while (i <= utf8Path.size()) {
    std::size_t j = utf8Path.find_first_of("/\\", i);
    if (j == std::string_view::npos)
      j = utf8Path.size();
    const std::string_view comp = utf8Path.substr(i, j - i);
    if (!comp.empty() && comp != ".") {
      if (!out.empty())
        out.push_back('/');
      out.append(comp.data(), comp.size());
    }
    if (j >= utf8Path.size())
      break;
    i = j + 1;
  }
  return out;
}

bool isSafeRelativePath(std::string_view utf8RelPath)
{
  if (utf8RelPath.empty() || utf8RelPath.size() > 4096)
    return false;
  if (utf8RelPath.find('\0') != std::string_view::npos)
    return false;

  const char first = utf8RelPath.front();
  if (first == '/' || first == '\\')
    return false;
  if (utf8RelPath.size() >= 2 && utf8RelPath[1] == ':' && isAsciiAlpha(first))
    return false;

  std::size_t i = 0;
  while (i <= utf8RelPath.size()) {
    std::size_t j = utf8RelPath.find_first_of("/\\", i);
    if (j == std::string_view::npos)
      j = utf8RelPath.size();
    const std::string_view comp = utf8RelPath.substr(i, j - i);
    if (comp.empty() || comp == "." || comp == "..")
      return false;
    if (comp.size() > 255)
      return false;
    if (j >= utf8RelPath.size())
      break;
    i = j + 1;
  }
  return true;
}

std::string sanitizeFileName(std::string_view utf8Name)
{
  std::string out;
  out.reserve(utf8Name.size());
  for (const char c : utf8Name) {
    if (isWindowsForbidden(c))
      out.push_back('_');
    else
      out.push_back(c);
  }
  while (!out.empty()) {
    const char last = out.back();
    if (last == '.' || last == ' ' || last == '\t')
      out.pop_back();
    else
      break;
  }
  bool allDots = !out.empty();
  for (const char c : out) {
    if (c != '.') {
      allDots = false;
      break;
    }
  }
  if (allDots)
    return {};
  return out;
}

std::string parentDirOf(std::string_view utf8RelPath)
{
  const std::size_t pos = utf8RelPath.find_last_of('/');
  if (pos == std::string_view::npos)
    return {};
  return std::string(utf8RelPath.substr(0, pos));
}

std::string partPathFor(std::string_view utf8RelPath)
{
  std::string out(utf8RelPath);
  out.append(kClipPartSuffix);
  return out;
}

} // namespace litekvm
