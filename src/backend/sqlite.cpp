#include "celer/backend/sqlite.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace celer {

#if CELER_HAS_SQLITE

// ── RAII wrapper for sqlite3_stmt ──

namespace {

struct StmtGuard {
    ::sqlite3_stmt* stmt{nullptr};
    StmtGuard() = default;
    explicit StmtGuard(::sqlite3_stmt* s) noexcept : stmt(s) {}
    ~StmtGuard() { if (stmt) ::sqlite3_finalize(stmt); }
    StmtGuard(const StmtGuard&) = delete;
    auto operator=(const StmtGuard&) -> StmtGuard& = delete;
    StmtGuard(StmtGuard&& o) noexcept : stmt(std::exchange(o.stmt, nullptr)) {}
    auto operator=(StmtGuard&& o) noexcept -> StmtGuard& {
        if (this != &o) { if (stmt) ::sqlite3_finalize(stmt); stmt = std::exchange(o.stmt, nullptr); }
        return *this;
    }
};

/// Build the prefix upper-bound for range queries: "abc" → "abd".
/// Returns empty string if no upper bound can be computed (e.g., all 0xFF bytes).
auto prefix_upper_bound(std::string_view prefix) -> std::string {
    std::string bound(prefix);
    while (!bound.empty()) {
        auto& last = bound.back();
        if (static_cast<unsigned char>(last) < 0xFF) {
            ++last;
            return bound;
        }
        bound.pop_back(); // carry: 0xFF → remove and increment previous
    }
    return {}; // all 0xFF — no upper bound
}

/// Validate that a name is safe for use as a SQL identifier.
/// Allows only non-empty strings of ASCII letters, digits, and underscores.
/// This is the primary defense against SQL injection for table/scope names;
/// quote_ident() below provides a secondary defense-in-depth layer.
auto validate_ident(std::string_view name) -> VoidResult {
    if (name.empty()) {
        return std::unexpected(Error{"SQLiteValidation", "identifier must not be empty"});
    }
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return std::unexpected(Error{"SQLiteValidation",
                "identifier contains invalid character: only [a-zA-Z0-9_] allowed"});
        }
    }
    return {};
}

/// Escape a SQL identifier: double any embedded double-quote characters.
/// Returns the identifier wrapped in double quotes, safe for use in SQL.
/// Defense-in-depth only — callers should validate_ident() first.
auto quote_ident(std::string_view name) -> std::string {
    std::string out;
    out.reserve(name.size() + 2);
    out.push_back('"');
    for (char c : name) {
        if (c == '\0') continue; // strip NUL bytes to prevent C-string truncation
        if (c == '"') out.push_back('"'); // double the quote to escape
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

// ── StorageBackend method implementations ──
// Each method uses table_name_ (the real SQL table inside the shared scope DB).

auto SQLiteBackend::get(std::string_view key) -> Result<std::optional<std::string>> {
    if (!db_) return std::unexpected(Error{"SQLiteGet", "db is closed"});

    auto sql = "SELECT value FROM " + quote_ident(table_name_) + " WHERE key = ?";
    ::sqlite3_stmt* raw_stmt = nullptr;
    int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
    StmtGuard guard(raw_stmt);
    if (rc != SQLITE_OK) {
        return std::unexpected(Error{"StoreGet", ::sqlite3_errmsg(db_.get())});
    }

    ::sqlite3_bind_text(raw_stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);

    rc = ::sqlite3_step(raw_stmt);
    if (rc == SQLITE_DONE) {
        return std::optional<std::string>{std::nullopt}; // not found
    }
    if (rc != SQLITE_ROW) {
        return std::unexpected(Error{"StoreGet", ::sqlite3_errmsg(db_.get())});
    }

    const auto* blob = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 0));
    int len = ::sqlite3_column_bytes(raw_stmt, 0);
    return std::optional<std::string>{std::string(blob, static_cast<std::size_t>(len))};
}

