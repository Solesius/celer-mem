#include "celer/backend/sqlite.hpp"

#include <filesystem>
#include <string>

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

} // namespace

// ── StorageBackend method implementations ──

auto SQLiteBackend::get(std::string_view key) -> Result<std::optional<std::string>> {
    if (!db_) return std::unexpected(Error{"SQLiteGet", "db is closed"});

    const char* sql = "SELECT value FROM kv WHERE key = ?";
    ::sqlite3_stmt* raw_stmt = nullptr;
    int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
    StmtGuard guard(raw_stmt);
    if (rc != SQLITE_OK) {
        return std::unexpected(Error{"StoreGet", ::sqlite3_errmsg(db_)});
    }

    ::sqlite3_bind_text(raw_stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);

    rc = ::sqlite3_step(raw_stmt);
    if (rc == SQLITE_DONE) {
        return std::optional<std::string>{std::nullopt}; // not found
    }
    if (rc != SQLITE_ROW) {
        return std::unexpected(Error{"StoreGet", ::sqlite3_errmsg(db_)});
    }

    const auto* blob = reinterpret_cast<const char*>(::sqlite3_column_blob(raw_stmt, 0));
    int len = ::sqlite3_column_bytes(raw_stmt, 0);
    return std::optional<std::string>{std::string(blob, static_cast<std::size_t>(len))};
}

auto SQLiteBackend::put(std::string_view key, std::string_view value) -> VoidResult {
    if (!db_) return std::unexpected(Error{"SQLitePut", "db is closed"});

    const char* sql = "INSERT OR REPLACE INTO kv (key, value) VALUES (?, ?)";
    ::sqlite3_stmt* raw_stmt = nullptr;
    int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
    StmtGuard guard(raw_stmt);
    if (rc != SQLITE_OK) {
        return std::unexpected(Error{"StorePut", ::sqlite3_errmsg(db_)});
    }

    ::sqlite3_bind_text(raw_stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);
    ::sqlite3_bind_blob(raw_stmt, 2, value.data(), static_cast<int>(value.size()), SQLITE_STATIC);

    rc = ::sqlite3_step(raw_stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(Error{"StorePut", ::sqlite3_errmsg(db_)});
    }
    return {};
}

auto SQLiteBackend::del(std::string_view key) -> VoidResult {
    if (!db_) return std::unexpected(Error{"SQLiteDel", "db is closed"});

    const char* sql = "DELETE FROM kv WHERE key = ?";
    ::sqlite3_stmt* raw_stmt = nullptr;
    int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
    StmtGuard guard(raw_stmt);
    if (rc != SQLITE_OK) {
        return std::unexpected(Error{"StoreDel", ::sqlite3_errmsg(db_)});
    }

    ::sqlite3_bind_text(raw_stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);

    rc = ::sqlite3_step(raw_stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(Error{"StoreDel", ::sqlite3_errmsg(db_)});
    }
    return {};
}

auto SQLiteBackend::prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>> {
    if (!db_) return std::unexpected(Error{"SQLiteScan", "db is closed"});

    std::vector<KVPair> results;

    if (prefix.empty()) {
        // Empty prefix → return all rows, ordered by key
        const char* sql = "SELECT key, value FROM kv ORDER BY key";
        ::sqlite3_stmt* raw_stmt = nullptr;
        int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
        StmtGuard guard(raw_stmt);
        if (rc != SQLITE_OK) {
            return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_)});
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
            return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_)});
        }
    } else {
        auto upper = prefix_upper_bound(prefix);
        if (upper.empty()) {
            // Prefix is all 0xFF — use >= only
            const char* sql = "SELECT key, value FROM kv WHERE key >= ? ORDER BY key";
            ::sqlite3_stmt* raw_stmt = nullptr;
            int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_)});
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
            const char* sql = "SELECT key, value FROM kv WHERE key >= ? AND key < ? ORDER BY key";
            ::sqlite3_stmt* raw_stmt = nullptr;
            int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_)});
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
                return std::unexpected(Error{"StorePrefixScan", ::sqlite3_errmsg(db_)});
            }
        }
    }

    return results;
}

