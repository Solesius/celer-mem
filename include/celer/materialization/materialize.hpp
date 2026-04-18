#pragma once
/// celer::materialization::materialize — write a RecordStream<T> into a target store.
///
/// Modes:
///   • Append  — write each record (last-writer wins on key collision)
///   • Upsert  — alias for Append on a KV target (no diff applied)
///   • Replace — clear target prefix first, then write everything
///   • Delta   — read watermark, only write records whose key > watermark,
///               then advance watermark to max(emitted_key)
///   • DryRun  — count rows, no writes
///
/// Atomicity:
///   Writes are buffered and flushed via underlying ``BackendHandle::batch()``
///   in chunks of ``MaterializeOptions::flush_batch_size`` (default 1024).
///   Each flush is atomic with respect to the backend; the overall job is
///   eventually consistent but progress is durable.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "celer/core/result.hpp"
#include "celer/core/types.hpp"
#include "celer/materialization/codec.hpp"
#include "celer/materialization/store_ref.hpp"
#include "celer/materialization/stream.hpp"

namespace celer::materialization {

enum class MaterializeMode : std::uint8_t {
    Append = 0,
    Upsert,
    Replace,
    Delta,
    DryRun,
};

struct MaterializeOptions {
    MaterializeMode  mode{MaterializeMode::Upsert};
    std::size_t      flush_batch_size{1024};
    std::string      replace_prefix;          ///< prefix cleared in Replace mode
    std::string      watermark_table;         ///< target table for watermark records (Delta)
    std::string      watermark_id;            ///< logical id of this materialization view
};

struct ExecutionMetrics {
    std::size_t  rows_in{0};
    std::size_t  rows_out{0};
    std::size_t  flushes{0};
    std::size_t  bytes_written{0};
    std::chrono::microseconds  elapsed{0};
};

struct MaterializeResult {
    std::size_t        rows_written{0};
    std::string        last_key;       ///< highest key flushed (Delta uses this)
    ExecutionMetrics   metrics;
};

// ── WatermarkRecord (RFC-005) ──
//
// Stored under key ``_celer_watermark/<id>`` in ``MaterializeOptions::watermark_table``.
// Hand-encoded as ``<u64 timestamp_us><u32 view_id_len><view_id><u32 last_key_len><last_key>``
// to avoid pulling reflect-cpp into the materialization layer.
struct WatermarkRecord {
    std::string  view_id;
    std::string  last_key;
    std::int64_t timestamp_us{0};
};

namespace detail {
inline auto watermark_key(std::string_view id) -> std::string {
    std::string k = "_celer_watermark/";
    k.append(id);
    return k;
}

inline auto encode_watermark(const WatermarkRecord& w) -> std::string {
    std::string out;
    out.reserve(8 + 4 + w.view_id.size() + 4 + w.last_key.size());
    auto append_raw = [&](const void* p, std::size_t n) {
        out.append(static_cast<const char*>(p), n);
    };
    append_raw(&w.timestamp_us, sizeof(w.timestamp_us));
    std::uint32_t l1 = static_cast<std::uint32_t>(w.view_id.size());
    append_raw(&l1, sizeof(l1));
    out.append(w.view_id);
    std::uint32_t l2 = static_cast<std::uint32_t>(w.last_key.size());
    append_raw(&l2, sizeof(l2));
    out.append(w.last_key);
    return out;
}

inline auto decode_watermark(std::string_view bytes) -> Result<WatermarkRecord> {
    WatermarkRecord w;
    if (bytes.size() < 8 + 4 + 4) {
        return std::unexpected(Error{"WatermarkDecode", "truncated"});
    }
    std::size_t off = 0;
    std::memcpy(&w.timestamp_us, bytes.data() + off, sizeof(w.timestamp_us));
    off += sizeof(w.timestamp_us);
    std::uint32_t l1 = 0;
    std::memcpy(&l1, bytes.data() + off, sizeof(l1));
    off += sizeof(l1);
    if (off + l1 > bytes.size()) {
        return std::unexpected(Error{"WatermarkDecode", "view_id overflow"});
    }
    w.view_id.assign(bytes.data() + off, l1);
    off += l1;
    std::uint32_t l2 = 0;
    if (off + sizeof(l2) > bytes.size()) {
        return std::unexpected(Error{"WatermarkDecode", "missing last_key length"});
    }
    std::memcpy(&l2, bytes.data() + off, sizeof(l2));
    off += sizeof(l2);
    if (off + l2 > bytes.size()) {
        return std::unexpected(Error{"WatermarkDecode", "last_key overflow"});
    }
    w.last_key.assign(bytes.data() + off, l2);
    return w;
}
} // namespace detail

// ── Public API ──
//
// KeyFn: const Record<T>& -> Result<std::string>   (encoded target key)
// ValFn: const T&         -> const T&              (passthrough by default)
//
// For type conversion T -> U, use stream.map(...) before calling materialize.
template <typename T, typename KeyFn>
[[nodiscard]] auto materialize(RecordStream<T> source,
                               StoreRef<T> target,
                               KeyFn keyfn,
                               MaterializeOptions opts = {})
    -> Result<MaterializeResult>
{
    using clock = std::chrono::steady_clock;
    auto start = clock::now();

    MaterializeResult result;
    if (target.handle() == nullptr) {
        return std::unexpected(Error{"Materialize", "target StoreRef is null"});
    }

    // ── Mode: Replace prelude — wipe target prefix ──
    if (opts.mode == MaterializeMode::Replace) {
        auto existing = target.handle()->prefix_scan(opts.replace_prefix);
        if (!existing) return std::unexpected(existing.error());
        if (!existing->empty()) {
            std::vector<BatchOp> dels;
            dels.reserve(existing->size());
            for (auto& kv : *existing) {
                dels.push_back(BatchOp{
                    .kind = BatchOp::Kind::del,
                    .cf_name = target.name(),
                    .key = kv.key,
                    .value = std::nullopt,
                });
            }
            auto del_r = target.handle()->batch(dels);
            if (!del_r) return std::unexpected(del_r.error());
        }
    }

    // ── Mode: Delta — read watermark cursor ──
    std::string delta_floor;
    if (opts.mode == MaterializeMode::Delta) {
        if (opts.watermark_id.empty()) {
            return std::unexpected(Error{"MaterializeDelta",
                "watermark_id required for Delta mode"});
        }
        auto wmk_key = detail::watermark_key(opts.watermark_id);
        auto raw = target.handle()->get(wmk_key);
        if (!raw) return std::unexpected(raw.error());
        if (raw->has_value()) {
            auto rec = detail::decode_watermark(raw->value());
            if (rec) delta_floor = rec->last_key;
        }
    }

    // ── Streaming write loop ──
    std::vector<BatchOp> pending;
    std::vector<std::string> value_storage;     // keep encoded bytes alive for batch
    std::vector<std::string> key_storage;
    pending.reserve(opts.flush_batch_size);
    value_storage.reserve(opts.flush_batch_size);
    key_storage.reserve(opts.flush_batch_size);

    auto flush = [&]() -> VoidResult {
        if (pending.empty()) return {};
        if (opts.mode != MaterializeMode::DryRun) {
            auto r = target.handle()->batch(pending);
            if (!r) return std::unexpected(r.error());
        }
        result.metrics.flushes += 1;
        pending.clear();
        value_storage.clear();
        key_storage.clear();
        return {};
    };

    auto& src_handle = const_cast<StreamHandle<Record<T>>&>(source.handle());
    while (true) {
        auto r = src_handle.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) break;
        for (const auto& rec : r->value()) {
            result.metrics.rows_in += 1;
            auto kr = keyfn(rec);
            if (!kr) return std::unexpected(kr.error());

            if (opts.mode == MaterializeMode::Delta && !delta_floor.empty()) {
                if (*kr <= delta_floor) continue;     // skip already-emitted
            }

            auto enc = codec<T>::encode(rec.value);
            if (!enc) return std::unexpected(enc.error());

            key_storage.push_back(std::move(*kr));
            value_storage.push_back(std::move(*enc));
            pending.push_back(BatchOp{
                .kind = BatchOp::Kind::put,
                .cf_name = target.name(),
                .key = key_storage.back(),
                .value = value_storage.back(),
            });
            result.metrics.rows_out += 1;
            result.metrics.bytes_written += value_storage.back().size();
            if (key_storage.back() > result.last_key) {
                result.last_key = key_storage.back();
            }

            if (pending.size() >= opts.flush_batch_size) {
                auto fr = flush();
                if (!fr) return std::unexpected(fr.error());
            }
        }
    }
    auto fr = flush();
    if (!fr) return std::unexpected(fr.error());
    result.rows_written = result.metrics.rows_out;

    // ── Mode: Delta — advance watermark on success ──
    if (opts.mode == MaterializeMode::Delta && !result.last_key.empty()) {
        WatermarkRecord wmk{
            .view_id   = opts.watermark_id,
            .last_key  = result.last_key,
            .timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                clock::now().time_since_epoch()).count(),
        };
        auto enc = detail::encode_watermark(wmk);
        auto pr = target.handle()->put(detail::watermark_key(opts.watermark_id), enc);
        if (!pr) return std::unexpected(pr.error());
    }

    result.metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now() - start);
    return result;
}

} // namespace celer::materialization