auto SQLiteBackend::put(std::string_view key, std::string_view value) -> VoidResult {
    if (!db_) return std::unexpected(Error{"SQLitePut", "db is closed"});

    auto sql = "INSERT OR REPLACE INTO " + quote_ident(table_name_) + " (key, value) VALUES (?, ?)";
    ::sqlite3_stmt* raw_stmt = nullptr;
    int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
    StmtGuard guard(raw_stmt);
    if (rc != SQLITE_OK) {
        return std::unexpected(Error{"StorePut", ::sqlite3_errmsg(db_.get())});
    }

    ::sqlite3_bind_text(raw_stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);
    ::sqlite3_bind_blob(raw_stmt, 2, value.data(), static_cast<int>(value.size()), SQLITE_STATIC);

    rc = ::sqlite3_step(raw_stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(Error{"StorePut", ::sqlite3_errmsg(db_.get())});
    }
    return {};
}

auto SQLiteBackend::del(std::string_view key) -> VoidResult {
    if (!db_) return std::unexpected(Error{"SQLiteDel", "db is closed"});

    auto sql = "DELETE FROM " + quote_ident(table_name_) + " WHERE key = ?";
    ::sqlite3_stmt* raw_stmt = nullptr;
    int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
    StmtGuard guard(raw_stmt);
    if (rc != SQLITE_OK) {
        return std::unexpected(Error{"StoreDel", ::sqlite3_errmsg(db_.get())});
    }

    ::sqlite3_bind_text(raw_stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);

    rc = ::sqlite3_step(raw_stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(Error{"StoreDel", ::sqlite3_errmsg(db_.get())});
    }
    return {};
}

auto SQLiteBackend::prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>> {
    if (!db_) return std::unexpected(Error{"SQLiteScan", "db is closed"});

    auto tbl = quote_ident(table_name_);
    std::vector<KVPair> results;

    if (prefix.empty()) {
        // Empty prefix → return all rows, ordered by key
        auto sql = "SELECT key, value FROM " + tbl + " ORDER BY key";
        ::sqlite3_stmt* raw_stmt = nullptr;
        int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
        StmtGuard guard(raw_stmt);
        if (rc != SQLITE_OK) {
            return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_.get())});
        }

        while ((rc = ::sqlite3_step(raw_stmt)) == SQLITE_ROW) {
            const auto* k = reinterpret_cast<const char*>(::sqlite3_column_text(raw_stmt, 0));
            int klen = ::sqlite3_column_bytes(raw_stmt, 0);
            const auto* v = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 1));
            int vlen = ::sqlite3_column_bytes(raw_stmt, 1);
            results.push_back(KVPair{
                std::string(k, static_cast<std::size_t>(klen)),
                std::string(v, static_cast<std::size_t>(vlen))
            });
        }
        if (rc != SQLITE_DONE) {
            return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_.get())});
        }
    } else {
        auto upper = prefix_upper_bound(prefix);
        if (upper.empty()) {
            // Prefix is all 0xFF — use >= only
            auto sql = "SELECT key, value FROM " + tbl + " WHERE key >= ? ORDER BY key";
            ::sqlite3_stmt* raw_stmt = nullptr;
            int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_.get())});
            }
            ::sqlite3_bind_text(raw_stmt, 1, prefix.data(), static_cast<int>(prefix.size()), SQLITE_STATIC);

            while ((rc = ::sqlite3_step(raw_stmt)) == SQLITE_ROW) {
                const auto* k = reinterpret_cast<const char*>(::sqlite3_column_text(raw_stmt, 0));
                int klen = ::sqlite3_column_bytes(raw_stmt, 0);
                std::string key_str(k, static_cast<std::size_t>(klen));
                if (!key_str.starts_with(prefix)) break;
                const auto* v = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 1));
                int vlen = ::sqlite3_column_bytes(raw_stmt, 1);
                results.push_back(KVPair{std::move(key_str), std::string(v, static_cast<std::size_t>(vlen))});
            }
        } else {
            auto sql = "SELECT key, value FROM " + tbl + " WHERE key >= ? AND key < ? ORDER BY key";
            ::sqlite3_stmt* raw_stmt = nullptr;
            int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_.get())});
            }
            ::sqlite3_bind_text(raw_stmt, 1, prefix.data(), static_cast<int>(prefix.size()), SQLITE_STATIC);
            ::sqlite3_bind_text(raw_stmt, 2, upper.data(), static_cast<int>(upper.size()), SQLITE_STATIC);

            while ((rc = ::sqlite3_step(raw_stmt)) == SQLITE_ROW) {
                const auto* k = reinterpret_cast<const char*>(::sqlite3_column_text(raw_stmt, 0));
                int klen = ::sqlite3_column_bytes(raw_stmt, 0);
                const auto* v = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 1));
                int vlen = ::sqlite3_column_bytes(raw_stmt, 1);
                results.push_back(KVPair{
                    std::string(k, static_cast<std::size_t>(klen)),
                    std::string(v, static_cast<std::size_t>(vlen))
                });
            }
            if (rc != SQLITE_DONE) {
                return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_.get())});
            }
        }
    }

    return results;
}

