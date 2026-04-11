#pragma once

#include <string>
#include <string_view>
#include <type_traits>

#include "celer/core/result.hpp"
#include "celer/serde/reflect.hpp"

namespace celer {

// ── Codec<T>: user-specializable encode/decode ──
// Default: use reflect-cpp msgpack. Specialize for custom wire formats.

template <typename T, typename = void>
struct Codec {
    [[nodiscard]] static auto encode(const T& value) -> Result<std::string> {
        return encode_msgpack(value);
    }

    [[nodiscard]] static auto decode(std::string_view bytes) -> Result<T> {
        return decode_msgpack<T>(bytes);
    }
};

// ── Convenience free functions ──

template <typename T>
[[nodiscard]] auto codec_encode(const T& value) -> Result<std::string> {
    return Codec<T>::encode(value);
}

template <typename T>
[[nodiscard]] auto codec_decode(std::string_view bytes) -> Result<T> {
    return Codec<T>::decode(bytes);
}

} // namespace celer
