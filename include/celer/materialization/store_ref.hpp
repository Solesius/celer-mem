#pragma once
/// celer::materialization::StoreRef<T> — typed lens over a TableRef.
///
/// Wraps a ``BackendHandle*`` (acquired through ``TableRef``) and threads
/// codec encode/decode at the boundary. Exposes:
///   - ``get(key)``           — single-key typed read
///   - ``get_many(keys)``     — batch read; routes to native vtable path
///   - ``put`` / ``del``      — single-key typed write
///   - ``stream_scan(...)``   — pull-based range stream of (Key, T)
///   - ``stream_all()``       — full-table stream (prefix = "")
///   - ``put_many(items)``    — atomic batch put via underlying ``batch``
///
/// Capability flags are queried up-front to drive the planner.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "celer/api/table_ref.hpp"
#include "celer/backend/concept.hpp"
#include "celer/core/result.hpp"
#include "celer/core/stream.hpp"
#include "celer/core/types.hpp"
#include "celer/materialization/codec.hpp"

namespace celer::materialization {

// ── Capability descriptor (planner input) ──

enum class CostTier : std::uint8_t {
    Local      = 0,    ///< RocksDB / SQLite — sub-ms point lookups
    Embedded   = 1,    ///< QPDF / on-disk file formats
    Network    = 2,    ///< S3 / remote KV — round-trip dominates
    Computed   = 3,    ///< pure synthetic / lambda streams
};

struct StoreCapabilities {
    bool      native_batch_get{false};
    bool      native_streaming{true};      ///< all backends expose stream_*
    bool      ordered_scan{true};          ///< prefix_scan returns sorted KVs
    bool      supports_atomic_batch{true};
    CostTier  cost_tier{CostTier::Local};
};

struct CostHint {
    double    point_get_us{2.0};      ///< median µs per get
    double    batch_get_us{0.4};      ///< median µs per key in get_many
    double    scan_throughput_mb{200};///< MB/s
};

struct ScanOptions {
    std::string  prefix;              ///< empty = full table
    std::optional<std::string> lower; ///< inclusive lower bound (optional)
    std::optional<std::string> upper; ///< exclusive upper bound (optional)
    std::size_t  page_size{1024};     ///< chunk emit size for stream_scan
    std::size_t  limit{0};            ///< 0 = unlimited
};

// ── Decoded record yielded by streams ──

template <typename T>
struct Record {
    std::string  key;
    T            value;
};

// ── Internal: stream impl that decodes KVPair → Record<T> on the fly ──
namespace detail {

template <typename T>
struct DecodeKVImpl {
    StreamHandle<KVPair> source;
    std::size_t          limit_remaining{0};   // 0 = no limit
    bool                 has_limit{false};

    DecodeKVImpl(StreamHandle<KVPair> s, std::size_t lim, bool has_lim)
        : source(std::move(s)), limit_remaining(lim), has_limit(has_lim) {}

    DecodeKVImpl(const DecodeKVImpl& o)
        : source(o.source.clone())
        , limit_remaining(o.limit_remaining)
        , has_limit(o.has_limit) {}

    auto pull() -> Result<std::optional<Chunk<Record<T>>>> {
        if (has_limit && limit_remaining == 0) {
            return std::optional<Chunk<Record<T>>>{};
        }
        auto r = source.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) return std::optional<Chunk<Record<T>>>{};
        const auto& kvs = r->value();

        std::uint32_t cap = static_cast<std::uint32_t>(kvs.size());
        if (has_limit && cap > limit_remaining) {
            cap = static_cast<std::uint32_t>(limit_remaining);
        }

        // We have to materialize to handle decode failures, but Chunk::build
        // keeps allocation count to one and gives a refcounted buffer.
        std::optional<Error> err;
        auto chunk = Chunk<Record<T>>::build(cap,
            [&](Record<T>* out, std::uint32_t n) -> std::uint32_t {
                std::uint32_t written = 0;
                for (std::uint32_t i = 0; i < n; ++i) {
                    auto dec = codec<T>::decode(kvs[i].value);
                    if (!dec) {
                        err = dec.error();
                        return written;
                    }
                    ::new (out + i) Record<T>{kvs[i].key, std::move(*dec)};
                    ++written;
                }
                return written;
            });
        if (err) return std::unexpected(*err);
        if (has_limit) limit_remaining -= chunk.size();
        if (chunk.empty()) return std::optional<Chunk<Record<T>>>{};
        return std::optional{std::move(chunk)};
    }
};

} // namespace detail

// ── StoreRef<T>: typed view ──

