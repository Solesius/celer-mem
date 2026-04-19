# celer-mem

[![CI](https://github.com/Solesius/celer-mem/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Solesius/celer-mem/actions/workflows/ci.yml)
[![Coverage](https://img.shields.io/endpoint?url=https%3A%2F%2Fsolesius.github.io%2Fceler-mem%2Fbadge.json)](https://solesius.github.io/celer-mem/)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg)](https://en.cppreference.com/w/cpp/23)
[![Release](https://img.shields.io/badge/release-v3.0.0-informational.svg)](https://github.com/Solesius/celer-mem/releases)

**A composite-tree storage kernel for C++23.** Mount RocksDB, SQLite, S3, and PDFs as siblings in one tree. Join across them. Materialize the result into a fourth.

```cpp
#include <celer/celer.hpp>
namespace mat = celer::materialization;

// Stream posts from SQLite, join each on user_id against RocksDB, write
// the joined view back to a RocksDB cache — one expression, no globals.
auto report = mat::materialize(
    std::move(*mat::join<std::string, std::string>(
        std::move(*mat::stream_from(mat::StoreRef<std::string>{*posts_tbl})),
        mat::StoreRef<std::string>{*users_tbl},
        [](const mat::Record<std::string>& p) -> celer::Result<std::string> {
            return p.value.substr(0, p.value.find('|'));          // user_id
        },
        mat::JoinOptions{ .kind = mat::JoinKind::Inner, .batch_size = 1024 }))
      .map([](const mat::Joined<std::string, std::string>& row) -> std::string {
          return row.left + " :: " + row.right.value_or("?");
      }),
    mat::StoreRef<std::string>{*view_tbl},
    [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.key; },
    mat::MaterializeOptions{ .mode = mat::MaterializeMode::Upsert });

// 100k × 100k rows → 275 ms join, 569 ms full pipeline.
```

## The idea

celer-mem is a new take on embedded storage for C++: treat a data store as a **composite tree** whose leaves are pluggable backends. RocksDB, SQLite, S3, and PDFs can sit side-by-side in the same tree, and the same API walks all of them. Because every leaf shares one contract, you can stream across them, join across them, and materialize the result into another leaf without leaving the tree.

It's a small idea with a lot of room inside it. Here's what falls out once you commit to it:

- **One shape for every backend.** A `StorageBackend` is a C++23 concept, so a new leaf is a struct that satisfies it — no base class, no registry.
- **Cross-store joins become natural.** If every leaf speaks the same contract, a planner can stream from one and look up in another.
- **Typed at compile time.** `constexpr` schema bindings map `scope/table` → model type, so wrong-type writes never compile.
- **Fast by default.** Dispatch goes through a `constexpr` struct-of-function-pointers vtable (~2 ns/call), and reads on the Okasaki-immutable tree are lock-free.
- **Monadic errors, RAII everything.** `Result<T>` flows end-to-end; `ResourceStack` tears down in reverse order.
- **Agent memory for free.** If chat turns, summaries, embeddings, and tool traces are all just leaves, you don't need a second system to wire them together.

celer-mem just asks a different question: *what if storage itself were a tree?*

## What's in the box

- **Backends (v3.0.0 shipped):** RocksDB, SQLite, S3 (async, streaming), QPDF (PDFs as trees)
- **Cross-store joins:** planner-driven (`BatchIndexNestedLoop`, more coming). 363k rows/s on 100k × 100k
- **Materialized views:** `Append`, `Upsert`, `Replace`, `CompareAndSwap`
- **Composite tree:** Okasaki-immutable `std::variant`, built once, lock-free reads
- **Typed schemas:** `constexpr` bindings — wrong-type writes are compile errors
- **`Result<T>` errors, RAII everything**
- **151 GoogleTest cases**, 87.8% line coverage, published to [Pages](https://solesius.github.io/celer-mem/)

## Quick start

```cpp
#include <celer/celer.hpp>

int main() {
    // Pick any backend factory; SQLite is always available.
    auto factory = celer::backends::sqlite::factory({.path = "./my.db"});
    std::vector<celer::TableDescriptor> schema{{"tasks", "today"}};

    celer::open(factory, schema);

    auto tbl = celer::db("tasks")->table("today");
    tbl->put_raw("task-1", "Ship it");
    tbl->put_raw("task-2", "Write tests");

    auto val = tbl->get_raw("task-1");   // "Ship it"
    celer::close();
}
```

Swap `sqlite::factory` for `rocksdb::factory`, `s3::factory`, or your own — the rest of the program doesn't change.

## Cross-store joins

Join a SQLite-backed table against a RocksDB-backed table, map the result, materialize into a third store — typed, planner-chosen, streaming.

```cpp
namespace mat = celer::materialization;

mat::StoreRef<std::string> posts{*posts_tbl};   // SQLite
mat::StoreRef<std::string> users{*users_tbl};   // RocksDB
mat::StoreRef<std::string> view {*view_tbl};    // RocksDB

auto src = mat::stream_from(posts);

auto joined = mat::join<std::string, std::string>(
    std::move(*src),
    users,
    [](const mat::Record<std::string>& r) -> celer::Result<std::string> {
        return r.value.substr(0, r.value.find('|'));   // user_id key extractor
    },
    mat::JoinOptions{
        .kind       = mat::JoinKind::Inner,
        .strategy   = mat::JoinStrategy::Auto,         // planner picks
        .batch_size = 1024,
    });

auto mapped = std::move(*joined).map(
    [](const mat::Joined<std::string, std::string>& row) -> std::string {
        return row.left + " :: " + row.right.value_or("?");
    });

auto report = mat::materialize(
    std::move(mapped),
    view,
    [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.key; },
    mat::MaterializeOptions{ .mode = mat::MaterializeMode::Upsert });

std::cout << report->rows_written << " rows in "
          << report->metrics.elapsed.count() / 1000.0 << " ms\n";
```

The planner picks a strategy from `BatchIndexNestedLoop`, `IndexNestedLoop`, `HashJoin`, `MergeJoin`, or `NestedLoop` based on each backend's capabilities. Accidental O(n²) plans are rejected by default.

See [`examples/06_join_bench.cpp`](examples/06_join_bench.cpp) for the full 100k × 100k benchmark and [RFC-003](finalized/RFC-003-cross-store-joins-and-materialization.md) for the design.

## Design principles

- **Composite pattern.** Hierarchical `db → table → key`. Uniform ops across leaves and subtrees.
- **Okasaki immutability.** Tree built at startup, never mutated. Reads are lock-free.
- **Concept-driven dispatch.** `StorageBackend` is a C++23 concept. No base class, no registration macro, no `std::function`.
- **`constexpr` vtable.** Manual struct-of-function-pointers type erasure. ~2 ns per call.
- **Typed schemas.** `constexpr` `fixed_string` bindings map `scope/table` → model type at compile time.
- **`Result<T>`.** Monadic errors end-to-end. No exceptions on the hot path.
- **RAII everything.** `ResourceStack` tears down in reverse construction order.

## Prerequisites

Only the toolchain and SQLite are required. Every backend is optional and auto-detected at configure time — if its headers aren't on your system, the backend is quietly dropped and the rest of the library still builds.

| Requirement | Version | Required? | Notes |
|---|---|---|---|
| GCC | ≥ 13 | yes | Or Clang ≥ 17. Must support `-std=c++23` |
| CMake | ≥ 3.25 | yes | Or GNU Make ≥ 4.0 |
| SQLite | ≥ 3.35 | yes | Usually preinstalled |
| RocksDB | ≥ 8.0 | optional | Auto-detected. Enables the RocksDB backend |
| QPDF | ≥ 10.0 | optional | Auto-detected. Enables the PDF backend |
| AWS SDK C++ | latest | optional | Fetched by CMake when `CELER_BUILD_S3=ON` |

### Optional backends

```bash
# RocksDB (skip if you don't need a KV backend)
sudo apt install librocksdb-dev          # Ubuntu / Debian
sudo dnf install rocksdb-devel           # Fedora / RHEL
brew install rocksdb                     # macOS

# QPDF (skip if you don't need the PDF backend)
sudo apt install libqpdf-dev             # Ubuntu / Debian
sudo dnf install qpdf-devel              # Fedora / RHEL
brew install qpdf                        # macOS
```

To explicitly disable a backend (even if its headers are installed):

```bash
# CMake
cmake -S . -B build -DCELER_BUILD_ROCKSDB=OFF -DCELER_BUILD_QPDF=OFF -DCELER_BUILD_S3=OFF

# Make
make CELER_NO_ROCKSDB=1 CELER_NO_QPDF=1
```

## Build from source

### Make (primary)

```bash
git clone https://github.com/Solesius/celer-mem.git
cd celer-mem

# Build the library
make

# Run the GoogleTest-backed suites
make test
make integration
make test-sqlite
make test-qpdf
make test-async
make test-all

# Generate HTML coverage locally (build/cmake/coverage/html/index.html)
make coverage

# Build example programs
make examples

# Check all headers compile independently
make check-headers
```

### CMake

```bash
# Default: build every backend whose headers are available
cmake -S . -B build -DCELER_BUILD_TESTS=ON -DCELER_BUILD_EXAMPLES=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# Minimal build — SQLite only, no RocksDB / QPDF / S3
cmake -S . -B build-min \
  -DCELER_BUILD_TESTS=ON \
  -DCELER_BUILD_ROCKSDB=OFF \
  -DCELER_BUILD_QPDF=OFF \
  -DCELER_BUILD_S3=OFF
cmake --build build-min -j

# Optional HTML coverage
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug \
  -DCELER_BUILD_TESTS=ON -DCELER_BUILD_EXAMPLES=OFF \
  -DCELER_BUILD_S3=OFF -DCELER_ENABLE_COVERAGE=ON
cmake --build build-coverage --target coverage
```

**CMake options**

| Option | Default | Effect |
|---|---|---|
| `CELER_BUILD_TESTS` | `ON` | Build GoogleTest suites |
| `CELER_BUILD_EXAMPLES` | `OFF` | Build everything under `examples/` |
| `CELER_BUILD_ROCKSDB` | `ON` (auto) | RocksDB backend. Off if headers missing |
| `CELER_BUILD_QPDF` | `ON` (auto) | QPDF backend. Off if headers missing |
| `CELER_BUILD_S3` | `ON` | AWS SDK fetched + S3 backend |
| `CELER_ENABLE_COVERAGE` | `OFF` | `--coverage` flags + `coverage` target |

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
    GIT_REPOSITORY https://github.com/Solesius/celer-mem.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(celer)

target_link_libraries(your_target PRIVATE celer)
```

### Use via find_package (after install)

```cmake
find_package(celer 3.0 REQUIRED)
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
| `05_agent_memory.cpp` | llama.cpp chat agent with persistent memory |
| `06_join_bench.cpp`   | 100k × 100k cross-store join benchmark |

Run an example after building:

```bash
make examples
./build/examples/06_join_bench
```

### LLM Agent Memory

[`05_agent_memory.cpp`](examples/05_agent_memory.cpp) is a complete chat agent backed by celer-mem and [llama.cpp](https://github.com/ggml-org/llama.cpp). Chat turns, summaries, and tool-call provenance live in RocksDB; the composite tree gives the agent typed, persistent, cross-session memory in a few lines of code. Auto-downloads TinyLlama 1.1B to demonstrate persistence on CPU — the point is the memory layer, not the model size.

```bash
# CPU — runs anywhere, good enough to show persistence
docker run --rm -v ./models:/models -p 8080:8080 \
  ghcr.io/ggml-org/llama.cpp:server \
  -m /models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf \
  --port 8080 --host 0.0.0.0

# GPU — for larger models (Llama 3, Mistral, Qwen, etc.)
docker run --rm --gpus all -v ./models:/models -p 8080:8080 \
  ghcr.io/ggml-org/llama.cpp:server-cuda \
  -m /models/your-model.gguf --port 8080 --host 0.0.0.0 --n-gpu-layers 99

# Run the agent
./build/examples/05_agent_memory --rocksdb   # persistent
./build/examples/05_agent_memory --memory    # ephemeral
```

## Project structure

```
celer-mem/
├── include/celer/
│   ├── core/            # Result, types, composite tree, dispatch, async
│   ├── backend/         # StorageBackend concept + RocksDB / SQLite / S3 / QPDF
│   ├── materialization/ # Join planner, executors, materialize modes
│   ├── schema/          # constexpr typed bindings (fixed_string NTTP)
│   ├── serde/           # Codec CPO, reflect-cpp wrappers
│   ├── api/             # Store, DbRef, TableRef, ResultSet, global API
│   └── celer.hpp        # Umbrella header
├── src/                 # Translation units
├── tests/               # 151 GoogleTest cases across 5 suites
├── examples/            # 6 programs incl. llama.cpp agent + join bench
├── finalized/           # RFC-001..003
├── Makefile
└── CMakeLists.txt
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

The repository ships with a GitHub Actions pipeline that:

- builds and runs every GoogleTest suite on pull requests and `main`
- generates an HTML coverage report with `lcov` + `genhtml`
- uploads both the HTML site (`coverage-html`) and the raw lcov tracefile
  (`coverage-lcov`) as workflow artifacts
- on pushes to `main`, deploys the coverage site to GitHub Pages at
  <https://solesius.github.io/celer-mem/>, which serves:
  - `index.html` — the browsable HTML coverage report
  - `lcov.info` — the raw lcov tracefile (for Codecov, Coveralls, local tooling)
  - `badge.json` — a [shields.io endpoint](https://shields.io/badges/endpoint-badge)
    payload powering the coverage badge in this README

The README is the project landing page; the Pages site is dedicated to the coverage report.

## RFCs

Major design decisions live in `finalized/` as versioned documents:

- [RFC-001 — celer-mem core](finalized/RFC-001-CELER-MEM.md)
- [RFC-002 — streaming S3 backend](finalized/RFC-002-STREAMING-S3.md)
- [RFC-003 — cross-store joins and materialization](finalized/RFC-003-cross-store-joins-and-materialization.md)

## Roadmap

- [x] `StorageBackend` concept
- [x] RocksDB, SQLite, S3 (async streaming), QPDF backends
- [x] Composite tree (Okasaki-immutable variant)
- [x] `constexpr` typed schemas + Slick-style `ResultSet`
- [x] Cross-store joins + materialized views (RFC-003, v3.0.0)
- [x] GoogleTest migration + published coverage site
- [ ] Hash join + spillable hash join (RFC-003 Phase 2)
- [ ] Merge join (RFC-003 Phase 3)
- [ ] Incremental materialization with watermarks
- [ ] LMDB backend
- [ ] Vector-search leaves (ANN hydration joins)
- [ ] Python / Rust bindings

## License

Copyright © 2026 Khalil Warren.

Licensed under the [Apache License, Version 2.0](LICENSE). Please retain attribution per Apache-2.0 Section 4 in any derivative works.
