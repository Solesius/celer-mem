# celer-mem

A fast, embeddable, backend-agnostic memory store for C++23. Built for agent hackers — started as something for my own agent work and it turned out really smooth.

```cpp
#include <celer/celer.hpp>

int main() {
    celer::backends::rocksdb::Config cfg{.path = "./my.db"};
    std::vector<celer::TableDescriptor> schema{{"tasks", "today"}};

    celer::open(celer::backends::rocksdb::factory(cfg), schema);

    auto tbl = celer::db("tasks")->table("today");
    tbl->put_raw("task-1", "Ship it");
    tbl->put_raw("task-2", "Write tests");

    auto val = tbl->get_raw("task-1");
    // val -> "Ship it"

    celer::close();
}
```

## What is this

celer-mem gives any C++ application a strongly-typed, RAII-safe embedded memory store. The core is a GoF Composite tree expressed as an immutable Okasaki-style `std::variant` — built once, shared lock-free across threads. The API is Slick-inspired: chainable, zero-ceremony, returns standard containers.

**RocksDB support ships out of the box.** Other backends (SQLite, LMDB, etc.) coming soon — the `StorageBackend` concept is defined from day one so adding new backends never touches core.

## Design

- **Composite pattern** — hierarchical `db → table → key` tree, uniform operations across leaves and subtrees
- **Okasaki immutability** — tree built at startup, never mutated. Reads are lock-free
- **Backend-agnostic** — `StorageBackend` concept. RocksDB v1, bring your own for anything else
- **Manual vtable type-erasure** — struct of function pointers, ~2ns per call, no `std::function` heap alloc
- **Typed schemas** — `constexpr` bindings map `scope/table` → C++ model type. Wrong-type writes are compile errors
- **Result\<T\> monadic errors** — no exceptions in the library
- **RAII everything** — `ResourceStack` tears down in reverse construction order

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| GCC | ≥ 13 | Or Clang ≥ 17. Must support `-std=c++23` |
| RocksDB | ≥ 8.0 | System install via package manager |
| Make | ≥ 4.0 | Or CMake ≥ 3.25 |

### Install RocksDB

```bash
# Ubuntu / Debian
sudo apt install librocksdb-dev

# Fedora / RHEL
sudo dnf install rocksdb-devel

# macOS
brew install rocksdb

# From source
git clone https://github.com/facebook/rocksdb.git
cd rocksdb && make shared_lib && sudo make install-shared
```

## Build from source

### Make (primary)

```bash
git clone https://github.com/yourusername/celer-mem.git
cd celer-mem

# Build the library
make

# Run unit tests (18 tests)
make test

# Run integration tests (10 E2E tests)
make integration

# Build example programs
make examples

# Check all headers compile independently
make check-headers
```

### CMake

```bash
cmake -B build -DCELER_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build
```

## Install

### System-wide (Make)

```bash
sudo make install                    # installs to /usr/local
sudo make install PREFIX=/opt/celer  # custom prefix

# Uninstall
sudo make uninstall
```

### System-wide (CMake)

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
sudo cmake --install build
```

### Use via CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    celer
    GIT_REPOSITORY https://github.com/yourusername/celer-mem.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(celer)

target_link_libraries(your_target PRIVATE celer)
```

### Use via find_package (after install)

```cmake
find_package(celer 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE celer::celer)
```

## Examples

All examples live in `examples/` and compile with `make examples`.

| File | What it demonstrates |
|---|---|
| `01_quickstart.cpp` | Open, put, get, prefix scan, close |
| `02_multi_scope.cpp` | Hierarchical scopes, cross-scope isolation |
| `03_batch_ops.cpp` | Batch writes, prefix grouping, foreach, compact |
| `04_custom_backend.cpp` | Writing a custom `StorageBackend` (in-memory) |

Run an example after building:

```bash
make examples
./build/examples/01_quickstart
```

## Project structure

```
celer-mem/
├── include/celer/
│   ├── core/           # Result, types, composite tree, dispatch
│   ├── backend/        # StorageBackend concept, RocksDB impl, dynamic loader
│   ├── schema/         # Compile-time schema bindings (fixed_string NTTP)
│   ├── serde/          # Codec trait, reflect-cpp wrappers
│   ├── api/            # Store, DbRef, TableRef, ResultSet, global API
│   └── celer.hpp       # Umbrella header
├── src/                # Translation units (5 .cpp files)
├── tests/
│   ├── main.cpp        # Unit tests (18)
│   └── integration.cpp # E2E integration tests (10)
├── examples/           # Working example programs (4)
├── Makefile            # Primary build system
└── CMakeLists.txt      # CMake alternative
```

