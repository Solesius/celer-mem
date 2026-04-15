#pragma once
/// FlatSymbolTable — Power-of-two open-addressing symbol table with memoized hashes.
///
/// Replaces std::unordered_map in CompositeNode for the immutable dispatch tree.
/// Design constraints (Okasaki-immutable, cache-hot):
///   1. Built once at construction, never mutated after.
///   2. Power-of-two capacity, index via bitwise AND (no modulo).
///   3. Cached hash per slot — avoids rehash on probe, enables branchless compare.
///   4. Bounded linear probe (max 8) — worst-case predictable, icache-friendly.
///   5. Small-string-optimized key (48 bytes inline, no heap alloc for typical names).
///   6. Transparent string_view lookup — zero-copy, no temporary std::string.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace celer {

class FlatSymbolTable {
public:
    static constexpr std::size_t max_probe = 8;
    static constexpr std::size_t sso_capacity = 48;

private:
    struct Slot {
        std::size_t  hash{0};
        std::size_t  value{0};
        std::uint8_t key_len{0};
        bool         occupied{false};
        char         key_buf[sso_capacity]{};    // inline key (SSO)
        std::string  key_heap;                   // fallback for long keys

        [[nodiscard]] auto key_view() const noexcept -> std::string_view {
            return key_len <= sso_capacity
                ? std::string_view{key_buf, key_len}
                : std::string_view{key_heap};
        }

        void set_key(std::string_view k) {
            key_len = static_cast<std::uint8_t>(k.size() <= 255 ? k.size() : 255);
            if (k.size() <= sso_capacity) {
                std::memcpy(key_buf, k.data(), k.size());
            } else {
                key_heap.assign(k.data(), k.size());
                key_len = 255;  // sentinel: use heap
            }
        }

        [[nodiscard]] auto matches(std::size_t h, std::string_view k) const noexcept -> bool {
            // Fast reject: cached hash mismatch — branchless-friendly
            if (hash != h) return false;
            return key_view() == k;
        }
    };

    std::vector<Slot> slots_;
    std::size_t mask_{0};
    std::size_t count_{0};

    static auto next_pow2(std::size_t n) noexcept -> std::size_t {
        // Minimum capacity 16, always >= 2x load
        n = n < 4 ? 4 : n;
        std::size_t cap = 16;
        while (cap < n * 2) cap <<= 1;
        return cap;
    }

    static auto hash_key(std::string_view k) noexcept -> std::size_t {
        return std::hash<std::string_view>{}(k);
    }

public:
    FlatSymbolTable() = default;

    /// Build from parallel key/value arrays. Keys must be unique.
    /// Capacity is rounded up to next power of two (>= 2x count).
    template <typename KeyRange>
    static auto build(const KeyRange& keys,
                      const std::vector<std::size_t>& values) -> FlatSymbolTable {
        assert(std::size(keys) == values.size());
        FlatSymbolTable tbl;
        auto n = std::size(keys);
        auto cap = next_pow2(n);
        tbl.slots_.resize(cap);
        tbl.mask_ = cap - 1;
        tbl.count_ = n;

        std::size_t idx = 0;
        for (const auto& k : keys) {
            std::string_view sv{k};
            auto h = hash_key(sv);
            auto pos = h & tbl.mask_;

            // Linear probe — bounded by capacity (always succeeds at <50% load)
            for (std::size_t probe = 0; probe < cap; ++probe) {
                auto& slot = tbl.slots_[(pos + probe) & tbl.mask_];
                if (!slot.occupied) {
                    slot.hash = h;
                    slot.value = values[idx];
                    slot.set_key(sv);
                    slot.occupied = true;
                    break;
                }
            }
            ++idx;
        }
        return tbl;
    }

    /// Build from name-extractor over a range of children.
    /// value = index into children array.
    template <typename Range, typename NameFn>
    static auto build_indexed(const Range& children, NameFn&& name_fn) -> FlatSymbolTable {
        std::vector<std::string_view> keys;
        std::vector<std::size_t> values;
        keys.reserve(std::size(children));
        values.reserve(std::size(children));
        std::size_t i = 0;
        for (const auto& child : children) {
            keys.push_back(name_fn(child));
            values.push_back(i++);
        }
        return build(keys, values);
    }

    /// Branchless-friendly lookup. Returns the stored value or nullopt.
    /// Probe bound: max(8, table_size). For typical scope/table counts (<64),
    /// this completes in 1-3 cache lines.
    [[nodiscard]] auto find(std::string_view key) const noexcept -> std::optional<std::size_t> {
        if (count_ == 0) [[unlikely]] return std::nullopt;

        auto h = hash_key(key);
        auto pos = h & mask_;
        auto bound = count_ < max_probe ? max_probe : count_;

        for (std::size_t probe = 0; probe < bound; ++probe) {
            const auto& slot = slots_[(pos + probe) & mask_];
            if (!slot.occupied) [[unlikely]] return std::nullopt;
            if (slot.matches(h, key)) [[likely]] return slot.value;
        }
        return std::nullopt;
    }

    /// Convenience: check if key exists.
    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool {
        return find(key).has_value();
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t { return count_; }
    [[nodiscard]] auto capacity() const noexcept -> std::size_t { return slots_.size(); }
    [[nodiscard]] auto empty() const noexcept -> bool { return count_ == 0; }
};

} // namespace celer