auto SQLiteBackend::batch(std::span<const BatchOp> ops) -> VoidResult {
    if (!db_) return std::unexpected(Error{"SQLiteBatch", "db is closed"});

    // Use SAVEPOINT instead of BEGIN/COMMIT for safe nesting on shared connections.
    // Two tables in the same scope share one sqlite3* — plain BEGIN would conflict.
    auto sp = "sp_" + std::to_string(next_savepoint_id());
    auto sp_begin    = "SAVEPOINT "    + quote_ident(sp);
    auto sp_release  = "RELEASE "      + quote_ident(sp);
    auto sp_rollback = "ROLLBACK TO "  + quote_ident(sp);
    auto tbl         = quote_ident(table_name_);

    char* errmsg = nullptr;
    int rc = ::sqlite3_exec(db_.get(), sp_begin.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        ::sqlite3_free(errmsg);
        return std::unexpected(Error{"StoreBatch", msg});
    }

    for (const auto& op : ops) {
        if (op.kind == BatchOp::Kind::put) {
            if (!op.value) {
                ::sqlite3_exec(db_.get(), sp_rollback.c_str(), nullptr, nullptr, nullptr);
                ::sqlite3_exec(db_.get(), sp_release.c_str(), nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", "put op missing value"});
            }
            auto sql = "INSERT OR REPLACE INTO " + tbl + " (key, value) VALUES (?, ?)";
            ::sqlite3_stmt* raw_stmt = nullptr;
            rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                ::sqlite3_exec(db_.get(), sp_rollback.c_str(), nullptr, nullptr, nullptr);
                ::sqlite3_exec(db_.get(), sp_release.c_str(), nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", ::sqlite3_errmsg(db_.get())});
            }
            ::sqlite3_bind_text(raw_stmt, 1, op.key.data(), static_cast<int>(op.key.size()), SQLITE_STATIC);
            ::sqlite3_bind_blob(raw_stmt, 2, op.value->data(), static_cast<int>(op.value->size()), SQLITE_STATIC);
            rc = ::sqlite3_step(raw_stmt);
            if (rc != SQLITE_DONE) {
                ::sqlite3_exec(db_.get(), sp_rollback.c_str(), nullptr, nullptr, nullptr);
                ::sqlite3_exec(db_.get(), sp_release.c_str(), nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", ::sqlite3_errmsg(db_.get())});
            }
        } else {
            auto sql = "DELETE FROM " + tbl + " WHERE key = ?";
            ::sqlite3_stmt* raw_stmt = nullptr;
            rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                ::sqlite3_exec(db_.get(), sp_rollback.c_str(), nullptr, nullptr, nullptr);
                ::sqlite3_exec(db_.get(), sp_release.c_str(), nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", ::sqlite3_errmsg(db_.get())});
            }
            ::sqlite3_bind_text(raw_stmt, 1, op.key.data(), static_cast<int>(op.key.size()), SQLITE_STATIC);
            rc = ::sqlite3_step(raw_stmt);
            if (rc != SQLITE_DONE) {
                ::sqlite3_exec(db_.get(), sp_rollback.c_str(), nullptr, nullptr, nullptr);
                ::sqlite3_exec(db_.get(), sp_release.c_str(), nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", ::sqlite3_errmsg(db_.get())});
            }
        }
    }

    rc = ::sqlite3_exec(db_.get(), sp_release.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        ::sqlite3_free(errmsg);
        return std::unexpected(Error{"StoreBatch", msg});
    }
    return {};
}

auto SQLiteBackend::compact() -> VoidResult {
    if (!db_) return std::unexpected(Error{"SQLiteCompact", "db is closed"});

    // SQLite's equivalent of compaction is VACUUM
    char* errmsg = nullptr;
    int rc = ::sqlite3_exec(db_.get(), "VACUUM", nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        ::sqlite3_free(errmsg);
        return std::unexpected(Error{"StoreCompact", msg});
    }
    return {};
}

auto SQLiteBackend::foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx)
    -> VoidResult {
    if (!db_) return std::unexpected(Error{"SQLiteForeach", "db is closed"});

    auto tbl = quote_ident(table_name_);

    if (prefix.empty()) {
        auto sql = "SELECT key, value FROM " + tbl + " ORDER BY key";
        ::sqlite3_stmt* raw_stmt = nullptr;
        int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
        StmtGuard guard(raw_stmt);
        if (rc != SQLITE_OK) {
            return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_.get())});
        }

        while ((rc = ::sqlite3_step(raw_stmt)) == SQLITE_ROW) {
            const auto* k = reinterpret_cast<const char*>(::sqlite3_column_text(raw_stmt, 0));
            int klen = ::sqlite3_column_bytes(raw_stmt, 0);
            const auto* v = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 1));
            int vlen = ::sqlite3_column_bytes(raw_stmt, 1);
            visitor(user_ctx, std::string_view(k, static_cast<std::size_t>(klen)),
                              std::string_view(v, static_cast<std::size_t>(vlen)));
        }
        if (rc != SQLITE_DONE) {
            return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_.get())});
        }
    } else {
        auto upper = prefix_upper_bound(prefix);
        if (upper.empty()) {
            auto sql = "SELECT key, value FROM " + tbl + " WHERE key >= ? ORDER BY key";
            ::sqlite3_stmt* raw_stmt = nullptr;
            int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_.get())});
            }
            ::sqlite3_bind_text(raw_stmt, 1, prefix.data(), static_cast<int>(prefix.size()), SQLITE_STATIC);

            while ((rc = ::sqlite3_step(raw_stmt)) == SQLITE_ROW) {
                const auto* k = reinterpret_cast<const char*>(::sqlite3_column_text(raw_stmt, 0));
                int klen = ::sqlite3_column_bytes(raw_stmt, 0);
                std::string_view key_sv(k, static_cast<std::size_t>(klen));
                if (!key_sv.starts_with(prefix)) break;
                const auto* v = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 1));
                int vlen = ::sqlite3_column_bytes(raw_stmt, 1);
                visitor(user_ctx, key_sv, std::string_view(v, static_cast<std::size_t>(vlen)));
            }
        } else {
            auto sql = "SELECT key, value FROM " + tbl + " WHERE key >= ? AND key < ? ORDER BY key";
            ::sqlite3_stmt* raw_stmt = nullptr;
            int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_.get())});
            }
            ::sqlite3_bind_text(raw_stmt, 1, prefix.data(), static_cast<int>(prefix.size()), SQLITE_STATIC);
            ::sqlite3_bind_text(raw_stmt, 2, upper.data(), static_cast<int>(upper.size()), SQLITE_STATIC);

            while ((rc = ::sqlite3_step(raw_stmt)) == SQLITE_ROW) {
                const auto* k = reinterpret_cast<const char*>(::sqlite3_column_text(raw_stmt, 0));
                int klen = ::sqlite3_column_bytes(raw_stmt, 0);
                const auto* v = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 1));
                int vlen = ::sqlite3_column_bytes(raw_stmt, 1);
                visitor(user_ctx, std::string_view(k, static_cast<std::size_t>(klen)),
                                  std::string_view(v, static_cast<std::size_t>(vlen)));
            }
            if (rc != SQLITE_DONE) {
                return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_.get())});
            }
        }
    }

    return {};
}

