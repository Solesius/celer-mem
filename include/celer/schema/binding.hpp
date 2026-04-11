#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace celer {

// ── fixed_string: constexpr NTTP string literal ──

template <std::size_t N>
struct fixed_string {
    char data[N]{};
    static constexpr std::size_t size = N - 1;

    constexpr fixed_string() = default;
    constexpr fixed_string(const char (&str)[N]) {
        std::copy_n(str, N, data);
    }

    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
        return {data, size};
    }

    template <std::size_t M>
    [[nodiscard]] constexpr auto operator==(const fixed_string<M>& o) const noexcept -> bool {
        return std::string_view{data, size} == std::string_view{o.data, o.size};
    }
};

template <std::size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

// ── SchemaBinding: compile-time scope/table → Model type map ──

template <fixed_string Scope, fixed_string Table, typename Model>
struct Bind {
    static constexpr auto scope = Scope;
    static constexpr auto table = Table;
    using model_type = Model;
};

// ── Schema: parameter pack of Bind<>... ──

template <typename... Bindings>
struct Schema {
    static constexpr std::size_t binding_count = sizeof...(Bindings);

    /// Check at compile time whether a scope/table pair exists.
    template <fixed_string Scope, fixed_string Table>
    static constexpr bool has_binding = (... || (Bindings::scope == Scope && Bindings::table == Table));

    /// Resolve the model type for a scope/table pair. Fails at compile time if not found.
    template <fixed_string Scope, fixed_string Table>
    struct resolve;
};

template <typename... Bindings>
template <fixed_string Scope, fixed_string Table>
struct Schema<Bindings...>::resolve {
    // Filter to the matching binding
    template <typename B, typename... Rest>
    static consteval auto find() {
        if constexpr (B::scope == Scope && B::table == Table) {
            return static_cast<typename B::model_type*>(nullptr);
        } else if constexpr (sizeof...(Rest) > 0) {
            return find<Rest...>();
        } else {
            static_assert(sizeof...(Rest) > 0, "Schema binding not found");
        }
    }

    using type = std::remove_pointer_t<decltype(find<Bindings...>())>;
};

// Convenience alias — requires the full Schema type, not the pack.
// Usage: using T = resolve_t<MySchema, "scope", "table">;
template <typename SchemaT, fixed_string Scope, fixed_string Table>
using resolve_t = typename SchemaT::template resolve<Scope, Table>::type;

} // namespace celer
