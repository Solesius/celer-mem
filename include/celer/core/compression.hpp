#pragma once
/// celer::compression — Per-chunk compression codecs (snappy/lz4).
///
/// Transparent stream layer: compress(source, codec) → StreamHandle<char>.
/// Each compressed chunk is self-framed: [4B orig_size LE][compressed_data].
/// Decompression reads the frame header to recover original size.
///
/// Codec availability is compile-time:
///   snappy: __has_include(<snappy.h>)  — fast, ~250MB/s compress
///   lz4:    __has_include(<lz4.h>)     — faster, ~500MB/s compress
///
/// Design:
///   Compression is a stream combinator (transparent to backends).
///   Backend stores compressed bytes; decompression happens on read.
///   Codec selection is per-stream, not per-backend — mixed storage is valid.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "celer/core/result.hpp"
#include "celer/core/stream.hpp"

// ── Compile-time codec detection ──

#if !defined(CELER_FORCE_NO_SNAPPY) && __has_include(<snappy.h>)
#  define CELER_HAS_SNAPPY 1
#  include <snappy.h>
#else
#  define CELER_HAS_SNAPPY 0
#endif

#if !defined(CELER_FORCE_NO_LZ4) && __has_include(<lz4.h>)
#  define CELER_HAS_LZ4 1
#  include <lz4.h>
#else
#  define CELER_HAS_LZ4 0
#endif

namespace celer::compression {

// ── Codec enum ──

enum class Codec : std::uint8_t {
    none   = 0,
    snappy = 1,
    lz4    = 2,
};

/// Query compile-time codec availability.
[[nodiscard]] constexpr auto is_available(Codec c) noexcept -> bool {
    switch (c) {
    case Codec::none:   return true;
    case Codec::snappy: return CELER_HAS_SNAPPY != 0;
    case Codec::lz4:    return CELER_HAS_LZ4 != 0;
    }
    return false;
}

// ── Frame format: [4B original_size_le32][compressed_bytes] ──
//
// Even Codec::none uses the frame header for uniformity.
// snappy embeds its own size, but we frame it anyway for consistent parsing.

namespace detail {

inline void write_le32(char* dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<char>(v & 0xFF);
    dst[1] = static_cast<char>((v >> 8) & 0xFF);
    dst[2] = static_cast<char>((v >> 16) & 0xFF);
    dst[3] = static_cast<char>((v >> 24) & 0xFF);
}

[[nodiscard]] inline auto read_le32(const char* src) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(src[0]))
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(src[1])) << 8)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(src[2])) << 16)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(src[3])) << 24);
}

} // namespace detail

/// Compress raw bytes into a framed buffer: [4B orig_size LE][compressed_data].
/// The frame header enables self-describing decompression (LZ4 needs original size).
[[nodiscard]] inline auto compress_block(const char* data, std::size_t size, Codec codec)
    -> Result<std::vector<char>>
{
    if (size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::unexpected(Error{"Compress", "input exceeds 4 GiB frame limit"});
    }
    if (codec == Codec::none) {
        std::vector<char> out(4 + size);
        detail::write_le32(out.data(), static_cast<std::uint32_t>(size));
        std::memcpy(out.data() + 4, data, size);
        return out;
    }

#if CELER_HAS_SNAPPY
    if (codec == Codec::snappy) {
        std::string compressed;
        snappy::Compress(data, size, &compressed);
        std::vector<char> out(4 + compressed.size());
        detail::write_le32(out.data(), static_cast<std::uint32_t>(size));
        std::memcpy(out.data() + 4, compressed.data(), compressed.size());
        return out;
    }
#endif

#if CELER_HAS_LZ4
    if (codec == Codec::lz4) {
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return std::unexpected(Error{"Compress", "input exceeds LZ4 int limit"});
        }
        auto max_dst = LZ4_compressBound(static_cast<int>(size));
        std::vector<char> out(4 + static_cast<std::size_t>(max_dst));
        detail::write_le32(out.data(), static_cast<std::uint32_t>(size));
        auto compressed_size = LZ4_compress_default(
            data, out.data() + 4, static_cast<int>(size), max_dst);
        if (compressed_size <= 0) {
            return std::unexpected(Error{"Compress", "LZ4 compression failed"});
        }
        out.resize(4 + static_cast<std::size_t>(compressed_size));
        return out;
    }
#endif

    return std::unexpected(Error{"Compress",
        "requested codec not available (compiled without support)"});
}

