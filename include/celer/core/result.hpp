#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace celer {

struct Error {
    std::string code;
    std::string message;
};

/// Result<T> = std::expected<T, Error>
template <typename T>
using Result = std::expected<T, Error>;

/// VoidResult = std::expected<void, Error>
using VoidResult = std::expected<void, Error>;

// ── Monadic combinators ──

template <typename T>
[[nodiscard]] constexpr auto is_ok(const Result<T>& r) noexcept -> bool {
    return r.has_value();
}

[[nodiscard]] inline constexpr auto is_ok(const VoidResult& r) noexcept -> bool {
    return r.has_value();
}

template <typename T, typename F>
[[nodiscard]] constexpr auto and_then(Result<T>&& r, F&& fn) -> decltype(fn(std::move(*r))) {
    if (r) return fn(std::move(*r));
    return std::unexpected(std::move(r.error()));
}

template <typename T, typename F>
[[nodiscard]] constexpr auto map(Result<T>&& r, F&& fn)
    -> Result<std::decay_t<decltype(fn(std::move(*r)))>> {
    if (r) return fn(std::move(*r));
    return std::unexpected(std::move(r.error()));
}

template <typename T, typename F>
[[nodiscard]] constexpr auto or_else(Result<T>&& r, F&& fn) -> Result<T> {
    if (r) return std::move(r);
    return fn(std::move(r.error()));
}

template <typename T>
[[nodiscard]] constexpr auto value_or(Result<T>&& r, T fallback) -> T {
    if (r) return std::move(*r);
    return std::move(fallback);
}

/// Convenience error constructor
[[nodiscard]] inline auto make_error(std::string code, std::string message) -> Error {
    return Error{std::move(code), std::move(message)};
}

} // namespace celer
