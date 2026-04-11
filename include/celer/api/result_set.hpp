#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "celer/core/dispatch.hpp"
#include "celer/core/result.hpp"
#include "celer/serde/codec.hpp"

namespace celer {

/// ResultSet<T> — Slick-style chainable query over a materialized vector.
/// filter(), sort_by(), take(), map(), collect() — all return a new ResultSet.
template <typename T>
class ResultSet {
public:
    explicit ResultSet(std::vector<T> data) : data_(std::move(data)) {}

    /// Filter elements by predicate.
    template <typename Pred>
    [[nodiscard]] auto filter(Pred&& pred) const -> ResultSet {
        std::vector<T> out;
        out.reserve(data_.size());
        std::copy_if(data_.begin(), data_.end(), std::back_inserter(out),
                     std::forward<Pred>(pred));
        return ResultSet{std::move(out)};
    }

    /// Sort by comparator.
    template <typename Cmp>
    [[nodiscard]] auto sort_by(Cmp&& cmp) const -> ResultSet {
        auto out = data_;
        std::sort(out.begin(), out.end(), std::forward<Cmp>(cmp));
        return ResultSet{std::move(out)};
    }

    /// Take first n elements.
    [[nodiscard]] auto take(std::size_t n) const -> ResultSet {
        auto count = std::min(n, data_.size());
        return ResultSet{std::vector<T>(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(count))};
    }

    /// Transform elements.
    template <typename Fn>
    [[nodiscard]] auto map(Fn&& fn) const -> ResultSet<std::decay_t<decltype(fn(std::declval<T>()))>> {
        using U = std::decay_t<decltype(fn(std::declval<T>()))>;
        std::vector<U> out;
        out.reserve(data_.size());
        for (const auto& elem : data_) {
            out.push_back(fn(elem));
        }
        return ResultSet<U>{std::move(out)};
    }

    /// Get first element.
    [[nodiscard]] auto first() const -> std::optional<T> {
        if (data_.empty()) return std::nullopt;
        return data_.front();
    }

    /// Materialize to vector.
    [[nodiscard]] auto collect() const -> const std::vector<T>& { return data_; }
    [[nodiscard]] auto collect() -> std::vector<T>&& { return std::move(data_); }

    /// Element count.
    [[nodiscard]] auto count() const noexcept -> std::size_t { return data_.size(); }

    /// Iterate without materializing a new vector.
    template <typename Fn>
    auto foreach(Fn&& fn) const -> void {
        for (const auto& elem : data_) {
            fn(elem);
        }
    }

private:
    std::vector<T> data_;
};

/// Deserialize raw KVPairs into a typed ResultSet.
template <typename T>
[[nodiscard]] auto from_raw_pairs(const std::vector<KVPair>& pairs) -> ResultSet<T> {
    std::vector<T> decoded;
    decoded.reserve(pairs.size());
    for (const auto& kv : pairs) {
        if (auto r = codec_decode<T>(kv.value); r) {
            decoded.push_back(std::move(*r));
        }
    }
    return ResultSet<T>{std::move(decoded)};
}

} // namespace celer