auto SQLiteBackend::close() -> VoidResult {
    db_.reset(); // shared_ptr — last handle triggers SqliteDeleter
    return {};
}

// ── Streaming stubs (RFC-002) — default implementations via materialization ──
// These satisfy the StorageBackend concept. Future enhancement: native
// chunked SQLite cursors for true streaming without full materialization.

auto SQLiteBackend::stream_get(std::string_view key) -> Result<StreamHandle<char>> {
    auto r = get(key);
    if (!r) return std::unexpected(r.error());
    if (!r->has_value()) return stream::empty<char>();
    return stream::from_string(std::move(r->value()));
}

auto SQLiteBackend::stream_put(std::string_view key, StreamHandle<char> input) -> VoidResult {
    auto collected = stream::collect_string(input);
    if (!collected) return std::unexpected(collected.error());
    return put(key, *collected);
}

auto SQLiteBackend::stream_scan(std::string_view prefix) -> Result<StreamHandle<KVPair>> {
    auto r = prefix_scan(prefix);
    if (!r) return std::unexpected(r.error());
    return stream::from_vector(std::move(*r));
}

// ── Batch get (RFC-005) — native IN-clause prepared statement ──
//
// SQLite has a default SQLITE_LIMIT_VARIABLE_NUMBER of 999 (older builds) or
// 32766 (newer builds). We chunk into batches of 256 to stay well under both
// while still amortizing the prepare cost across many keys.
auto SQLiteBackend::get_many(std::span<const std::string_view> keys)
    -> Result<std::vector<BatchGetItem>> {
    if (!db_) return std::unexpected(Error{"SQLiteGetMany", "db is closed"});
    const auto n = keys.size();
    std::vector<BatchGetItem> out;
    out.reserve(n);
    if (n == 0) return out;

    // Result map: key -> value (sqlite IN does not preserve order).
    std::unordered_map<std::string_view, std::optional<std::string>> found;
    found.reserve(n);

    constexpr std::size_t kChunk = 256;
    auto tbl = quote_ident(table_name_);

    for (std::size_t base = 0; base < n; base += kChunk) {
        const auto cnt = std::min(kChunk, n - base);
        std::string sql = "SELECT key, value FROM " + tbl + " WHERE key IN (";
        for (std::size_t i = 0; i < cnt; ++i) sql += (i == 0 ? "?" : ",?");
        sql += ")";

        ::sqlite3_stmt* raw_stmt = nullptr;
        int rc = ::sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
        StmtGuard guard(raw_stmt);
        if (rc != SQLITE_OK) {
            return std::unexpected(Error{"SQLiteGetMany", ::sqlite3_errmsg(db_.get())});
        }
        for (std::size_t i = 0; i < cnt; ++i) {
            auto k = keys[base + i];
            ::sqlite3_bind_text(raw_stmt, static_cast<int>(i + 1),
                                k.data(), static_cast<int>(k.size()), SQLITE_STATIC);
        }
        while (true) {
            rc = ::sqlite3_step(raw_stmt);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                return std::unexpected(Error{"SQLiteGetMany", ::sqlite3_errmsg(db_.get())});
            }
            const auto* kbuf = reinterpret_cast<const char*>(::sqlite3_column_text(raw_stmt, 0));
            int klen = ::sqlite3_column_bytes(raw_stmt, 0);
            const auto* vbuf = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 1));
            int vlen = ::sqlite3_column_bytes(raw_stmt, 1);
            // Match against the live key-view by copying into the lookup map.
            std::string_view kv{kbuf, static_cast<std::size_t>(klen)};
            // We have to find the matching input key_view (same content) to use as the map key.
            for (std::size_t i = 0; i < cnt; ++i) {
                if (keys[base + i] == kv) {
                    found.emplace(keys[base + i],
                                  std::string(vbuf, static_cast<std::size_t>(vlen)));
                    break;
                }
            }
        }
    }

    for (auto k : keys) {
        auto it = found.find(k);
        if (it == found.end()) {
            out.push_back(BatchGetItem{std::string(k), std::nullopt});
        } else {
            out.push_back(BatchGetItem{std::string(k), std::move(it->second)});
        }
    }
    return out;
}