template <typename T>
class StoreRef {
public:
    StoreRef() = default;

    /// Construct from a TableRef. The underlying BackendHandle must outlive
    /// this StoreRef (lifetime is owned by the Store/ResourceStack).
    explicit StoreRef(const TableRef& table)
        : handle_(table.handle()), table_name_(table.name()) {}

    StoreRef(const BackendHandle* handle, std::string name)
        : handle_(handle), table_name_(std::move(name)) {}

    [[nodiscard]] auto valid() const noexcept -> bool { return handle_ != nullptr; }
    [[nodiscard]] auto name()  const noexcept -> const std::string& { return table_name_; }
    [[nodiscard]] auto handle() const noexcept -> const BackendHandle* { return handle_; }

    [[nodiscard]] auto capabilities() const -> StoreCapabilities {
        StoreCapabilities c;
        if (handle_) c.native_batch_get = handle_->has_native_batch_get();
        return c;
    }

    [[nodiscard]] auto cost_hint() const -> CostHint {
        // Defaults are fine for the v1 planner; future: per-backend overrides.
        return CostHint{};
    }

    // ── Single-key ops ──

    [[nodiscard]] auto get(std::string_view key) const -> Result<std::optional<T>> {
        auto raw = handle_->get(key);
        if (!raw) return std::unexpected(raw.error());
        if (!raw->has_value()) return std::optional<T>{};
        auto dec = codec<T>::decode(raw->value());
        if (!dec) return std::unexpected(dec.error());
        return std::optional<T>{std::move(*dec)};
    }

    [[nodiscard]] auto put(std::string_view key, const T& value) const -> VoidResult {
        auto enc = codec<T>::encode(value);
        if (!enc) return std::unexpected(enc.error());
        return handle_->put(key, *enc);
    }

    [[nodiscard]] auto del(std::string_view key) const -> VoidResult {
        return handle_->del(key);
    }

    // ── Batch ops ──

    /// Batch read: routes through native MultiGet/IN-clause when available,
    /// or transparent loop-over-get otherwise. Output preserves input order.
    /// Decoding failures abort the whole batch with the offending error.
    [[nodiscard]] auto get_many(std::span<const std::string_view> keys) const
        -> Result<std::vector<std::pair<std::string, std::optional<T>>>>
    {
        auto raw = handle_->get_many(keys);
        if (!raw) return std::unexpected(raw.error());
        std::vector<std::pair<std::string, std::optional<T>>> out;
        out.reserve(raw->size());
        for (auto& item : *raw) {
            if (!item.value.has_value()) {
                out.emplace_back(std::move(item.key), std::nullopt);
                continue;
            }
            auto dec = codec<T>::decode(*item.value);
            if (!dec) return std::unexpected(dec.error());
            out.emplace_back(std::move(item.key), std::optional<T>{std::move(*dec)});
        }
        return out;
    }

    /// Atomic write set. All-or-nothing within the underlying backend.
    [[nodiscard]] auto put_many(std::span<const std::pair<std::string, T>> items) const
        -> VoidResult
    {
        std::vector<std::string> encoded;
        encoded.reserve(items.size());
        std::vector<BatchOp> ops;
        ops.reserve(items.size());
        for (const auto& [k, v] : items) {
            auto enc = codec<T>::encode(v);
            if (!enc) return std::unexpected(enc.error());
            encoded.push_back(std::move(*enc));
            ops.push_back(BatchOp{
                .kind = BatchOp::Kind::put,
                .cf_name = table_name_,
                .key = k,
                .value = encoded.back(),
            });
        }
        return handle_->batch(ops);
    }

    // ── Streaming reads ──

    /// Pull-based stream of decoded ``Record<T>`` over a prefix range.
    /// Honors ``ScanOptions::limit`` (lower/upper bounds are forwarded as a
    /// best-effort prefix today; full range scans land in v2).
    [[nodiscard]] auto stream_scan(ScanOptions opts) const
        -> Result<StreamHandle<Record<T>>>
    {
        auto src = handle_->stream_scan(opts.prefix);
        if (!src) return std::unexpected(src.error());
        auto* impl = new detail::DecodeKVImpl<T>(
            std::move(*src), opts.limit, opts.limit > 0);
        return make_stream_handle<Record<T>>(impl);
    }

    [[nodiscard]] auto stream_all() const -> Result<StreamHandle<Record<T>>> {
        return stream_scan({});
    }

private:
    const BackendHandle*  handle_{nullptr};
    std::string           table_name_;
};

} // namespace celer::materialization
