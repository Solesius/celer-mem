#include "celer/backend/rocksdb.hpp"

#include <filesystem>

namespace celer {

#if CELER_HAS_ROCKSDB

// ── SML: rocksdb_get ──
auto RocksDBBackend::get(std::string_view key) -> Result<std::optional<std::string>> {
    if (!db_) return std::unexpected(Error{"RocksDBGet", "db is closed"});
    rocksdb::ReadOptions ro;
    std::string val;
    auto s = db_->Get(ro, rocksdb::Slice(key.data(), key.size()), &val);
    if (s.IsNotFound()) return std::optional<std::string>{std::nullopt};
    if (!s.ok())         return std::unexpected(Error{"StoreGet", s.ToString()});
    return std::optional<std::string>{std::move(val)};
}

// ── SML: rocksdb_put ──
auto RocksDBBackend::put(std::string_view key, std::string_view value) -> VoidResult {
    if (!db_) return std::unexpected(Error{"RocksDBPut", "db is closed"});
    rocksdb::WriteOptions wo;
    auto s = db_->Put(wo, rocksdb::Slice(key.data(), key.size()),
                          rocksdb::Slice(value.data(), value.size()));
    if (!s.ok()) return std::unexpected(Error{"StorePut", s.ToString()});
    return {};
}

// ── SML: rocksdb_del ──
auto RocksDBBackend::del(std::string_view key) -> VoidResult {
    if (!db_) return std::unexpected(Error{"RocksDBDel", "db is closed"});
    rocksdb::WriteOptions wo;
    auto s = db_->Delete(wo, rocksdb::Slice(key.data(), key.size()));
    if (!s.ok()) return std::unexpected(Error{"StoreDel", s.ToString()});
    return {};
}

// ── SML: rocksdb_prefix_scan ──
auto RocksDBBackend::prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>> {
    if (!db_) return std::unexpected(Error{"RocksDBScan", "db is closed"});
    rocksdb::ReadOptions ro;
    std::vector<KVPair> results;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));
    it->Seek(rocksdb::Slice(prefix.data(), prefix.size()));
    while (it->Valid()) {
        auto k = it->key().ToString();
        if (k.compare(0, prefix.size(), prefix.data(), prefix.size()) != 0) break;
        results.push_back(KVPair{std::move(k), it->value().ToString()});
        it->Next();
    }
    if (!it->status().ok()) {
        return std::unexpected(Error{"StorePrefixScan", it->status().ToString()});
    }
    return results;
}

// ── SML: rocksdb_batch ──
auto RocksDBBackend::batch(std::span<const BatchOp> ops) -> VoidResult {
    if (!db_) return std::unexpected(Error{"RocksDBBatch", "db is closed"});
    rocksdb::WriteBatch wb;
    for (const auto& op : ops) {
        rocksdb::Slice ks(op.key.data(), op.key.size());
        if (op.kind == BatchOp::Kind::put) {
            if (!op.value) return std::unexpected(Error{"StoreBatch", "put op missing value"});
            wb.Put(ks, rocksdb::Slice(op.value->data(), op.value->size()));
        } else {
            wb.Delete(ks);
        }
    }
    rocksdb::WriteOptions wo;
    auto s = db_->Write(wo, &wb);
    if (!s.ok()) return std::unexpected(Error{"StoreBatch", s.ToString()});
    return {};
}

// ── SML: rocksdb_compact ──
auto RocksDBBackend::compact() -> VoidResult {
    if (!db_) return std::unexpected(Error{"RocksDBCompact", "db is closed"});
    auto s = db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
    if (!s.ok()) return std::unexpected(Error{"StoreCompact", s.ToString()});
    return {};
}

// ── SML: rocksdb_foreach_scan (zero-copy visitor, cursor stays internal) ──
auto RocksDBBackend::foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx)
    -> VoidResult {
    if (!db_) return std::unexpected(Error{"RocksDBForeach", "db is closed"});
    rocksdb::ReadOptions ro;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));
    it->Seek(rocksdb::Slice(prefix.data(), prefix.size()));
    while (it->Valid()) {
        auto ks = it->key();
        if (ks.size() < prefix.size() ||
            ks.compare(rocksdb::Slice(prefix.data(), prefix.size())) < 0) break;
        // Check prefix match
        std::string_view k(ks.data(), ks.size());
        if (!k.starts_with(prefix)) break;
        std::string_view v(it->value().data(), it->value().size());
        visitor(user_ctx, k, v);
        it->Next();
    }
    if (!it->status().ok()) {
        return std::unexpected(Error{"StoreForeachScan", it->status().ToString()});
    }
    return {};
}

// ── SML: rocksdb_close ──
auto RocksDBBackend::close() -> VoidResult {
    if (db_) {
        delete db_;
        db_ = nullptr;
    }
    return {};
}

// ── SML: create_rocksdb_backend (factory) ──
auto create_rocksdb_backend(const StoreConfig& config) -> Result<BackendHandle> {
    // Ensure parent directories exist
    std::error_code ec;
    std::filesystem::create_directories(config.path, ec);
    if (ec) {
        return std::unexpected(Error{"RocksDBOpen",
            "failed to create directory '" + config.path + "': " + ec.message()});
    }

    rocksdb::Options opts;
    opts.create_if_missing = config.create_if_missing;
    if (config.enable_compression) {
        opts.compression = rocksdb::kLZ4Compression;
    }
    opts.max_open_files = config.max_open_files;
    opts.write_buffer_size = static_cast<std::size_t>(config.write_buffer_size_bytes);

    rocksdb::DB* raw_db{nullptr};
    auto s = rocksdb::DB::Open(opts, config.path, &raw_db);
    if (!s.ok()) {
        return std::unexpected(Error{"RocksDBOpen",
            "failed to open RocksDB at '" + config.path + "': " + s.ToString()});
    }

    auto* backend = new RocksDBBackend(raw_db);
    return make_backend_handle<RocksDBBackend>(backend);
}

#else // CELER_HAS_ROCKSDB == 0

auto create_rocksdb_backend(const StoreConfig& /*config*/) -> Result<BackendHandle> {
    return std::unexpected(Error{"NotAvailable", "compiled without RocksDB support"});
}

#endif

} // namespace celer
