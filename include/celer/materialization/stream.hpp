#pragma once
/// celer::materialization::RecordStream<T> — typed pull-stream builder.
///
/// Thin wrapper over ``StreamHandle<Record<T>>`` that provides a fluent
/// chain of pure transforms (where/map/flat_map/take/inspect/batch).
/// Terminal ops: ``collect()`` (materialize to vector) and ``drain()``
/// (visit each record). Does **no** work until terminal — every link in
/// the chain is constructed as a lazy pull source.
///
/// All combinators take ownership of *this (move-only). To fan-out, use
/// ``.clone_handle()`` which delegates to the prototype clone of the
/// underlying ``StreamHandle``.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "celer/core/result.hpp"
#include "celer/core/stream.hpp"
#include "celer/materialization/store_ref.hpp"

namespace celer::materialization {

namespace detail {

// where / filter — keeps records satisfying pred
template <typename T, typename Pred>
struct WhereImpl {
    StreamHandle<Record<T>> source;
    Pred                    pred;

    auto pull() -> Result<std::optional<Chunk<Record<T>>>> {
        while (true) {
            auto r = source.pull();
            if (!r) return std::unexpected(r.error());
            if (!r->has_value()) return std::optional<Chunk<Record<T>>>{};
            const auto& chunk = r->value();
            auto built = Chunk<Record<T>>::build(
                static_cast<std::uint32_t>(chunk.size()),
                [&](Record<T>* out, std::uint32_t /*cap*/) -> std::uint32_t {
                    std::uint32_t n = 0;
                    for (const auto& rec : chunk) {
                        if (pred(rec)) {
                            ::new (out + n) Record<T>(rec);
                            ++n;
                        }
                    }
                    return n;
                });
            if (!built.empty()) return std::optional{std::move(built)};
        }
    }

    WhereImpl(const WhereImpl& o) : source(o.source.clone()), pred(o.pred) {}
    WhereImpl(StreamHandle<Record<T>> s, Pred p)
        : source(std::move(s)), pred(std::move(p)) {}
};

// map — Record<T> → Record<U>, key preserved by default
template <typename T, typename U, typename Fn>
struct MapImpl {
    StreamHandle<Record<T>> source;
    Fn                      fn;

    auto pull() -> Result<std::optional<Chunk<Record<U>>>> {
        auto r = source.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) return std::optional<Chunk<Record<U>>>{};
        const auto& chunk = r->value();
        auto built = Chunk<Record<U>>::build(
            static_cast<std::uint32_t>(chunk.size()),
            [&](Record<U>* out, std::uint32_t n) -> std::uint32_t {
                for (std::uint32_t i = 0; i < n; ++i) {
                    ::new (out + i) Record<U>{chunk[i].key, fn(chunk[i].value)};
                }
                return n;
            });
        return std::optional{std::move(built)};
    }

    MapImpl(const MapImpl& o) : source(o.source.clone()), fn(o.fn) {}
    MapImpl(StreamHandle<Record<T>> s, Fn f)
        : source(std::move(s)), fn(std::move(f)) {}
};

// take — short-circuit at N records
template <typename T>
struct TakeImpl {
    StreamHandle<Record<T>> source;
    std::size_t             remaining;

    auto pull() -> Result<std::optional<Chunk<Record<T>>>> {
        if (remaining == 0) return std::optional<Chunk<Record<T>>>{};
        auto r = source.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) return std::optional<Chunk<Record<T>>>{};
        auto& chunk = r->value();
        if (chunk.size() <= remaining) {
            remaining -= chunk.size();
            return r;
        }
        auto sliced = chunk.slice(0, remaining);
        remaining = 0;
        return std::optional{std::move(sliced)};
    }

    TakeImpl(const TakeImpl& o) : source(o.source.clone()), remaining(o.remaining) {}
    TakeImpl(StreamHandle<Record<T>> s, std::size_t n)
        : source(std::move(s)), remaining(n) {}
};

// inspect — fire-and-forget side effect; passes chunks through unchanged
template <typename T, typename Fn>
struct InspectImpl {
    StreamHandle<Record<T>> source;
    Fn                      fn;

    auto pull() -> Result<std::optional<Chunk<Record<T>>>> {
        auto r = source.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) return std::optional<Chunk<Record<T>>>{};
        for (const auto& rec : r->value()) fn(rec);
        return r;
    }

    InspectImpl(const InspectImpl& o) : source(o.source.clone()), fn(o.fn) {}
    InspectImpl(StreamHandle<Record<T>> s, Fn f)
        : source(std::move(s)), fn(std::move(f)) {}
};

// batch — repack records into chunks of exactly N (last chunk may be partial)
template <typename T>
struct BatchImpl {
    StreamHandle<Record<T>> source;
    std::size_t             batch_size;
    std::vector<Record<T>>  buffer;
    bool                    upstream_done{false};

