// SPDX-License-Identifier: GPL-2.0
// LiteKVM cross-machine file clipboard — Qt-free wire-format core.
//
// Spec: docs/plans/2026-08-25-litekvm-mvp.md Phase 4 Task 4.1
//       (metadata + 256 KiB chunks + sha1 verification, progress + cancel)
//
// ---------------------------------------------------------------------------
// Framing. PairingService::objToFrame() and LayoutSync::encodeFrame() prefix a
// JSON payload with a 2-byte big-endian length. That convention cannot carry a
// 256 KiB chunk (16 bits cap the payload at 64 KiB), so this module keeps a
// leading frame-kind byte and uses the same big-endian length prefix with a
// width that fits the payload kind:
//
//   control frame: [0x01][len_hi len_lo 2B BE][UTF-8 JSON, len bytes]
//                  ^ byte-for-byte the PairingService frame shape
//   data frame:    [0x02][len 4B BE][chunk header 36B][chunk bytes]
//
// The length prefix never counts the kind byte or the prefix bytes themselves
// (identical semantics to PairingService's `frame.append(size>>8)...`), so a
// control frame here is exactly `[PairingService prefix + JSON]` with a 0x01
// marker in front of it.
//
// Data payload layout (kClipChunkHeaderBytes == 36, fields big-endian):
//
//   seq       4 bytes   chunk sequence number, 0-based, chunk-aligned
//   offset    8 bytes   byte offset of this chunk inside the file
//   data_len  4 bytes   number of chunk bytes that follow (0 .. 4 MiB)
//   sha1     20 bytes   SHA-1 over exactly the data_len bytes
//   data    data_len bytes
//
// Control frame types (JSON, "type" field):
//   CLIP_HELLO / CLIP_OFFER / CLIP_ACCEPT / CLIP_REJECT /
//   CLIP_COMPLETE / CLIP_DONE / CLIP_ERROR / CLIP_CANCEL
//
// Everything in this header is plain C++20: no Qt, so it can be unit-tested
// and syntax-checked without a Qt toolchain.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace litekvm {

// --------------------------------------------------------------- constants

inline constexpr std::size_t kClipKindBytes = 1;
inline constexpr std::size_t kClipControlLengthBytes = 2; ///< control frames: as PairingService
inline constexpr std::size_t kClipDataLengthBytes = 4;    ///< data frames: 256 KiB+ needs >16 bits
inline constexpr std::size_t kClipDefaultChunkSize = 256 * 1024; ///< 256 KiB (plan Task 4.1)
inline constexpr std::size_t kClipMinChunkSize = 4 * 1024;
inline constexpr std::size_t kClipMaxChunkSize = 4 * 1024 * 1024;
inline constexpr std::size_t kClipSha1Bytes = 20;
inline constexpr std::size_t kClipChunkHeaderBytes = 4 + 8 + 4 + kClipSha1Bytes; // == 36
/// Control (JSON) payload cap — same order as PairingService's 64 KiB cap.
inline constexpr std::size_t kClipMaxControlPayload = 64 * 1024;
/// Largest data-frame payload: one full-size chunk plus its header.
inline constexpr std::size_t kClipMaxDataPayload = kClipChunkHeaderBytes + kClipMaxChunkSize;
/// Sanity cap for a single file, and the default cap for a whole transfer.
inline constexpr std::uint64_t kClipMaxFileBytes = std::uint64_t(1) << 40; // 1 TiB
inline constexpr std::uint64_t kClipDefaultMaxTransferBytes = std::uint64_t(2) << 30; // 2 GiB
inline constexpr const char *kClipPartSuffix = ".litekvm-part";

using Sha1Digest = std::array<std::uint8_t, kClipSha1Bytes>;

enum class ClipFrameKind : std::uint8_t {
  Control = 0x01, ///< payload is a UTF-8 JSON object
  Data = 0x02,    ///< payload is ChunkHeader || chunk bytes
};

struct ClipFrame {
  ClipFrameKind kind = ClipFrameKind::Control;
  std::vector<std::uint8_t> payload;
};

// ------------------------------------------------------------------- sha1

namespace clipsha1 {

/**
 * @brief Minimal portable SHA-1 (RFC 3174). Used only for chunk/file integrity
 * checks, never for authentication — the paired deskflow data channel is
 * already encrypted and authenticated.
 */
class Sha1 {
public:
  static constexpr std::size_t DigestBytes = kClipSha1Bytes;
  static constexpr std::size_t BlockBytes = 64;

