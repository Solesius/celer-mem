#pragma once
/// celer::materialization::codec — Customization Point Object for keys/values.
///
/// Two-tier strategy:
///   1. ADL hook: free functions `mat_to_bytes(const T&)` / `mat_from_bytes(T&, sv)`
///      in the type's namespace. Found first if present.
///   2. Default: delegate to celer::Codec<T> (msgpack via reflect-cpp, with
///      string identity passthrough). Means any type already storable in
///      celer is automatically usable in joins/materialization.
///
/// Plus a fast 64-bit FNV-1a key hash for hash-join probe sets.

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "celer/core/result.hpp"
#include "celer/serde/codec.hpp"

namespace celer::materialization {

// ── ADL probes ──

namespace detail {

// Use a tag arg for ADL resolution without polluting global lookup.
struct adl_tag {};

template <typename T>
concept HasAdlToBytes = requires(const T& v, std::string& out) {
    { mat_to_bytes(v, out, adl_tag{}) } -> std::same_as<void>;
};

template <typename T>
concept HasAdlFromBytes = requires(T& v, std::string_view bytes) {
    { mat_from_bytes(v, bytes, adl_tag{}) } -> std::same_as<bool>;
};

} // namespace detail

// ── CPO: codec ──

template <typename T>
struct codec {
    [[nodiscard]] static auto encode(const T& value) -> Result<std::string> {
        if constexpr (detail::HasAdlToBytes<T>) {
            std::string out;
            mat_to_bytes(value, out, detail::adl_tag{});
            return out;
        } else {
            return ::celer::Codec<T>::encode(value);
        }
    }

    [[nodiscard]] static auto decode(std::string_view bytes) -> Result<T> {
        if constexpr (detail::HasAdlFromBytes<T>) {
            T v{};
            if (!mat_from_bytes(v, bytes, detail::adl_tag{})) {
                return std::unexpected(Error{"MatCodecDecode", "ADL decode rejected bytes"});
            }
            return v;
        } else {
            return ::celer::Codec<T>::decode(bytes);
        }
    }
};

template <typename T>
[[nodiscard]] auto encode(const T& value) -> Result<std::string> {
    return codec<T>::encode(value);
}

template <typename T>
[[nodiscard]] auto decode(std::string_view bytes) -> Result<T> {
    return codec<T>::decode(bytes);
}

// ── FNV-1a 64-bit hash ──
//
// Used for hash-join probe sets and dense dedupe of left-side keys.
// FNV-1a is fast, branchless, and has good avalanche on short keys (typical
// for primary-key lookups). ~1 cycle/byte on modern x86.

[[nodiscard]] constexpr auto fnv1a64(std::string_view bytes) noexcept -> std::uint64_t {
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime  = 0x100000001b3ULL;
    std::uint64_t h = kOffset;
    for (auto c : bytes) {
        h ^= static_cast<std::uint8_t>(c);
        h *= kPrime;
    }
    return h;
}

// ── Key: encoded-bytes + cached hash ──

struct Key {
    std::string    bytes;
    std::uint64_t  hash{0};

    Key() = default;
    explicit Key(std::string b) : bytes(std::move(b)), hash(fnv1a64(bytes)) {}

    [[nodiscard]] auto view() const noexcept -> std::string_view { return bytes; }

    friend auto operator==(const Key& a, const Key& b) noexcept -> bool {
        return a.hash == b.hash && a.bytes == b.bytes;
    }
};

struct KeyHash {
    [[nodiscard]] auto operator()(const Key& k) const noexcept -> std::size_t {
        return static_cast<std::size_t>(k.hash);
    }
};

// ── CompositeKey: ordered concatenation of part-bytes with length prefixes ──
//
// Used when joining on multiple columns. Layout:
//   [u32 part_len][part_bytes][u32 part_len][part_bytes]...

struct CompositeKey {
    std::string  bytes;
    std::uint64_t hash{0};

    CompositeKey() = default;

    template <typename... Parts>
    static auto make(const Parts&... parts) -> Result<CompositeKey> {
        CompositeKey out;
        VoidResult err = {};
        auto append_one = [&](const auto& p) {
            if (!err) return;
            auto enc = encode(p);
            if (!enc) { err = std::unexpected(enc.error()); return; }
            std::uint32_t n = static_cast<std::uint32_t>(enc->size());
            out.bytes.append(reinterpret_cast<const char*>(&n), sizeof(n));
            out.bytes.append(*enc);
        };
        (append_one(parts), ...);
        if (!err) return std::unexpected(err.error());
        out.hash = fnv1a64(out.bytes);
        return out;
    }

    [[nodiscard]] auto to_key() const -> Key {
        Key k;
        k.bytes = bytes;
        k.hash = hash;
        return k;
    }
};

} // namespace celer::materialization