namespace {

/// Open (or reuse) a scope-level SQLite database at path/<scope>.db.
/// Returns a shared_ptr so multiple tables in the same scope share one connection.
auto open_scope_db(const backends::sqlite::Config& config, const std::string& db_path)
    -> Result<SqliteDbPtr> {
    // Create parent directories
    auto parent = std::filesystem::path(db_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(Error{"SQLiteOpen",
                "failed to create directory '" + parent.string() + "': " + ec.message()});
        }
    }

    int flags = SQLITE_OPEN_READWRITE;
    if (config.create_if_missing) {
        flags |= SQLITE_OPEN_CREATE;
    }
    // Enable serialized (fully thread-safe) mode for this connection
    flags |= SQLITE_OPEN_FULLMUTEX;

    ::sqlite3* raw_db = nullptr;
    int rc = ::sqlite3_open_v2(db_path.c_str(), &raw_db, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = raw_db ? ::sqlite3_errmsg(raw_db) : "unknown error";
        if (raw_db) ::sqlite3_close(raw_db);
        return std::unexpected(Error{"SQLiteOpen",
            "failed to open SQLite at '" + db_path + "': " + msg});
    }

    // Set busy timeout
    ::sqlite3_busy_timeout(raw_db, config.busy_timeout_ms);

    // Enable WAL mode if requested
    if (config.enable_wal) {
        char* errmsg = nullptr;
        rc = ::sqlite3_exec(raw_db, "PRAGMA journal_mode=WAL", nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            std::string msg = errmsg ? errmsg : "unknown error";
            ::sqlite3_free(errmsg);
            ::sqlite3_close(raw_db);
            return std::unexpected(Error{"SQLiteOpen", "failed to set WAL mode: " + msg});
        }
    }