auto SQLiteBackend::batch(std::span<const BatchOp> ops) -> VoidResult {
    if (!db_) return std::unexpected(Error{"SQLiteBatch", "db is closed"});

    // Execute as a single transaction for atomicity
    char* errmsg = nullptr;
    int rc = ::sqlite3_exec(db_, "BEGIN", nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        ::sqlite3_free(errmsg);
        return std::unexpected(Error{"StoreBatch", msg});
    }

    for (const auto& op : ops) {
        if (op.kind == BatchOp::Kind::put) {
            if (!op.value) {
                ::sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", "put op missing value"});
            }
            const char* sql = "INSERT OR REPLACE INTO kv (key, value) VALUES (?, ?)";
            ::sqlite3_stmt* raw_stmt = nullptr;
            rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                ::sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", ::sqlite3_errmsg(db_)});
            }
            ::sqlite3_bind_text(raw_stmt, 1, op.key.data(), static_cast<int>(op.key.size()), SQLITE_STATIC);
            ::sqlite3_bind_blob(raw_stmt, 2, op.value->data(), static_cast<int>(op.value->size()), SQLITE_STATIC);
            rc = ::sqlite3_step(raw_stmt);
            if (rc != SQLITE_DONE) {
                ::sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", ::sqlite3_errmsg(db_)});
            }
        } else {
            const char* sql = "DELETE FROM kv WHERE key = ?";
            ::sqlite3_stmt* raw_stmt = nullptr;
            rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                ::sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", ::sqlite3_errmsg(db_)});
            }
            ::sqlite3_bind_text(raw_stmt, 1, op.key.data(), static_cast<int>(op.key.size()), SQLITE_STATIC);
            rc = ::sqlite3_step(raw_stmt);
            if (rc != SQLITE_DONE) {
                ::sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                return std::unexpected(Error{"StoreBatch", ::sqlite3_errmsg(db_)});
            }
        }
    }

    rc = ::sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &errmsg);
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
    int rc = ::sqlite3_exec(db_, "VACUUM", nullptr, nullptr, &errmsg);
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

    if (prefix.empty()) {
        const char* sql = "SELECT key, value FROM kv ORDER BY key";
        ::sqlite3_stmt* raw_stmt = nullptr;
        int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
        StmtGuard guard(raw_stmt);
        if (rc != SQLITE_OK) {
            return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_)});
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
            return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_)});
        }
    } else {
        auto upper = prefix_upper_bound(prefix);
        if (upper.empty()) {
            const char* sql = "SELECT key, value FROM kv WHERE key >= ? ORDER BY key";
            ::sqlite3_stmt* raw_stmt = nullptr;
            int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_)});
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
            const char* sql = "SELECT key, value FROM kv WHERE key >= ? AND key < ? ORDER BY key";
            ::sqlite3_stmt* raw_stmt = nullptr;
            int rc = ::sqlite3_prepare_v2(db_, sql, -1, &raw_stmt, nullptr);
            StmtGuard guard(raw_stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_)});
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
                return std::unexpected(Error{"StoreForeachScan", ::sqlite3_errmsg(db_)});
            }
        }
    }

    return {};
}

auto SQLiteBackend::close() -> VoidResult {
    if (db_) {
        ::sqlite3_close(db_);
        db_ = nullptr;
    }
    return {};
}

namespace {

/// Open a single SQLite database at the resolved path.
auto open_single(const backends::sqlite::Config& config, const std::string& resolved_path)
    -> Result<BackendHandle> {
    // Create parent directories
    auto parent = std::filesystem::path(resolved_path).parent_path();
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
    int rc = ::sqlite3_open_v2(resolved_path.c_str(), &raw_db, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = raw_db ? ::sqlite3_errmsg(raw_db) : "unknown error";
        if (raw_db) ::sqlite3_close(raw_db);
        return std::unexpected(Error{"SQLiteOpen",
            "failed to open SQLite at '" + resolved_path + "': " + msg});
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

    // Create the kv table if it doesn't exist
    {
        char* errmsg = nullptr;
        rc = ::sqlite3_exec(raw_db,
            "CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value BLOB NOT NULL)",
            nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            std::string msg = errmsg ? errmsg : "unknown error";
            ::sqlite3_free(errmsg);
            ::sqlite3_close(raw_db);
            return std::unexpected(Error{"SQLiteOpen", "failed to create kv table: " + msg});
        }
    }

    auto* backend = new SQLiteBackend(raw_db);
    return make_backend_handle<SQLiteBackend>(backend);
}

} // namespace

namespace backends::sqlite {

auto factory(Config cfg) -> BackendFactory {
    return [c = std::move(cfg)](std::string_view scope, std::string_view table) -> Result<BackendHandle> {
        auto resolved = c.path + "/" + std::string(scope) + "/" + std::string(table) + ".db";
        return open_single(c, resolved);
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