  Sha1();

  void reset();
  void update(const void *data, std::size_t len);

  /// Idempotent: calling it twice returns the same digest.
  const Sha1Digest &final();

  std::string hex();

private:
  void absorb(const std::uint8_t *data, std::size_t len);
  void processBlock(const std::uint8_t *block);

  std::uint32_t m_h[5] = {0, 0, 0, 0, 0};
  std::uint64_t m_total = 0;
  std::uint8_t m_block[BlockBytes] = {};
  std::size_t m_blockLen = 0;
  bool m_done = false;
  Sha1Digest m_digest = {};
};

Sha1Digest digest(const void *data, std::size_t len);
std::string hex(const Sha1Digest &d);
std::string hex(const std::uint8_t *data, std::size_t len);
bool fromHex(std::string_view hexText, Sha1Digest *out);

} // namespace clipsha1

// -------------------------------------------------------------- endianness

std::uint16_t be16Read(const std::uint8_t *p);
std::uint32_t be32Read(const std::uint8_t *p);
std::uint64_t be64Read(const std::uint8_t *p);
void be16Write(std::uint8_t *p, std::uint16_t v);
void be32Write(std::uint8_t *p, std::uint32_t v);
void be64Write(std::uint8_t *p, std::uint64_t v);

// ------------------------------------------------------------------ frames

/// Serialises one frame. Returns an empty vector when the payload is oversized
/// for its kind (control > 64 KiB, data > 4 MiB + header) or empty-when-it-
/// cannot-be (a control frame needs at least 1 byte of JSON).
std::vector<std::uint8_t> encodeFrame(ClipFrameKind kind, const std::uint8_t *payload,
                                     std::size_t payloadLen);

/// Convenience overload for JSON control payloads.
std::vector<std::uint8_t> encodeControlFrame(std::string_view jsonUtf8);

/// Serialises one data frame from a header plus its chunk bytes.
/// @p data may be nullptr when header.dataLen == 0.
std::vector<std::uint8_t> encodeDataFrame(const std::uint8_t *header, std::size_t headerLen,
                                          const std::uint8_t *data, std::size_t dataLen);

// ------------------------------------------------------- incremental decoder

/**
 * @brief Incremental frame reassembler: append whatever the socket handed us,
 * then pull out complete frames one at a time. Mirrors the buffer logic of
 * PairingService::onReadyRead() but without Qt.
 */
class FrameDecoder {
public:
  explicit FrameDecoder(std::size_t maxControlPayload = kClipMaxControlPayload,
                        std::size_t maxDataPayload = kClipMaxDataPayload);

  void append(const std::uint8_t *data, std::size_t len);
  void append(const std::vector<std::uint8_t> &data);

  /// Pops the next complete frame. Returns false when more bytes are needed,
  /// or when the stream is broken (check failed()).
  bool next(ClipFrame *out);

  bool failed() const
  {
    return m_failed;
  }
  const std::string &failureReason() const
  {
    return m_failure;
  }

  std::size_t buffered() const
  {
    return m_buffer.size() - m_start;
  }
  std::size_t maxControlPayload() const
  {
    return m_maxControl;
  }
  std::size_t maxDataPayload() const
  {
    return m_maxData;
  }
  void reset();

private:
  bool fail(std::string reason);
  std::size_t available() const
  {
    return m_buffer.size() - m_start;
  }

  std::vector<std::uint8_t> m_buffer;
  std::size_t m_start = 0;
  std::size_t m_maxControl = kClipMaxControlPayload;
  std::size_t m_maxData = kClipMaxDataPayload;
  bool m_failed = false;
  std::string m_failure;
};

// ------------------------------------------------------------- chunk header

struct ChunkHeader {
  std::uint32_t seq = 0;
  std::uint64_t offset = 0;
  std::uint32_t dataLen = 0;
  Sha1Digest digest = {};

  bool operator==(const ChunkHeader &other) const = default;

  std::string digestHex() const
  {
    return clipsha1::hex(digest);
  }
};

