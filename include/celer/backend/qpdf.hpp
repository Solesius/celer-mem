#pragma once
/// celer QPDF backend — satisfies StorageBackend concept via libqpdf.
///
/// Architecture:
///   - One PDF file per scope (at path/<scope>.pdf)
///   - Multiple tables share one QPDF object (mutex-protected)
///   - KV data stored in a /CelerKV dictionary in the PDF catalog
///   - Each table is a sub-dictionary: /CelerKV/<table> → { <key>: <value>, ... }
///   - Keys encoded as PDF name objects; values as PDF string objects
///   - Writes modify in-memory QPDF; flush on compact()/close()
///
/// Design principles:
///   - Okasaki immutability: tree built at startup, shared lock-free
///   - RAII: shared_ptr to scope state, last handle flushes and closes
///   - constexpr vtable: zero-overhead type-erasure via BackendVTable
///   - Security: validate_ident on all scope/table names, PDF name encoding
///     for keys prevents injection in both filesystem and PDF object layers
///
/// Requires: libqpdf (https://github.com/qpdf/qpdf)
/// Detection: compile-time via __has_include(<qpdf/QPDF.hh>)

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "celer/backend/concept.hpp"
#include "celer/core/result.hpp"

#if !defined(CELER_FORCE_NO_QPDF) && __has_include(<qpdf/QPDF.hh>)
#  define CELER_HAS_QPDF 1
// QPDF 11.x: opt into std::shared_ptr migration, suppress deprecated
// PointerHolder<T> warnings.
#  ifndef POINTERHOLDER_TRANSITION
#    define POINTERHOLDER_TRANSITION 4
#  endif
#  include <qpdf/QPDF.hh>
#  include <qpdf/QPDFObjectHandle.hh>
#  include <qpdf/QPDFWriter.hh>
#else
#  define CELER_HAS_QPDF 0
#endif

namespace celer {

namespace backends::qpdf {

/// QPDF-specific configuration.
struct Config {
    std::string path;                  ///< Directory for PDF files (one file per scope)
    bool        create_if_missing = true;
    bool        linearize         = false; ///< Linearize PDFs for web delivery on save
};

/// Returns a BackendFactory that opens one PDF file per scope
/// at path/<scope>.pdf, with each logical table as a sub-dictionary
/// inside the PDF's /CelerKV catalog entry.
[[nodiscard]] auto factory(Config cfg) -> BackendFactory;

} // namespace backends::qpdf

#if CELER_HAS_QPDF

/// Shared mutable state for one QPDF scope (one PDF file).
/// Protected by mutex; multiple QPDFBackend instances (one per table)
/// share ownership via shared_ptr.
struct QPDFScopeState {
    std::mutex          mu;
    std::unique_ptr<QPDF> pdf;
    std::string         file_path;
    bool                dirty{false};
    bool                linearize{false};

    /// Flush in-memory PDF to disk (caller must hold mu).
    [[nodiscard]] auto flush_locked() -> VoidResult;
};

using QPDFScopeStatePtr = std::shared_ptr<QPDFScopeState>;

/// QPDFBackend — satisfies StorageBackend concept.
/// One shared PDF file per scope; each logical table is a sub-dictionary
/// inside the PDF's /CelerKV catalog entry.
class QPDFBackend {
public:
    QPDFBackend() = default;

    QPDFBackend(QPDFScopeStatePtr state, std::string table_name) noexcept
        : state_(std::move(state)), table_name_(std::move(table_name)) {}

    QPDFBackend(QPDFBackend&&) noexcept            = default;
    auto operator=(QPDFBackend&&) noexcept -> QPDFBackend& = default;

    QPDFBackend(const QPDFBackend&)            = delete;
    auto operator=(const QPDFBackend&) -> QPDFBackend& = delete;

    ~QPDFBackend() { (void)close(); }

    [[nodiscard]] static constexpr auto name() noexcept -> std::string_view { return "qpdf"; }

    [[nodiscard]] auto get(std::string_view key) -> Result<std::optional<std::string>>;
    [[nodiscard]] auto put(std::string_view key, std::string_view value) -> VoidResult;
    [[nodiscard]] auto del(std::string_view key) -> VoidResult;
    [[nodiscard]] auto prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>>;
    [[nodiscard]] auto batch(std::span<const BatchOp> ops) -> VoidResult;
    [[nodiscard]] auto compact() -> VoidResult;
    [[nodiscard]] auto foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx) -> VoidResult;

    // Streaming extensions (RFC-002) — native pull-based implementations.
    // stream_get / stream_scan are the primary data path; materializing
    // methods (get, prefix_scan, foreach_scan) delegate to these.
    [[nodiscard]] auto stream_get(std::string_view key) -> Result<StreamHandle<char>>;
    [[nodiscard]] auto stream_put(std::string_view key, StreamHandle<char> stream) -> VoidResult;
    [[nodiscard]] auto stream_scan(std::string_view prefix) -> Result<StreamHandle<KVPair>>;

    auto close() -> VoidResult;

private:
    QPDFScopeStatePtr state_;
    std::string       table_name_;

    /// Get (or create) the table dictionary for this backend (caller must hold state_->mu).
    [[nodiscard]] auto get_table_dict_locked() -> QPDFObjectHandle;
};

#endif // CELER_HAS_QPDF

} // namespace celer
