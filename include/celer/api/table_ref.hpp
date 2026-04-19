#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "celer/api/result_set.hpp"
#include "celer/backend/concept.hpp"
#include "celer/core/dispatch.hpp"
#include "celer/core/result.hpp"
#include "celer/serde/codec.hpp"

namespace celer {

/// TableRef — direct dispatch to a single leaf's BackendHandle.
/// This is the main workhorse: put/get/all/prefix/foreach.
class TableRef {
public:
    TableRef(std::string table_name, const BackendHandle* handle)
        : table_name_(std::move(table_name)), handle_(handle) {}

    /// Get a typed value by key.
    template <typename T>
    [[nodiscard]] auto get(std::string_view key) const -> Result<std::optional<T>> {
        auto raw = handle_->get(key);
        if (!raw) return std::unexpected(raw.error());
        if (!raw->has_value()) return std::optional<T>{std::nullopt};
        auto decoded = codec_decode<T>(raw->value());
        if (!decoded) return std::unexpected(decoded.error());
        return std::optional<T>{std::move(*decoded)};
    }

    /// Put a typed value.
    template <typename T>
    [[nodiscard]] auto put(std::string_view key, const T& value) const -> VoidResult {
        auto encoded = codec_encode(value);
        if (!encoded) return std::unexpected(encoded.error());
        return handle_->put(key, *encoded);
    }

    /// Delete a key.
    [[nodiscard]] auto del(std::string_view key) const -> VoidResult {
        return handle_->del(key);
    }

    /// Get all rows as a typed ResultSet.
    template <typename T>
    [[nodiscard]] auto all() const -> Result<ResultSet<T>> {
        auto raw = handle_->prefix_scan("");
        if (!raw) return std::unexpected(raw.error());
        return from_raw_pairs<T>(*raw);
    }

    /// Prefix scan as a typed ResultSet.
    template <typename T>
    [[nodiscard]] auto prefix(std::string_view pfx) const -> Result<ResultSet<T>> {
        auto raw = handle_->prefix_scan(pfx);
        if (!raw) return std::unexpected(raw.error());
        return from_raw_pairs<T>(*raw);
    }

    /// Zero-copy foreach scan — never materializes a vector.
    template <typename T, typename Fn>
    [[nodiscard]] auto foreach(Fn&& callback) const -> VoidResult {
        // Wrap the user callback into a C-style visitor that deserializes on the fly.
        struct Ctx {
            Fn* fn;
        };
        Ctx ctx{&callback};
        ScanVisitor visitor = [](void* raw_ctx, std::string_view /*key*/, std::string_view value) {
            auto* c = static_cast<Ctx*>(raw_ctx);
            if (auto decoded = codec_decode<T>(value); decoded) {
                (*c->fn)(std::move(*decoded));
            }
        };
        return handle_->foreach_scan("", visitor, &ctx);
    }

    /// Compact the underlying storage.
    [[nodiscard]] auto compact() const -> VoidResult {
        return handle_->compact();
    }

    /// Raw put — bypasses serde.
    [[nodiscard]] auto put_raw(std::string_view key, std::string_view value) const -> VoidResult {
        return handle_->put(key, value);
    }

    /// Raw get — bypasses serde.
    [[nodiscard]] auto get_raw(std::string_view key) const -> Result<std::optional<std::string>> {
        return handle_->get(key);
    }

    [[nodiscard]] auto name() const noexcept -> const std::string& { return table_name_; }

    /// Raw handle accessor. Used by celer::materialization::StoreRef<T> and
    /// other typed lenses that need to dispatch through the BackendHandle
    /// vtable directly. The returned pointer is non-owning and must not
    /// outlive the underlying Store.
    [[nodiscard]] auto handle() const noexcept -> const BackendHandle* { return handle_; }

private:
    std::string            table_name_;
    const BackendHandle*   handle_;
};

} // namespace celer