## API Quick Reference

```cpp
// ── Global convenience API (singleton) ──
celer::open(factory, tables)       → VoidResult
celer::db("scope_name")            → Result<DbRef>
celer::close()                     → VoidResult

// ── Tree builder (instance API — backend-agnostic) ──
celer::build_tree(factory, tables) → Result<StoreNode>   // full tree
celer::build_leaf(name, handle)    → Result<StoreNode>   // single leaf
celer::build_composite(name, kids) → Result<StoreNode>   // composite

// ── DbRef (scope handle) ──
db.table("name")               → Result<TableRef>
db.scan_all("prefix")          → Result<vector<KVPair>>
db.batch(ops)                  → VoidResult

// ── TableRef (leaf handle) ──
tbl.put<T>(key, value)         → VoidResult
tbl.get<T>(key)                → Result<optional<T>>
tbl.del(key)                   → VoidResult
tbl.all<T>()                   → Result<ResultSet<T>>
tbl.prefix<T>(pfx)             → Result<ResultSet<T>>
tbl.foreach<T>(callback)       → VoidResult
tbl.compact()                  → VoidResult
tbl.put_raw(key, value)        → VoidResult
tbl.get_raw(key)               → Result<optional<string>>

// ── ResultSet<T> ──
rs.filter(pred)                → ResultSet<T>
rs.sort_by(cmp)                → ResultSet<T>
rs.take(n)                     → ResultSet<T>
rs.map<U>(fn)                  → ResultSet<U>
rs.first()                     → optional<T>
rs.collect()                   → vector<T>
rs.count()                     → size_t
rs.foreach(fn)                 → void
```

## Custom backends

The `StorageBackend` concept is the only contract. No base class, no registration macro.

```cpp
struct InMemoryBackend {
    static constexpr auto name() -> std::string_view { return "in_memory"; }

    auto get(std::string_view key) -> celer::Result<std::optional<std::string>>;
    auto put(std::string_view key, std::string_view value) -> celer::VoidResult;
    auto del(std::string_view key) -> celer::VoidResult;
    auto prefix_scan(std::string_view prefix) -> celer::Result<std::vector<celer::KVPair>>;
    auto batch(std::span<const celer::BatchOp> ops) -> celer::VoidResult;
    auto compact() -> celer::VoidResult;
    auto foreach_scan(std::string_view prefix, celer::ScanVisitor v, void* ctx) -> celer::VoidResult;
};

static_assert(celer::StorageBackend<InMemoryBackend>);

// Wrap it in a BackendFactory — same shape as backends::rocksdb::factory()
celer::BackendFactory mem_factory = [](std::string_view, std::string_view)
    -> celer::Result<celer::BackendHandle> {
    return celer::make_backend_handle(new InMemoryBackend{});
};

// Works with build_tree (instance API)
auto tree = celer::build_tree(mem_factory, schema);
celer::Store store{std::move(*tree), celer::ResourceStack{}};

// Or with the global singleton
celer::open(mem_factory, schema);
```

See `examples/04_custom_backend.cpp` for a complete working implementation.

## CI Integration

### GitHub Actions

```yaml
name: CI
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install deps
        run: sudo apt-get install -y librocksdb-dev
      - name: Build
        run: make
      - name: Unit tests
        run: make test
      - name: Integration tests
        run: make integration
      - name: Header check
        run: make check-headers
      - name: Examples
        run: make examples
```

## Roadmap

- [x] `StorageBackend` concept + RocksDB adapter
- [x] Composite tree (Okasaki-immutable variant)
- [x] Slick-style API (`all`, `filter`, `map`, `collect`, `foreach`, `batch`)
- [x] `constexpr` typed schemas
- [x] Working examples (4)
- [x] Integration test suite (10 E2E tests)
- [x] CMake install + FetchContent support
- [ ] SQLite backend
- [ ] LMDB backend
- [ ] OpenTofu/Terraform provider + schema codegen
- [ ] Memory tiering (episodic → semantic distillation)
- [ ] Python/Rust bindings

## License

MIT