    auto pull() -> Result<std::optional<Chunk<Record<T>>>> {
        while (buffer.size() < batch_size && !upstream_done) {
            auto r = source.pull();
            if (!r) return std::unexpected(r.error());
            if (!r->has_value()) { upstream_done = true; break; }
            for (const auto& rec : r->value()) buffer.push_back(rec);
        }
        if (buffer.empty()) return std::optional<Chunk<Record<T>>>{};
        const auto take_n = std::min(batch_size, buffer.size());
        std::vector<Record<T>> out(
            std::make_move_iterator(buffer.begin()),
            std::make_move_iterator(buffer.begin() + take_n));
        buffer.erase(buffer.begin(), buffer.begin() + take_n);
        return std::optional{Chunk<Record<T>>::from(std::move(out))};
    }

    BatchImpl(const BatchImpl& o)
        : source(o.source.clone()), batch_size(o.batch_size)
        , buffer(o.buffer), upstream_done(o.upstream_done) {}
    BatchImpl(StreamHandle<Record<T>> s, std::size_t n)
        : source(std::move(s)), batch_size(n) {}
};

} // namespace detail

// ── RecordStream<T> ──

template <typename T>
class RecordStream {
public:
    explicit RecordStream(StreamHandle<Record<T>> handle) : handle_(std::move(handle)) {}

    RecordStream(RecordStream&&) noexcept            = default;
    auto operator=(RecordStream&&) noexcept -> RecordStream& = default;
    RecordStream(const RecordStream&)                = delete;
    auto operator=(const RecordStream&) -> RecordStream& = delete;

    [[nodiscard]] auto take_handle() && noexcept -> StreamHandle<Record<T>> {
        return std::move(handle_);
    }

    [[nodiscard]] auto handle() const noexcept -> const StreamHandle<Record<T>>& {
        return handle_;
    }

    [[nodiscard]] auto clone_handle() const -> StreamHandle<Record<T>> {
        return handle_.clone();
    }

    // ── lazy combinators ──

    template <typename Pred>
    [[nodiscard]] auto where(Pred pred) && -> RecordStream<T> {
        auto* impl = new detail::WhereImpl<T, Pred>{std::move(handle_), std::move(pred)};
        return RecordStream<T>{make_stream_handle<Record<T>>(impl)};
    }

    template <typename Fn>
    [[nodiscard]] auto map(Fn fn) &&
        -> RecordStream<std::invoke_result_t<Fn, const T&>>
    {
        using U = std::invoke_result_t<Fn, const T&>;
        auto* impl = new detail::MapImpl<T, U, Fn>{std::move(handle_), std::move(fn)};
        return RecordStream<U>{make_stream_handle<Record<U>>(impl)};
    }

    [[nodiscard]] auto take(std::size_t n) && -> RecordStream<T> {
        auto* impl = new detail::TakeImpl<T>{std::move(handle_), n};
        return RecordStream<T>{make_stream_handle<Record<T>>(impl)};
    }

    template <typename Fn>
    [[nodiscard]] auto inspect(Fn fn) && -> RecordStream<T> {
        auto* impl = new detail::InspectImpl<T, Fn>{std::move(handle_), std::move(fn)};
        return RecordStream<T>{make_stream_handle<Record<T>>(impl)};
    }

    [[nodiscard]] auto batch(std::size_t n) && -> RecordStream<T> {
        auto* impl = new detail::BatchImpl<T>{std::move(handle_), n};
        return RecordStream<T>{make_stream_handle<Record<T>>(impl)};
    }

    // ── terminal ops ──

    [[nodiscard]] auto collect() && -> Result<std::vector<Record<T>>> {
        std::vector<Record<T>> out;
        while (true) {
            auto r = handle_.pull();
            if (!r) return std::unexpected(r.error());
            if (!r->has_value()) break;
            for (const auto& rec : r->value()) out.push_back(rec);
        }
        return out;
    }

    template <typename Fn>
    [[nodiscard]] auto drain(Fn&& fn) && -> VoidResult {
        while (true) {
            auto r = handle_.pull();
            if (!r) return std::unexpected(r.error());
            if (!r->has_value()) break;
            for (const auto& rec : r->value()) fn(rec);
        }
        return {};
    }

    [[nodiscard]] auto count() && -> Result<std::size_t> {
        std::size_t n = 0;
        while (true) {
            auto r = handle_.pull();
            if (!r) return std::unexpected(r.error());
            if (!r->has_value()) break;
            n += r->value().size();
        }
        return n;
    }

private:
    StreamHandle<Record<T>> handle_;
};

// ── Factories ──

template <typename T>
[[nodiscard]] auto stream_from(StoreRef<T> store, ScanOptions opts = {})
    -> Result<RecordStream<T>>
{
    auto h = store.stream_scan(std::move(opts));
    if (!h) return std::unexpected(h.error());
    return RecordStream<T>{std::move(*h)};
}

template <typename T>
[[nodiscard]] auto stream_of(std::vector<Record<T>> records) -> RecordStream<T> {
    return RecordStream<T>{stream::from_vector(std::move(records))};
}

} // namespace celer::materialization