/// Header fields only. False when the buffer is shorter than the header, or
/// when data_len exceeds kClipMaxChunkSize.
bool decodeChunkHeader(const std::uint8_t *data, std::size_t len, ChunkHeader *out);
std::vector<std::uint8_t> encodeChunkHeader(const ChunkHeader &header);

/// Builds a header for @p data, hashing it into digest and setting data_len.
bool makeChunkHeader(std::uint32_t seq, std::uint64_t offset, const std::uint8_t *data,
                     std::size_t dataLen, ChunkHeader *out);

/// ChunkHeader || chunk bytes, ready to become a Data frame payload, or to be
/// handed to encodeDataFrame(). Uses header.dataLen as the copy length; the
/// result is empty when dataLen is inconsistent or oversized.
std::vector<std::uint8_t> encodeDataPayload(const ChunkHeader &header, const std::uint8_t *data);

/// Splits a Data frame payload; @p data points into @p payload.
bool decodeDataPayload(const std::vector<std::uint8_t> &payload, ChunkHeader *header,
                       const std::uint8_t **data, std::size_t *dataLen);

/// Re-hashes @p data and compares it with header.digest.
bool chunkDigestMatches(const ChunkHeader &header, const std::uint8_t *data);

// -------------------------------------------------------------- chunk plan

/**
 * @brief Splits a file of @p fileSize bytes into chunkSize-byte chunks.
 * An empty file still has exactly one (empty) chunk, so both sides agree on
 * frame counts without a special case.
 */
class ChunkPlan {
public:
  ChunkPlan() = default;
  ChunkPlan(std::uint64_t fileSize, std::size_t chunkSize);

  bool valid() const
  {
    return m_valid;
  }
  std::uint64_t fileSize() const
  {
    return m_fileSize;
  }
  std::size_t chunkSize() const
  {
    return m_chunkSize;
  }
  /// Number of chunks, always >= 1.
  std::uint64_t count() const
  {
    return m_count;
  }
  std::uint64_t offset(std::uint64_t index) const;
  std::size_t length(std::uint64_t index) const;
  /// Sum of all chunk lengths — must equal fileSize().
  std::uint64_t bytesCovered() const;

  /// Maps a byte offset to its chunk index. False when not chunk-aligned.
  bool seqForOffset(std::uint64_t offset, std::uint32_t *seq) const;
  static bool isChunkAligned(std::uint64_t offset, std::size_t chunkSize);

private:
  std::uint64_t m_fileSize = 0;
  std::size_t m_chunkSize = kClipDefaultChunkSize;
  std::uint64_t m_count = 1;
  bool m_valid = true;
};

// ---------------------------------------------------------------- progress

struct TransferProgress {
  std::uint64_t bytesDone = 0;
  std::uint64_t bytesTotal = 0;

  std::uint64_t bytesRemaining() const
  {
    return bytesTotal > bytesDone ? bytesTotal - bytesDone : 0;
  }
  /// 1.0 for an empty transfer (nothing left to do), otherwise clamped 0..1.
  double ratio() const;
  int percent() const;

  bool operator==(const TransferProgress &other) const = default;
};

// ------------------------------------------------------------- path safety

/// Backslashes to slashes, collapses "//", drops leading "/" and "./", drops
/// trailing "/". Never rewrites the UTF-8 bytes themselves. ".." components are
/// preserved here on purpose — isSafeRelativePath() is what rejects them.
std::string normalizeRelativePath(std::string_view utf8Path);

/**
 * @brief Rejects anything that could escape the incoming directory: empty
 * paths, absolute paths ("/x", "\\x", "C:/x"), drive-relative ("C:x"), any
 * "." or ".." component, empty components, NUL bytes, over-long paths
 * (>4096 bytes) and over-long components (>255 bytes).
 * Callers should normalize first.
 */
bool isSafeRelativePath(std::string_view utf8RelPath);

/// Replaces the characters Windows forbids in file names and trims trailing
/// dots/spaces. Returns an empty string when nothing usable is left.
std::string sanitizeFileName(std::string_view utf8Name);

/// Appends kClipPartSuffix to the last path component.
/// Example: "a/b/c.bin" -> "a/b/c.bin.litekvm-part".
std::string partPathFor(std::string_view utf8RelPath);

/// UTF-8 "sub/dir/file.ext" -> its parent directory part ("sub/dir"), or "".
std::string parentDirOf(std::string_view utf8RelPath);

} // namespace litekvm