    return SqliteDbPtr(raw_db, SqliteDeleter{});
}

/// Create a named SQL table inside an already-open scope DB and return a BackendHandle.
auto open_table(const SqliteDbPtr& db, std::string_view table_name)
    -> Result<BackendHandle> {
    // Primary defense: reject any name that isn't strictly alphanumeric/underscore.
    // This prevents SQL injection regardless of quoting correctness.
    if (auto v = validate_ident(table_name); !v) return std::unexpected(v.error());

    // Create the table (named after the logical table) if it doesn't exist
    auto create_sql = "CREATE TABLE IF NOT EXISTS " + quote_ident(table_name) +
                      " (key TEXT PRIMARY KEY, value BLOB NOT NULL)";
    char* errmsg = nullptr;
    int rc = ::sqlite3_exec(db.get(), create_sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        ::sqlite3_free(errmsg);
        return std::unexpected(Error{"SQLiteOpen",
            "failed to create table '" + std::string(table_name) + "': " + msg});
    }

    auto* backend = new SQLiteBackend(db, std::string(table_name));
    return make_backend_handle<SQLiteBackend>(backend);
}

} // namespace

namespace backends::sqlite {

auto factory(Config cfg) -> BackendFactory {
    // Cache: one shared sqlite3 connection per scope.
    // Protected by mutex since build_tree calls the factory sequentially,
    // but we guard anyway for safety.
    struct SharedState {
        std::mutex mu;
        std::unordered_map<std::string, SqliteDbPtr> scope_dbs;
    };
    auto state = std::make_shared<SharedState>();

    return [c = std::move(cfg), state](std::string_view scope, std::string_view table) -> Result<BackendHandle> {
        // Validate scope name — it becomes part of a file path AND could be used as an identifier.
        if (auto v = validate_ident(scope); !v) return std::unexpected(v.error());

        std::string scope_key(scope);
        SqliteDbPtr db;

        {
            std::lock_guard lock(state->mu);
            auto it = state->scope_dbs.find(scope_key);
            if (it != state->scope_dbs.end()) {
                db = it->second;
            } else {
                auto db_path = c.path + "/" + scope_key + ".db";
                auto r = open_scope_db(c, db_path);
                if (!r) return std::unexpected(r.error());
                db = std::move(*r);
                state->scope_dbs[scope_key] = db;
            }
        }

        return open_table(db, table);
    };
}

} // namespace backends::sqlite

#else // CELER_HAS_SQLITE == 0

namespace backends::sqlite {

auto factory(Config /*cfg*/) -> BackendFactory {
    return [](std::string_view, std::string_view) -> Result<BackendHandle> {
        return std::unexpected(Error{"NotAvailable", "compiled without SQLite support"});
    };
}

} // namespace backends::sqlite

#endif

} // namespace celer
