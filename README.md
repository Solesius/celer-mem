# celer-mem

A fast, embeddable, backend-agnostic memory store for C++23. Built for agent hackers — started as something for my own agent work and it turned out really smooth.

```cpp
#include <celer/celer.hpp>

celer::Store store{"./my.db", celer::rocksdb_backend{}};

store.db("tasks")
    .table("today")
    .put("task-1", Task{.title="Ship it", .status=Status::in_progress})
    .put("task-2", Task{.title="Write tests", .status=Status::not_started});

auto urgent = store.db("tasks")
    .table("today")
    .all<Task>()
    .filter([](const Task& t){ return t.status == Status::in_progress; })
    .collect();
```

## What is this

celer-mem gives any C++ application a strongly-typed, RAII-safe embedded memory store. The core is a GoF Composite tree expressed as an immutable Okasaki-style `std::variant` — built once, shared lock-free across threads. The API is Slick-inspired: chainable, zero-ceremony, returns standard containers.

**RocksDB support ships out of the box.** Other backends (SQLite, LMDB, etc.) coming soon — the `StorageBackend` concept is defined from day one so adding new backends never touches core.

## Design

- **Composite pattern** — hierarchical `db → table → key` tree, uniform operations across leaves and subtrees
- **Okasaki immutability** — tree built at startup, never mutated. Reads are lock-free. No mutex, no atomic, no RCU
- **Backend-agnostic** — `StorageBackend` concept. RocksDB v1, bring your own for anything else
- **Manual vtable type-erasure** — struct of function pointers, ~2ns per call, no `std::function` heap alloc
- **Typed schemas** — `constexpr` bindings map `scope/table` → C++ model type. Wrong-type writes are compile errors
- **reflect-cpp serde** — zero-macro struct reflection. MessagePack wire format, JSON debug mode
- **Result\<T\> monadic errors** — no exceptions in the library
- **RAII everything** — `ResourceStack` tears down in reverse construction order. Zero raw pointers in the public API

## Hack your own backend

The `StorageBackend` concept is the only contract. No base class, no registration macro. Write a struct, satisfy the concept, pass it to `celer::Store`. Done.

```cpp
struct InMemoryBackend {
    static constexpr auto name() -> std::string_view { return "in_memory"; }
    std::map<std::string, std::string, std::less<>> store_;
    std::mutex mu_;

    auto get(std::string_view key) -> celer::Result<std::optional<std::string>> { /* ... */ }
    auto put(std::string_view key, std::string_view value) -> celer::VoidResult { /* ... */ }
    auto del(std::string_view key) -> celer::VoidResult { /* ... */ }
    auto prefix_scan(std::string_view prefix) -> celer::Result<std::vector<celer::KVPair>> { /* ... */ }
    auto batch(std::span<const celer::BatchOp> ops) -> celer::VoidResult { /* ... */ }
    auto compact() -> celer::VoidResult { return celer::OkVoid(); }
    auto close() -> celer::VoidResult { store_.clear(); return celer::OkVoid(); }
};

celer::Store store{"./db", InMemoryBackend{}, my_schema};
```

Dynamic backends via C-ABI (`dlopen` a `.so`) are also supported — ship Rust, Go, or proprietary backends as plugins without recompiling celer-mem.

## Build

Requires GCC 13+ or Clang 17+ (C++23). RocksDB ≥ 8.0.

```bash
# System RocksDB
make

# Custom RocksDB path
make ROCKSDB_DIR=/opt/rocksdb

# CMake
cmake -B build && cmake --build build

# Tests
make test
```

CMake `FetchContent` pulls RocksDB if not found on system.

## Dependencies

| Dep | Version | Notes |
|-----|---------|-------|
| RocksDB | ≥ 8.0 | System install or `ROCKSDB_DIR` |
| reflect-cpp | ≥ 0.16 | Header-only, auto struct reflection + msgpack/JSON |
| GoogleTest | ≥ 1.14 | Dev only |
| Google Benchmark | ≥ 1.8 | Dev only |

## Roadmap

- [x] `StorageBackend` concept + RocksDB adapter
- [x] Composite tree (Okasaki-immutable variant)
- [x] Slick-style API (`all`, `filter`, `map`, `collect`, `foreach`, `batch`)
- [x] `constexpr` typed schemas
- [x] reflect-cpp auto-serde (msgpack + JSON debug)
- [x] C-ABI dynamic backend adapter
- [ ] SQLite backend
- [ ] LMDB backend
- [ ] OpenTofu/Terraform provider + schema codegen
- [ ] Memory tiering (episodic → semantic distillation)
- [ ] Python/Rust bindings

## License

MIT