/// Decompress a framed buffer: reads [4B orig_size LE][compressed_data].
[[nodiscard]] inline auto decompress_block(const char* data, std::size_t size, Codec codec)
    -> Result<std::vector<char>>
{
    if (size < 4) {
        return std::unexpected(Error{"Decompress", "frame too short (< 4 bytes)"});
    }
    auto original_size = detail::read_le32(data);
    static constexpr std::uint32_t max_decompress_size = 256u * 1024u * 1024u; // 256 MiB
    if (original_size > max_decompress_size) {
        return std::unexpected(Error{"Decompress",
            "claimed original size exceeds 256 MiB safety limit"});
    }
    const char* body = data + 4;
    auto body_size = size - 4;

    if (codec == Codec::none) {
        return std::vector<char>(body, body + body_size);
    }

#if CELER_HAS_SNAPPY
    if (codec == Codec::snappy) {
        std::string output;
        if (!snappy::Uncompress(body, body_size, &output)) {
            return std::unexpected(Error{"Decompress", "snappy decompression failed"});
        }
        return std::vector<char>(output.begin(), output.end());
    }
#endif

#if CELER_HAS_LZ4
    if (codec == Codec::lz4) {
        std::vector<char> output(original_size);
        auto decompressed = LZ4_decompress_safe(
            body, output.data(),
            static_cast<int>(body_size),
            static_cast<int>(original_size));
        if (decompressed < 0) {
            return std::unexpected(Error{"Decompress", "LZ4 decompression failed"});
        }
        output.resize(static_cast<std::size_t>(decompressed));
        return output;
    }
#endif

    return std::unexpected(Error{"Decompress",
        "requested codec not available (compiled without support)"});
}

} // namespace celer::compression

// ────────────────────────────────────────────────────────────────────
// Stream combinators: compress / decompress
// ────────────────────────────────────────────────────────────────────

namespace celer::stream {

namespace detail_compress {

/// CompressImpl: per-chunk compression as a stream combinator.
/// Input: StreamHandle<char> (raw bytes).
/// Output: StreamHandle<char> (framed compressed bytes).
struct CompressImpl {
    StreamHandle<char> source;
    compression::Codec codec;

    auto pull() -> Result<std::optional<Chunk<char>>> {
        auto r = source.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) return std::optional<Chunk<char>>{};
        auto& chunk = r->value();
        auto compressed = compression::compress_block(
            chunk.data(), chunk.size(), codec);
        if (!compressed) return std::unexpected(compressed.error());
        return std::optional{Chunk<char>::from(std::move(*compressed))};
    }

    CompressImpl(const CompressImpl& o)
        : source(o.source.clone()), codec(o.codec) {}
    CompressImpl(StreamHandle<char> s, compression::Codec c)
        : source(std::move(s)), codec(c) {}
};

/// DecompressImpl: per-chunk decompression as a stream combinator.
/// Input: StreamHandle<char> (framed compressed bytes).
/// Output: StreamHandle<char> (raw bytes).
struct DecompressImpl {
    StreamHandle<char> source;
    compression::Codec codec;

    auto pull() -> Result<std::optional<Chunk<char>>> {
        auto r = source.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) return std::optional<Chunk<char>>{};
        auto& chunk = r->value();
        auto decompressed = compression::decompress_block(
            chunk.data(), chunk.size(), codec);
        if (!decompressed) return std::unexpected(decompressed.error());
        return std::optional{Chunk<char>::from(std::move(*decompressed))};
    }

    DecompressImpl(const DecompressImpl& o)
        : source(o.source.clone()), codec(o.codec) {}
    DecompressImpl(StreamHandle<char> s, compression::Codec c)
        : source(std::move(s)), codec(c) {}
};

} // namespace detail_compress

/// Compress each chunk in a byte stream with the given codec.
/// Output chunks are framed: [4B original_size LE][compressed_data].
/// Roundtrips with decompress(): decompress(compress(s, c), c) ≡ s.
[[nodiscard]] inline auto compress(StreamHandle<char> source, compression::Codec codec)
    -> StreamHandle<char>
{
    return make_stream_handle<char>(
        new detail_compress::CompressImpl{std::move(source), codec});
}

/// Decompress each chunk in a byte stream with the given codec.
/// Expects framed input: [4B original_size LE][compressed_data].
/// Use together with compress(): decompress(compress(s, c), c) ≡ s.
[[nodiscard]] inline auto decompress(StreamHandle<char> source, compression::Codec codec)
    -> StreamHandle<char>
{
    return make_stream_handle<char>(
        new detail_compress::DecompressImpl{std::move(source), codec});
}

} // namespace celer::stream
