# RFC 001: celer-mem — A Composite-Pattern Embedded Memory Framework for C++23

## Status: Draft
## Author: Khalil Warren (🦎 Sal)
## Date: 2026-04-11

---

## 1. Abstract

**celer-mem** is a single-library, header-plus-implementation C++23 framework that gives any application a strongly-typed, RAII-safe, backend-agnostic embedded memory store. The core data structure is a GoF Composite tree expressed as an immutable Okasaki-style `std::variant<ColumnLeaf, CompositeNode>`, constructed once and shared lock-free across threads.

The consumer-facing API is modeled after Slick (Scala's functional relational mapping): expressive, chainable, zero-ceremony.

```cpp
#include <celer/celer.hpp>

// Instance-based (primary API — Soren §3.1)
celer::Store store{"./my.db", celer::rocksdb_backend{}};

store.db("tasks")
    .table("today")
    .put("task-1", Task{.title="Ship RFC", .status=Status::in_progress})
    .put("task-2", Task{.title="Write tests", .status=Status::not_started});

auto urgent = store.db("tasks")
    .table("today")
    .all<Task>()
    .filter([](const Task& t){ return t.status == Status::in_progress; })
    .collect();
// urgent : std::vector<Task>

// Global convenience (delegates to a default Store instance)
// celer::open("./my.db", celer::rocksdb_backend{});
// celer::db("tasks").table("today").all<Task>().collect();
```

**v1 ships with RocksDB as the sole backend.** The backend concept is defined from day one so SQLite, LMDB, SQL Server, and custom backends can be added without touching core.

---

## 2. Problem Statement

### 2.1 No Good C++ Embedded Memory Library Exists

The landscape:

| Library | Language | Issue for C++ Agent/App Devs |
|---------|----------|------------------------------|
| LangChain Memory | Python | Wrong language, LLM-coupled |
| Mem0 | Python | SaaS-first, no embedded mode |
| Redis | C | External daemon, not embedded |
| SQLite | C | No composite tree, no typed schema, C API |
| RocksDB | C++ | Raw KV — no schema, no tree, no API sugar |
| LevelDB | C++ | Subset of RocksDB, same raw-KV problem |

Developers building C++ agents (Corvus, llama.cpp tool-use, custom inference servers) are stuck hand-rolling persistence every time. The pattern is always the same: open a DB, partition by column family or table, route keys, manage handles, serialize/deserialize. **celer-mem extracts this into a library.**

### 2.2 The Composite Pattern Is the Right Abstraction

Agent memory is inherently hierarchical:

```
root
├── global
│   ├── foundation     (model: Foundation)
│   ├── global_memory  (model: MemoryEntry)
│   └── global_meta    (model: GlobalMeta)
├── project
│   ├── memory         (model: MemoryEntry)
│   ├── tasks          (model: TodoList)
│   ├── reflections    (model: Reflections)
│   └── session        (model: SessionEntry)
└── analytics
    ├── spans          (model: TraceSpan)
    ├── runs           (model: AnalyticsRun)
    └── tool_traces    (model: ToolTrace)
```

Every operation — get, put, scan, batch — applies uniformly whether the target is a single leaf or an entire subtree (fan-out scan). GoF Composite (§16.3) + algebraic variant = closed dispatch, no vtable, no heap indirection on the hot path.

### 2.3 Why Immutable (Okasaki)

The tree is built once at startup from a schema declaration and **never mutated**. Reads are lock-free. The only mutable state is inside the backend (RocksDB's own LSM). This gives us:

- Free thread-safety for tree traversal (no mutex, no atomic, no RCU)
- Structural sharing if the tree is ever rebuilt (hot-reload schema)
- Deterministic destruction (RAII tear-down in reverse construction order)

---

## 3. Design Principles

| # | Principle | Implication |
|---|-----------|-------------|
| 1 | **No free lunches** | Every abstraction must justify itself in fewer lines of user code or fewer bugs |
| 2 | **RAII or nothing** | Zero raw pointers in the public API. `unique_ptr`, `shared_ptr`, RAII guards only |
| 3 | **Strongly typed** | `constexpr` schema binds table → C++ model type at compile time. Wrong-type writes are compile errors |
| 4 | **Backend-agnostic** | `StorageBackend` concept defined in v1; RocksDB adapter ships first |
| 5 | **Functional API surface** | Slick-inspired: `.all<T>()`, `.filter()`, `.map()`, `.first()`, `.collect()` — returns standard containers |
| 6 | **Zero-config happy path** | `celer::open(path, backend)` + schema = running. No XML, no YAML, no daemon |
| 7 | **Makefile + CMake** | Community build. No Bazel, no custom generator. FetchContent for RocksDB if not installed |
| 8 | **IaC schema management** | OpenTofu/Terraform provider generates the C++ `constexpr` schema from HCL or validates HCL against it |
| 9 | **C-style OOP + Okasaki immutability** | Structs + free functions + variants. No inheritance hierarchies. No virtual. |
| 10 | **Lazy nothing, eager everything** | Scans materialize to `std::vector` immediately. No lazy iterators that hold DB cursors open across call boundaries |

---

## 4. Architecture

### 4.1 Layer Cake

```
┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│   api/       │  │   schema/    │  │   core/      │  │   serde/     │
│              │  │              │  │              │  │              │
│ Store        │  │ Binding<>    │  │ StoreNode    │  │ Codec<T>     │
│ DbRef        │  │ Schema<>     │  │ ColumnLeaf   │  │ reflect-cpp  │
│ TableRef     │  │ fixed_string │  │ CompositeNode│  │ encode/decode│
│ ResultSet<T> │  │ resolve<>    │  │ dispatch()   │  │              │
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │                 │
       │    uses         │   validates     │   routes        │  serializes
       ▼                 ▼                 ▼                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│                        backend/concept.hpp                           │
│           concept StorageBackend { get, put, del, scan, ... }        │
│                                                                      │
│  BackendVTable (manual fn-ptr vtable)    BackendHandle (RAII erased) │
└────────┬─────────────┬─────────────┬─────────────┬──────────────────┘
         │             │             │             │
         ▼             ▼             ▼             ▼
   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
   │ rocksdb  │  │  sqlite  │  │   lmdb   │  │  c_abi   │
   │ .hpp     │  │  .hpp    │  │  .hpp    │  │  .h      │
   │  (v1)    │  │  (v2)    │  │  (v2)    │  │ dlopen() │
   └──────────┘  └──────────┘  └──────────┘  └──────────┘

   ┌──────────────┐
   │   core/      │
   │              │     Orthogonal — no layer depends "down" on this.
   │ ResourceStack│     Any component that acquires resources pushes
   │ Result<T>    │     onto the stack. Teardown is automatic.
   │ Error        │
   └──────────────┘
```

**Composition rules:**

| Module | Knows about | Does NOT know about |
|--------|-------------|---------------------|
| `api/` | `core/`, `schema/`, `serde/`, `backend/concept` | Any concrete backend |
| `schema/` | Nothing (pure compile-time types) | Everything else |
| `core/` (composite) | `backend/concept` (for `BackendHandle`) | `api/`, `schema/`, `serde/`, concrete backends |
| `core/` (resources) | Nothing (standalone RAII) | Everything else |
| `serde/` | reflect-cpp | `core/`, `api/`, `backend/` |
| `backend/rocksdb` | `backend/concept`, RocksDB headers | `api/`, `schema/`, `serde/`, `core/composite` |
| `backend/c_abi` | C standard library only | All C++ modules |

Each module is independently compilable and testable. You can use `core/` without `api/`, `schema/` without `serde/`, `backend/rocksdb` without `schema/`. The `Store` class in `api/` is the **composer** — it wires the orthogonal pieces together at construction time, but none of them import each other horizontally.

### 4.2 The Algebraic Core

```cpp
namespace celer::core {

// ── Manual vtable for backend type-erasure (Soren §2.2, §3.4) ──
// A struct of function pointers — faster than std::function,
// no heap allocation, one indirect jump per call (~2ns).
// The BackendHandle owns the erased backend via a void* + destroy fn.

struct BackendVTable {
    auto (*get)(void* ctx, std::string_view key)
        -> Result<std::optional<std::string>>;
    auto (*put)(void* ctx, std::string_view key, std::string_view value)
        -> VoidResult;
    auto (*del)(void* ctx, std::string_view key)
        -> VoidResult;
    auto (*prefix_scan)(void* ctx, std::string_view prefix)
        -> Result<std::vector<KVPair>>;
    auto (*batch)(void* ctx, std::span<const BatchOp> ops)
        -> VoidResult;
    auto (*compact)(void* ctx)
        -> VoidResult;
    auto (*foreach_scan)(void* ctx, std::string_view prefix,
                         void (*visitor)(void* user, std::string_view k, std::string_view v),
                         void* user_ctx)
        -> VoidResult;  // zero-copy visitor, cursor stays internal
    void (*destroy)(void* ctx);
};

// Construct a vtable from any type satisfying StorageBackend concept
template <StorageBackend B>
constexpr BackendVTable make_vtable() {
    return BackendVTable{
        .get         = [](void* c, std::string_view k) { return static_cast<B*>(c)->get(k); },
        .put         = [](void* c, std::string_view k, std::string_view v) { return static_cast<B*>(c)->put(k, v); },
        .del         = [](void* c, std::string_view k) { return static_cast<B*>(c)->del(k); },
        .prefix_scan = [](void* c, std::string_view p) { return static_cast<B*>(c)->prefix_scan(p); },
        .batch       = [](void* c, std::span<const BatchOp> ops) { return static_cast<B*>(c)->batch(ops); },
        .compact     = [](void* c) { return static_cast<B*>(c)->compact(); },
        .foreach_scan = [](void* c, std::string_view p,
                           void (*vis)(void*, std::string_view, std::string_view),
                           void* u) {
            // Default: materializes via prefix_scan + callback.
            // Backends can override for true zero-copy.
            auto res = static_cast<B*>(c)->prefix_scan(p);
            if (!res.ok()) return Err("ForeachScan", res.error().message);
            for (const auto& kv : res.value()) vis(u, kv.key, kv.value);
            return OkVoid();
        },
        .destroy     = [](void* c) { delete static_cast<B*>(c); },
    };
}

// RAII handle: owns the erased backend, dispatches through vtable
struct BackendHandle {
    void*                ctx_   = nullptr;
    const BackendVTable* vtable_ = nullptr;

    template <StorageBackend B>
    explicit BackendHandle(B&& backend)
        : ctx_(new std::remove_cvref_t<B>(std::forward<B>(backend)))
        , vtable_(&vtable_for<std::remove_cvref_t<B>>)
    {}

    ~BackendHandle() { if (ctx_) vtable_->destroy(ctx_); }

    // Move-only
    BackendHandle(BackendHandle&& o) noexcept
        : ctx_(std::exchange(o.ctx_, nullptr)), vtable_(o.vtable_) {}
    BackendHandle& operator=(BackendHandle&& o) noexcept {
        if (this != &o) { if (ctx_) vtable_->destroy(ctx_);
            ctx_ = std::exchange(o.ctx_, nullptr); vtable_ = o.vtable_; }
        return *this;
    }
    BackendHandle(const BackendHandle&) = delete;
    BackendHandle& operator=(const BackendHandle&) = delete;

    // Dispatch
    auto get(std::string_view k) const { return vtable_->get(ctx_, k); }
    auto put(std::string_view k, std::string_view v) const { return vtable_->put(ctx_, k, v); }
    auto del(std::string_view k) const { return vtable_->del(ctx_, k); }
    auto prefix_scan(std::string_view p) const { return vtable_->prefix_scan(ctx_, p); }
    auto batch(std::span<const BatchOp> ops) const { return vtable_->batch(ctx_, ops); }
    auto compact() const { return vtable_->compact(ctx_); }

private:
    template <StorageBackend B>
    static constexpr BackendVTable vtable_for = make_vtable<B>();
};

// ── The two node types ──
struct ColumnLeaf;
struct CompositeNode;
using StoreNode = std::variant<ColumnLeaf, CompositeNode>;

struct ColumnLeaf {
    std::string   name;     // e.g. "tasks", "spans"
    BackendHandle handle;   // type-erased, RAII-managed — no raw pointers (Soren §2.3)
};

struct CompositeNode {
    std::string name;           // e.g. "root", "global", "project"
    std::vector<StoreNode> children;
    // O(1) child lookup — built at construction, never mutated
    std::unordered_map<std::string, std::size_t> index;
};

} // namespace celer::core
```

**Invariant:** After `Store` construction, the `StoreNode` tree is immutable. The tree does not own backend resources — the `ResourceStack` inside `Store` does. Tree nodes hold `BackendHandle` views that are valid for the lifetime of the `Store`. All mutations go through the backend handles, not the tree structure.

### 4.3 The Backend Concept

```cpp
namespace celer {

template <typename B>
concept StorageBackend = requires(B b,
    std::string_view key, std::string_view value,
    std::string_view prefix, std::span<const BatchOp> ops)
{
    // Identity
    { B::name() } -> std::convertible_to<std::string_view>;

    // Core CRUD
    { b.get(key) }       -> std::same_as<Result<std::optional<std::string>>>;
    { b.put(key, value) } -> std::same_as<VoidResult>;
    { b.del(key) }        -> std::same_as<VoidResult>;

    // Scan
    { b.prefix_scan(prefix) } -> std::same_as<Result<std::vector<KVPair>>>;

    // Batch (atomic multi-op)
    { b.batch(ops) } -> std::same_as<VoidResult>;

    // Lifecycle
    { b.compact() } -> std::same_as<VoidResult>;
    { b.close() }   -> std::same_as<VoidResult>;
};

} // namespace celer
```

The v1 RocksDB adapter satisfies this concept. Future backends (SQLite, LMDB, SQL Server ODBC) implement the same concept — the composite layer and consumer API are completely backend-blind.

### 4.3.1 Writing Your Own Backend (Hacker's Guide)

The `StorageBackend` concept is the **only** contract. No base class, no registration macro, no plugin system. You write a struct, satisfy the concept, pass it to `celer::open()`. Done.

**Minimal custom backend — in-memory hash map (< 60 lines):**

```cpp
#include <celer/celer.hpp>
#include <mutex>
#include <map>

struct InMemoryBackend {
    // The concept requires a static name
    static constexpr auto name() -> std::string_view { return "in_memory"; }

    // Internal state — yours to manage however you want
    std::map<std::string, std::string, std::less<>> store_;
    std::mutex mu_;

    // ── Satisfy the concept ──

    auto get(std::string_view key) -> celer::Result<std::optional<std::string>> {
        std::lock_guard lk{mu_};
        if (auto it = store_.find(key); it != store_.end())
            return celer::Ok(std::optional{it->second});
        return celer::Ok(std::optional<std::string>{});
    }

    auto put(std::string_view key, std::string_view value) -> celer::VoidResult {
        std::lock_guard lk{mu_};
        store_.insert_or_assign(std::string{key}, std::string{value});
        return celer::OkVoid();
    }

    auto del(std::string_view key) -> celer::VoidResult {
        std::lock_guard lk{mu_};
        store_.erase(store_.find(key));
        return celer::OkVoid();
    }

    auto prefix_scan(std::string_view prefix) -> celer::Result<std::vector<celer::KVPair>> {
        std::lock_guard lk{mu_};
        std::vector<celer::KVPair> out;
        for (auto it = store_.lower_bound(prefix); it != store_.end(); ++it) {
            if (!it->first.starts_with(prefix)) break;
            out.push_back({it->first, it->second});
        }
        return celer::Ok(std::move(out));
    }

    auto batch(std::span<const celer::BatchOp> ops) -> celer::VoidResult {
        std::lock_guard lk{mu_};
        for (const auto& op : ops) {
            if (op.kind == celer::BatchOpKind::put && op.value)
                store_.insert_or_assign(op.key, *op.value);
            else if (op.kind == celer::BatchOpKind::del)
                store_.erase(store_.find(op.key));
        }
        return celer::OkVoid();
    }

    auto compact() -> celer::VoidResult { return celer::OkVoid(); }  // no-op
    auto close()   -> celer::VoidResult { store_.clear(); return celer::OkVoid(); }
};

// Now use it exactly like RocksDB:
int main() {
    celer::open("./ignored_for_memory", InMemoryBackend{}, my_schema);
    celer::db("project").table("tasks").put("t1", Task{...});
    auto all = celer::db("project").table("tasks").all<Task>().collect();
}
```

**Key design decisions for extensibility:**

| Decision | Why |
|----------|-----|
| **Concept, not base class** | No vtable inheritance tax. Your backend is a plain struct. The compiler checks the contract at `celer::open()` instantiation — if you're missing a method, you get a clear concept-failure error, not a linker bomb |
| **No registration/plugin system** | No `REGISTER_BACKEND("my_backend", MyBackend)` macros. No shared library loading. Just template instantiation. If you can `#include` your backend header, it works |
| **`std::string` in, `std::string` out** | The concept traffics in `std::string` / `std::string_view` for keys and values. Serde is handled above the backend layer. Your backend never needs to know about MessagePack, Task structs, or schemas |
| **You own your state** | The concept says nothing about your internal data structures. Use a `std::map`, a flat file, a network socket to Postgres, a shared memory segment — celer doesn't care. Just satisfy the 7 methods |
| **Backend gets one handle per leaf** | Each `ColumnLeaf` in the tree holds its own backend handle. If your backend needs partitioning (like RocksDB column families), you handle that in your constructor. If it doesn't (like the in-memory map above), each handle is independent |
| **Thread safety is your call** | The composite tree is immutable and safe. Backend calls can come from any thread. Whether you lock internally (like the mutex above) or rely on your backend's native thread safety (like RocksDB) is your decision |

**Backend ideas the community might build:**

- `SqliteBackend` — single-file, portable, human-readable via `sqlite3` CLI
- `LmdbBackend` — mmap-based, zero-copy reads, insane read throughput
- `S3Backend` — remote KV for cloud-native agents (high latency, but works)
- `SharedMemBackend` — IPC between processes via `shm_open`
- `FlatFileBackend` — one JSON file per key, git-diffable (prototyping)
- `RedisBackend` — network-attached, shared across agent instances
- `NullBackend` — `/dev/null` for benchmarking the framework overhead

### 4.3.2 C-ABI Dynamic Backend Adapter (Soren §4)

For backends that can't be compiled into the same translation unit (shared library plugins, FFI from other languages), celer exposes a C-ABI adapter. This lets users `dlopen()` a `.so` / `.dll` containing a backend implementation without recompiling celer-mem.

```c
// celer/backend/c_abi.h — C-linkage adapter (stable ABI)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct celer_backend_t celer_backend_t;

typedef struct {
    const char* data;
    size_t      len;
} celer_slice_t;

typedef struct {
    int         ok;          // 1 = success, 0 = error
    const char* error_msg;   // null if ok
} celer_status_t;

typedef struct {
    celer_slice_t key;
    celer_slice_t value;
} celer_kv_t;

// The vtable a dynamic backend must export
typedef struct {
    const char*     (*name)(void);
    celer_status_t  (*get)(celer_backend_t* ctx, celer_slice_t key,
                           celer_slice_t* out_value);
    celer_status_t  (*put)(celer_backend_t* ctx, celer_slice_t key,
                           celer_slice_t value);
    celer_status_t  (*del)(celer_backend_t* ctx, celer_slice_t key);
    celer_status_t  (*prefix_scan)(celer_backend_t* ctx, celer_slice_t prefix,
                                   void (*visitor)(void* user, celer_kv_t kv),
                                   void* user_ctx);
    celer_status_t  (*batch)(celer_backend_t* ctx, const celer_kv_t* ops,
                             size_t n_ops);
    celer_status_t  (*compact)(celer_backend_t* ctx);
    void            (*destroy)(celer_backend_t* ctx);
} celer_backend_vtable_t;

// Every dynamic backend .so must export this symbol:
//   celer_backend_vtable_t* celer_create_backend(const char* config_json);

#ifdef __cplusplus
}
#endif
```

**celer wraps this into a `StorageBackend`-satisfying C++ struct automatically:**

```cpp
// Load a dynamic backend at runtime
auto backend = celer::load_dynamic_backend("./libmy_backend.so", R"({"path":"/tmp/data"})");
celer::Store store{"./db", std::move(backend), my_schema};
```

This enables use cases like:
- Rust backends compiled as `.so` with `extern "C"` exports
- Go backends via cgo
- Python backends via cffi (for prototyping, not production)
- Proprietary backends shipped as closed-source plugins

### 4.4 RAII: The ResourceStack

```cpp
namespace celer::core {

// Linear, move-only resource accumulator (Soren §2.3).
// Maintains a vector of type-erased resources with destroy functions.
// Uses raw function pointers — no std::function allocation.
// Destructor tears down in reverse order: last opened = first closed.
// If construction fails partway, already-pushed resources are released immediately.

class ResourceStack {
    struct Entry {
        void* resource;
        void (*destroy)(void*);   // raw fn ptr, no std::function
        std::string label;        // debug/diagnostics only
    };
    std::vector<Entry> stack_;

public:
    // Push a resource with its cleanup function pointer
    template <typename T>
    void push(std::string label, T* resource, void (*destroy)(void*)) {
        stack_.push_back({static_cast<void*>(resource), destroy, std::move(label)});
    }

    // Convenience: push with a stateless lambda (decays to fn ptr)
    template <typename T, typename D>
        requires std::is_convertible_v<D, void(*)(void*)>
    void push(std::string label, T* resource, D deleter) {
        stack_.push_back({static_cast<void*>(resource),
                          static_cast<void(*)(void*)>(deleter),
                          std::move(label)});
    }

    // RAII destructor — closes in reverse order
    ~ResourceStack() {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it)
            it->destroy(it->resource);
    }

    // Move-only (linear type — Soren §2.3)
    ResourceStack(ResourceStack&&) noexcept = default;
    ResourceStack& operator=(ResourceStack&&) noexcept = default;
    ResourceStack(const ResourceStack&) = delete;
    ResourceStack& operator=(const ResourceStack&) = delete;
};

} // namespace celer::core
```

### 4.5 Typed Schema (constexpr Bindings)

```cpp
namespace celer::schema {

// Compile-time string literal for NTTP
template <std::size_t N>
struct fixed_string {
    char data[N]{};
    constexpr fixed_string(const char (&str)[N]) {
        std::copy_n(str, N, data);
    }
    constexpr operator std::string_view() const { return {data, N - 1}; }
};

// A single binding: scope + table → C++ model type
template <fixed_string Scope, fixed_string Table, typename Model>
struct Binding {
    static constexpr auto scope = Scope;
    static constexpr auto table = Table;
    using model_type = Model;
};

// The schema is a type-list
template <typename... Bindings>
struct Schema {
    static constexpr std::size_t size = sizeof...(Bindings);
};

// Example user schema:
using MySchema = Schema<
    Binding<"global",    "foundation",    Foundation>,
    Binding<"global",    "global_memory", MemoryEntry>,
    Binding<"project",   "tasks",         TodoList>,
    Binding<"project",   "memory",        MemoryEntry>,
    Binding<"analytics", "spans",         TraceSpan>
>;

// Compile-time lookup — fails at compile time if binding doesn't exist
template <fixed_string Scope, fixed_string Table, typename SchemaT>
using resolve_model = /* ... SFINAE/concepts magic ... */;

} // namespace celer::schema
```

**Wrong-type writes are compile errors:**

```cpp
// Given: Binding<"project", "tasks", TodoList>
celer::db("project").table("tasks").put("k", MemoryEntry{...});
// ^^^ COMPILE ERROR: MemoryEntry is not TodoList
```

### 4.6 The Slick-Style Consumer API

This is the **public surface**. Everything above is hidden behind this.

```cpp
namespace celer {

// ── The Store (instance-based, Soren §3.1) ──
class Store {
    core::ResourceStack resources_;   // RAII, reverse-order teardown
    core::StoreNode     root_;        // immutable after construction

public:
    // Constructor opens the backend, builds the tree, validates schema
    Store(std::string_view path, auto backend, auto schema);
    ~Store();  // RAII cleanup via ResourceStack

    // Non-copyable, movable
    Store(Store&&) noexcept = default;
    Store& operator=(Store&&) noexcept = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // ── Scope access ──
    auto db(std::string_view scope) -> DbRef;

    // ── Template NTTP access (compile-time schema-checked, Soren §3.8) ──
    template <fixed_string Scope>
    auto db() -> TypedDbRef<Scope>;
};

// ── Global convenience (thin wrapper around a static Store instance) ──
void open(std::string_view path, auto backend, auto schema);
void close();
auto db(std::string_view scope) -> DbRef;

// ── DbRef: scope-level handle ──
class DbRef {
public:
    auto table(std::string_view name) -> TableRef;

    // Template NTTP variant (Soren §3.8)
    template <fixed_string Table>
    auto table() -> TypedTableRef<Table>;

    // Fan-out scan across all tables in this scope
    auto scan_all(std::string_view prefix) -> std::vector<KVPair>;

    // Atomic batch across multiple tables in this scope
    auto batch(std::span<const BatchOp> ops) -> VoidResult;
};

// ── TableRef: table-level handle (the main workhorse) ──
class TableRef {
public:
    // ── Single-key ops ──
    template <typename T>
    auto get(std::string_view key) -> std::optional<T>;

    template <typename T>
    auto put(std::string_view key, const T& value) -> TableRef&;  // chainable

    auto del(std::string_view key) -> TableRef&;  // chainable

    // ── Bulk retrieval → std::vector (eager, no dangling cursors) ──
    template <typename T>
    auto all() -> ResultSet<T>;

    template <typename T>
    auto prefix(std::string_view pfx) -> ResultSet<T>;

    // ── Zero-copy scan (Soren §2.4 — large-table safe) ──
    // Callback receives each deserialized record without materializing the full set.
    // DB cursor is opened and closed within this call — never escapes.
    template <typename T, typename F>
        requires std::invocable<F, const T&>
    auto foreach(F&& callback) -> VoidResult;

    template <typename T, typename F>
        requires std::invocable<F, const T&>
    auto foreach_prefix(std::string_view pfx, F&& callback) -> VoidResult;

    // ── Compaction ──
    auto compact() -> VoidResult;
};

// ── ResultSet<T>: chainable query result ──
// This is NOT lazy. On construction it materializes the full scan
// into a std::vector<T>. The chain methods are std:: algorithm sugar.
// For large tables, use TableRef::foreach() instead (Soren §2.4).
template <typename T>
class ResultSet {
    std::vector<T> data_;

public:
    // Filter in-place
    auto filter(std::predicate<const T&> auto&& pred) -> ResultSet<T>&;

    // Transform to new type
    template <typename F>
    auto map(F&& fn) -> ResultSet<std::invoke_result_t<F, const T&>>;

    // First match
    auto first() -> std::optional<T>;

    // Materialize to vector (terminal)
    auto collect() -> std::vector<T>;

    // Count
    auto count() -> std::size_t;

    // Sort
    auto sort_by(auto&& comparator) -> ResultSet<T>&;

    // Take N
    auto take(std::size_t n) -> ResultSet<T>&;

    // Zero-copy visitor over materialized data
    template <typename F>
        requires std::invocable<F, const T&>
    auto foreach(F&& callback) -> ResultSet<T>&;

    // Standard iteration
    auto begin() -> typename std::vector<T>::iterator;
    auto end()   -> typename std::vector<T>::iterator;
};

} // namespace celer
```

**Full usage example:**

```cpp
#include <celer/celer.hpp>

struct Task {
    std::string id;
    std::string title;
    int priority;
    bool done;
    // No macro needed — reflect-cpp auto-discovers aggregate fields (Soren §3.3)
};

int main() {
    // 1. Instance-based — open with RocksDB backend and typed schema (Soren §3.1)
    celer::Store store{"./my_agent.db", celer::rocksdb_backend{},
                       celer::schema::Schema<
                           celer::schema::Binding<"project", "tasks", Task>
                       >{}};

    // 2. Write (chainable)
    store.db("project")
        .table("tasks")
        .put("t1", Task{"t1", "Ship RFC",    1, false})
        .put("t2", Task{"t2", "Write tests", 2, false})
        .put("t3", Task{"t3", "Fix bug",     1, true});

    // 3. Query — Slick-style (eager materialization)
    auto hot = store.db("project")
        .table("tasks")
        .all<Task>()
        .filter([](const Task& t){ return !t.done && t.priority == 1; })
        .sort_by([](const Task& a, const Task& b){ return a.title < b.title; })
        .collect();
    // hot: std::vector<Task> = [{t1, "Ship RFC", 1, false}]

    // 4. Large-table safe: foreach visitor (Soren §2.4)
    //    No materialization — cursor lives and dies inside the call
    store.db("project")
        .table("tasks")
        .foreach<Task>([](const Task& t){
            if (!t.done) fmt::println("TODO: {}", t.title);
        });

    // 5. Template NTTP — compile-time schema validation (Soren §3.8)
    auto count = store.db<"project">()
        .table<"tasks">()
        .all<Task>()
        .filter([](const Task& t){ return t.done; })
        .count();

    // 6. Atomic batch
    store.db("project").batch({
        {celer::BatchOpKind::put, "tasks", "t4", celer::encode(Task{"t4", "Deploy", 0, false})},
        {celer::BatchOpKind::del, "tasks", "t3", {}},
    });

    // 7. Store destructs here — ResourceStack tears down in reverse order
}

    // 4. Atomic batch
    celer::db("project").batch({
        {celer::BatchOpKind::put, "tasks", "t4", serialize(Task{"t4", "Deploy", 0, false})},
        {celer::BatchOpKind::del, "tasks", "t3", {}},
    });

    // 5. Close (also happens on scope exit via RAII)
    celer::close();
}
```

---

## 5. Serialization

### 5.1 Default: reflect-cpp Automatic Reflection (Soren §3.3)

All model types are serialized via [reflect-cpp](https://github.com/getml/reflect-cpp) — a C++20 header-only library that provides compile-time struct reflection without macros. MessagePack is the wire format.

```cpp
#include <rfl/msgpack.hpp>

struct Task {
    std::string id;
    std::string title;
    int priority;
    bool done;
};
// That's it. No macros. reflect-cpp discovers fields via structured bindings.
// celer::Codec<Task>::encode/decode "just works" for any aggregate type.
```

**Why reflect-cpp over macros:**

| | `CELER_FIELDS` macro | reflect-cpp |
|---|---|---|
| User ceremony | Must list every field | Zero — aggregate types auto-discovered |
| Maintenance | New field = update macro call | New field = just add the member |
| Nested structs | Manual recursive macro | Automatic recursive reflection |
| Compiler support | Any C++17 | GCC 13+, Clang 17+, MSVC 19.38+ (same as our C++23 floor) |
| Dependency | None (internal macro) | Header-only, ~50KB, FetchContent-friendly |

reflect-cpp supports msgpack, JSON, CBOR, flexbuffers, and TOML out of the box. We use msgpack as the default wire format and JSON as an opt-in debug format:

```cpp
// Normal (binary, fast):
auto bytes = celer::encode<Task>(task);       // msgpack
auto task  = celer::decode<Task>(bytes);

// Debug (human-readable, slower):
auto json  = celer::encode_json<Task>(task);  // for logging/inspection
```

### 5.2 Custom Serde (Escape Hatch)

Users can still specialize `celer::Codec<T>` for types that don't play well with reflection (C unions, bitfields, legacy structs):

```cpp
template <>
struct celer::Codec<MyType> {
    static auto encode(const MyType& v) -> std::string;
    static auto decode(std::string_view bytes) -> MyType;
};
```

The custom codec takes priority over reflect-cpp auto-discovery.

---

## 6. Infrastructure-as-Code: OpenTofu/Terraform Provider

### 6.1 Motivation

For production deployments, the DB schema should be version-controlled, diffable, and plan/apply-able — not buried in C++ template parameters.

### 6.2 HCL Schema Definition

```hcl
# celer_schema.tf

resource "celer_database" "agent_store" {
  path    = "./agent.db"
  backend = "rocksdb"

  scope "global" {
    table "foundation" {
      model = "Foundation"
      compression = "lz4"
    }
    table "global_memory" {
      model = "MemoryEntry"
    }
  }

  scope "project" {
    table "tasks" {
      model     = "TodoList"
      max_concurrent = 2
    }
    table "memory" {
      model = "MemoryEntry"
    }
  }

  scope "analytics" {
    table "spans" {
      model       = "TraceSpan"
      compression = "zstd"
      ttl_seconds = 604800  # 7 days
    }
  }
}
```

### 6.3 Workflow

```
tofu plan    → Diffs current schema against desired, shows add/remove/modify tables
tofu apply   → Creates column families, configures options, writes sentinel
celer-codegen → Reads .tfstate, emits constexpr Schema<...> C++ header
```

The generated header is committed to source control and included by the application. The `constexpr` schema in code is always in sync with the deployed DB.

---

## 7. Build System

### 7.1 Makefile (Primary — Community)

```makefile
# Users with RocksDB installed system-wide:
make                          # builds libceler.a + headers

# Users pointing to a custom RocksDB path:
make ROCKSDB_DIR=/opt/rocksdb

# Run tests:
make test

# Install:
make install PREFIX=/usr/local
```

### 7.2 CMake (Secondary — Integration)

```cmake
include(FetchContent)
FetchContent_Declare(celer_mem
    GIT_REPOSITORY https://github.com/khalil-warren/celer-mem
    GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(celer_mem)

target_link_libraries(my_app PRIVATE celer::celer)
```

If RocksDB is not found on system, CMake falls back to FetchContent for it too (with a loud warning about build time).

### 7.3 File Layout

```
celer-mem/
├── include/
│   └── celer/
│       ├── celer.hpp              # public umbrella header
│       ├── core/
│       │   ├── composite.hpp      # StoreNode, ColumnLeaf, CompositeNode
│       │   ├── dispatch.hpp       # free-function visit dispatch
│       │   ├── resource_stack.hpp # RAII resource management
│       │   └── result.hpp         # Result<T>, VoidResult
│       ├── schema/
│       │   ├── binding.hpp        # constexpr Binding, Schema, fixed_string
│       │   └── codec.hpp          # CELER_FIELDS, Codec<T> specialization
│       ├── api/
│       │   ├── db_ref.hpp         # DbRef
│       │   ├── table_ref.hpp      # TableRef
│       │   └── result_set.hpp     # ResultSet<T> chainable query
│       ├── backend/
│       │   ├── concept.hpp        # StorageBackend concept
│       │   ├── rocksdb.hpp        # RocksDB adapter (v1)
│       │   ├── c_abi.h            # C-linkage dynamic backend adapter (Soren §4)
│       │   ├── dynamic.hpp        # dlopen() wrapper → StorageBackend from .so
│       │   └── template.hpp       # Copy-paste backend starter (satisfies concept, all methods stubbed)
│       └── serde/
│           ├── reflect.hpp        # reflect-cpp auto-serde (msgpack + JSON debug)
│           └── codec.hpp          # Codec<T> specialization escape hatch
├── src/
│   ├── backend/
│   │   └── rocksdb.cpp            # RocksDB adapter impl (compiled, not header-only)
│   ├── core/
│   │   └── dispatch.cpp           # composite dispatch impl
│   └── api/
│       └── celer.cpp              # open/close, global state
├── terraform/
│   ├── provider/                  # OpenTofu provider (Go)
│   └── examples/
│       └── agent_store.tf
├── tools/
│   └── celer-codegen/             # .tfstate → constexpr Schema header generator
├── tests/
│   ├── unit/
│   │   ├── test_composite.cpp
│   │   ├── test_dispatch.cpp
│   │   ├── test_schema.cpp
│   │   ├── test_result_set.cpp
│   │   ├── test_rocksdb_backend.cpp
│   │   ├── test_backend_concept.cpp  # compile-time concept conformance checks
│   │   └── test_api.cpp
│   ├── integration/
│   │   └── test_full_workflow.cpp
│   └── bench/
│       └── bench_hot_path.cpp     # must complete <50ms for 10K ops
├── examples/
│   ├── basic_usage.cpp
│   ├── agent_memory.cpp
│   ├── custom_backend.cpp         # in-memory backend, full working example
│   └── write_your_own_backend.md  # step-by-step guide (the only doc file)
├── Makefile
├── CMakeLists.txt
├── LICENSE                        # MIT
└── README.md
```

---

## 8. Error Handling

### 8.1 Result Type

```cpp
namespace celer {

template <typename T>
class Result {
    std::variant<T, Error> inner_;
public:
    bool ok() const;
    T& value();
    const Error& error() const;

    // Monadic chaining
    template <typename F>
    auto and_then(F&& fn) -> Result<std::invoke_result_t<F, T>>;

    template <typename F>
    auto map(F&& fn) -> Result<std::invoke_result_t<F, T>>;

    auto or_else(auto&& fn) -> Result<T>;
};

struct Error {
    std::string code;     // e.g. "StoreGet", "SchemaViolation"
    std::string message;
};

using VoidResult = Result<std::monostate>;

} // namespace celer
```

No exceptions in the library. All errors are `Result<T>`. Users who want exceptions can `.value()` which throws on error.

---

## 9. Thread Safety Model

| Component | Safety | Mechanism |
|-----------|--------|-----------|
| `StoreNode` tree | Thread-safe (immutable) | Okasaki — no mutation after construction |
| `ResourceStack` | Not shared | Linear move-only type, owned by `Store` instance (Soren §2.3) |
| `DbRef`, `TableRef` | Thread-safe | Stateless handles; all state lives in backend |
| `ResultSet<T>` | Not shared | Returned by value, owned by caller |
| RocksDB backend | Thread-safe | RocksDB's own internal concurrency (column families are independently lockable) |

**Rule:** The library never internally spawns threads. Concurrency is the caller's responsibility. The library is thread-*safe* but not thread-*managed*.

---

## 10. Performance Targets

| Operation | Target (10K ops, release build, NVMe) |
|-----------|---------------------------------------|
| `put` (single key, 1KB value) | < 5ms total |
| `get` (single key, warm cache) | < 1ms total |
| `all<T>()` (1K records) | < 10ms |
| `all<T>().filter().collect()` (1K records, 100 match) | < 12ms |
| `batch` (100 ops, atomic) | < 3ms |
| `prefix_scan` (1K prefix matches) | < 8ms |
| Tree construction (20 nodes) | < 1ms |

All benchmarks enforced in CI via `bench_hot_path.cpp`.

---

## 11. v1 Scope

### In Scope

- [ ] `StoreNode` composite tree (Okasaki-immutable variant)
- [ ] Free-function dispatch (`std::visit`)
- [ ] `StorageBackend` concept
- [ ] RocksDB adapter (column families, WriteBatch, prefix scan, compaction)
- [ ] `ResourceStack` RAII
- [ ] `constexpr` typed schema (`Binding`, `Schema`)
- [ ] reflect-cpp auto-serde (msgpack wire format, JSON debug) — no macros
- [ ] `BackendHandle` manual vtable type-erasure (struct of fn ptrs)
- [ ] C-ABI dynamic backend adapter (`celer/backend/c_abi.h`)
- [ ] Consumer API: `Store{...}`, `db()` / `db<>()`, `table()` / `table<>()`, `get`, `put`, `del`, `all`, `prefix`, `foreach`, `filter`, `map`, `collect`, `first`, `sort_by`, `take`, `batch`, `compact`
- [ ] Global convenience API: `celer::open()`, `celer::db()` (delegates to static Store)
- [ ] `Result<T>` monadic error type (no exceptions)
- [ ] Makefile + CMakeLists.txt
- [ ] Unit tests (composite, dispatch, schema, result_set, backend, API)
- [ ] Integration test (full open→write→query→close cycle)
- [ ] Benchmark suite (10K ops < 50ms)
- [ ] Examples (basic, agent memory, custom backend stub)
- [ ] README + API docs

### Out of Scope (v2+)

- [ ] SQLite, LMDB, SQL Server backends
- [ ] OpenTofu/Terraform provider + celer-codegen
- [ ] Algorithmic memory tiering (episodic → semantic distillation without LLM)
- [ ] Hot-reload schema (rebuild tree without restart)
- [ ] Secondary/replica DB support
- [ ] `ScanCallback` zero-copy visitor (backend-native lazy iteration, beyond `foreach`)
- [ ] Cross-DB atomic WriteBatch
- [ ] Python/Rust bindings

---

## 12. Open Questions for Red-Team

| # | Question | Soren's Verdict | Status |
|---|----------|----------------|--------|
| 1 | **Global state or instance-based?** | **Instance-based.** `celer::Store store{...}` is primary. Global `celer::open()` is convenience sugar over a static instance. | **RESOLVED** |
| 2 | **ResultSet eager vs lazy?** | **Eager by default** + `foreach()` zero-copy visitor for large tables. Never hold cursors in returned objects. | **RESOLVED** |
| 3 | **CELER_FIELDS macro vs. reflection?** | **reflect-cpp.** Zero-macro aggregate reflection. Fallback `Codec<T>` specialization for non-aggregates. | **RESOLVED** |
| 4 | **Backend type-erasure cost** | **Manual vtable** (struct of function pointers). ~2ns indirect jump, no `std::function` heap alloc. | **RESOLVED** |
| 5 | **Terraform in v1 or v2?** | v2. Codegen tool (`.tfstate` → header) is a stretch goal for v1. Full provider is v2. | **RESOLVED** |
| 6 | **Msgpack as default serde** | Msgpack default + JSON debug via reflect-cpp's built-in JSON support. | **RESOLVED** |
| 7 | **Namespace: `celer::` vs `celer_mem::`?** | `celer::` — short, clean, no real-world collision risk. | **RESOLVED** |
| 8 | **String vs. Template routing** | **Both.** `db("scope")` for dynamic, `db<"scope">()` for compile-time checked. NTTP path resolves to string path at compile time. | **RESOLVED** |

---

## 13. Dependencies

| Dependency | Version | Required? | Notes |
|------------|---------|-----------|-------|
| RocksDB | ≥ 8.0 | Yes (v1) | System install or `ROCKSDB_DIR` |
| reflect-cpp | ≥ 0.16 | Yes | Header-only, auto struct reflection + msgpack/JSON serde |
| GoogleTest | ≥ 1.14 | Dev only | FetchContent in CMake |
| Google Benchmark | ≥ 1.8 | Dev only | FetchContent in CMake |

**Compiler:** GCC 13+ or Clang 17+ (C++23: `std::expected`, concepts, NTTP, `std::format`)

**Removed dependency:** msgpack-c — reflect-cpp bundles msgpack support internally.

---

*End of RFC 001*
