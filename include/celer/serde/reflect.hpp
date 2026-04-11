#pragma once

#include <string>
#include <string_view>

#include "celer/core/result.hpp"

namespace celer {

// ── Serde via reflect-cpp ──
// Zero-macro aggregate struct reflection.
// MessagePack wire format, JSON debug mode.

#if __has_include(<rfl/msgpack.hpp>)

#include <rfl/msgpack.hpp>
#include <rfl/json.hpp>

template <typename T>
[[nodiscard]] auto encode_msgpack(const T& value) -> Result<std::string> {
    try {
        return rfl::msgpack::write(value);
    } catch (const std::exception& e) {
        return std::unexpected(Error{"SerdeEncode", e.what()});
    }
}

template <typename T>
[[nodiscard]] auto decode_msgpack(std::string_view bytes) -> Result<T> {
    try {
        return rfl::msgpack::read<T>(bytes);
    } catch (const std::exception& e) {
        return std::unexpected(Error{"SerdeDecode", e.what()});
    }
}

template <typename T>
[[nodiscard]] auto encode_json(const T& value) -> Result<std::string> {
    try {
        return rfl::json::write(value);
    } catch (const std::exception& e) {
        return std::unexpected(Error{"SerdeEncodeJson", e.what()});
    }
}

template <typename T>
[[nodiscard]] auto decode_json(std::string_view json) -> Result<T> {
    try {
        return rfl::json::read<T>(json);
    } catch (const std::exception& e) {
        return std::unexpected(Error{"SerdeDecodeJson", e.what()});
    }
}

#else

// ── Stub when reflect-cpp is not available ──

template <typename T>
[[nodiscard]] auto encode_msgpack(const T&) -> Result<std::string> {
    return std::unexpected(Error{"SerdeEncode", "reflect-cpp not available"});
}

template <typename T>
[[nodiscard]] auto decode_msgpack(std::string_view) -> Result<T> {
    return std::unexpected(Error{"SerdeDecode", "reflect-cpp not available"});
}

template <typename T>
[[nodiscard]] auto encode_json(const T&) -> Result<std::string> {
    return std::unexpected(Error{"SerdeEncodeJson", "reflect-cpp not available"});
}

template <typename T>
[[nodiscard]] auto decode_json(std::string_view) -> Result<T> {
    return std::unexpected(Error{"SerdeDecodeJson", "reflect-cpp not available"});
}

#endif

} // namespace celer
