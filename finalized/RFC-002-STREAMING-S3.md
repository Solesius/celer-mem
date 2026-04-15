# RFC 002: Streaming Primitive & S3 Backend for celer-mem

## Status: Draft
## Author: Khalil Warren (🦎 Sal)
## Date: 2026-04-14
## Depends: RFC-001 (celer-mem core)

---

## 1. Abstract

This RFC introduces two tightly coupled additions to celer-mem:

1. **`celer::stream` — A pull-based, Okasaki-immutable streaming primitive** inspired by fs2 (Pilquist/Chiusano), adapted for C++23 with RAII semantics, constexpr vtable type-erasure, and the GoF Prototype pattern for opaque stream cloning.

2. **`S3Backend` — An Amazon S3 storage backend** that natively leverages the streaming primitive for chunked upload/download and paginated scans, satisfying the (now-extended) `StorageBackend` concept.

The `StorageBackend` concept gains three new required methods (`stream_get`, `stream_put`, `stream_scan`). Existing backends (RocksDB, SQLite) receive default materializing stubs. The S3 backend implements native streaming from day one.

```cpp
#include <celer/celer.hpp>

// S3 backend — streaming is native
celer::backends::s3::Config s3_cfg{
    .bucket     = "my-agent-store",
    .prefix     = "prod/v1/",
    .region     = "us-west-2",
    .chunk_size = 8 * 1024 * 1024,  // 8MB multipart chunks
};
auto tree = celer::build_tree(celer::backends::s3::factory(s3_cfg), schema);
celer::Store store{std::move(*tree), celer::ResourceStack{}};

// Stream a large value without full materialization
auto stream = store.db("analytics")->table("spans")->stream_get("big-trace-001");
auto head   = celer::stream::take(std::move(*stream), 1000);
auto first_1k = celer::stream::collect(head);

// Stream a scan with pagination (S3 ListObjectsV2 under the hood)
auto kv_stream = store.db("project")->table("memory")->stream_scan("2026-04/");
celer::stream::drain(*kv_stream, [](const celer::KVPair& kv) {
    process(kv);
});
```

---

## 2. Problem Statement

### 2.1 Large Values Break the Materializing API

RFC-001's API materializes every value into `std::string` and every scan into `std::vector<KVPair>`. This works for local embedded stores (RocksDB, SQLite) where values are typically < 1MB and scans < 10K records.

S3 breaks these assumptions:

| Dimension | Local (RocksDB/SQLite) | S3 |
|-----------|------------------------|----|
| Value size | < 1MB typical | Up to 5TB per object |
| Scan result | 10K records in-memory | Millions of objects, paginated |
| Latency | < 1ms | 50-200ms per request |
| Transfer cost | Free (local disk) | $0.09/GB egress |

Materializing a 1GB S3 object into a `std::string` is not just slow — it's architecturally wrong. The value should flow through the system as a stream of chunks, never fully resident in memory.

### 2.2 No Good C++ Stream Library Exists

| Library | Issue |
|---------|-------|
| `std::istream` | Mutable, no composition, no chunking, no RAII for the pipeline |
| `std::ranges` (C++20) | Pull-based but synchronous, no effects, no resource management |
| Boost.Asio streams | Coupled to async I/O model, not value-oriented |
| RxCpp | Push-based (Observable), wrong model for I/O pull |
| folly::IOBuf | Buffer chain, not a compositional stream algebra |

fs2 (Scala) got this right: streams are immutable descriptions of effectful computations that produce chunks. We bring that model to C++23 with zero virtual dispatch (constexpr vtable), zero heap allocation on the hot path (Chunk structural sharing), and deterministic cleanup (RAII).

### 2.3 S3 Is Inherently Streaming

S3's HTTP API is naturally chunked/paginated:

- **GetObject**: HTTP chunked transfer encoding → stream of byte chunks
- **PutObject (multipart)**: upload 5MB-5GB parts → consume a byte stream
- **ListObjectsV2**: paginated, 1000 keys per page → stream of KV pages

A materializing adapter over S3 forces double-buffering: S3 streams bytes over HTTP, we buffer them into a string, the user processes the string. With native streaming, the user processes chunks as they arrive — one allocation per chunk, bounded memory.

---

## 3. Design: The Streaming Primitive

### 3.1 Core Algebra

The stream primitive has three types:

```
Chunk<T>        — Immutable, shared-ownership batch of elements
StreamHandle<T> — Type-erased, pull-based source of Chunk<T>
StreamVTable<T> — Constexpr vtable: pull + clone + destroy
```

The algebra is pull-based: the consumer drives. Each `pull()` returns the next chunk (or Done, or Error). This matches fs2's `Pull[F, O, R]` where our effect type `F` = `Result` (fallible I/O, no async).

```
pull : StreamHandle<T> → Result<Optional<Chunk<T>>>
       Done   = Optional is empty (nullopt)
       Emit   = Optional contains Chunk<T>
       Fail   = Result is unexpected(Error)
```

### 3.2 Chunk<T> — Okasaki-Immutable Batched Elements

```cpp
template <typename T>
class Chunk {
    std::shared_ptr<const std::vector<T>> storage_;
    std::size_t offset_;
    std::size_t length_;
public:
    static auto from(std::vector<T> data) -> Chunk<T>;
    static auto singleton(T value) -> Chunk<T>;

    auto slice(std::size_t off, std::size_t len) const -> Chunk<T>;  // O(1)
    auto data()  const -> const T*;
    auto size()  const -> std::size_t;
    auto begin() const -> const T*;
    auto end()   const -> const T*;
};
```

**Okasaki properties:**

| Property | Mechanism |
|----------|-----------|
| Immutability | `shared_ptr<const vector<T>>` — never mutated after construction |
| Structural sharing | `slice()` returns a new Chunk over the same storage, different offset/length |
| Thread safety | `shared_ptr` refcount is atomic; const data requires no synchronization |
| Deterministic cleanup | Last `shared_ptr` destructs the storage; no GC, no leak |

**Performance:** S3 multipart downloads naturally produce ~5MB byte chunks. Each HTTP response body becomes one `Chunk<char>`. Scanning produces `Chunk<KVPair>` pages (1000 keys per S3 page). The chunk is the unit of allocation — within a chunk, iteration is pointer arithmetic.

### 3.3 StreamVTable<T> — Constexpr Vtable (Soren §2.2 Pattern)

```cpp
template <typename T>
struct StreamVTable {
    auto (*pull_fn)(void* ctx)         -> Result<std::optional<Chunk<T>>>;
    auto (*clone_fn)(const void* ctx)  -> void*;     // Prototype pattern
    void (*destroy_fn)(void* ctx);
};
```

Same pattern as `BackendVTable` from RFC-001:
- One `static constexpr` instance per `(T, Impl)` pair
- No heap allocation for the vtable itself
- ~2ns indirect jump per call
- No `std::function`, no `std::any`, no virtual

The **clone** function pointer is the Prototype pattern (GoF §3.4): it deep-copies the concrete stream implementation without knowing its type. This enables:

- **Fan-out:** Clone a stream position, consume independently from two threads
- **Retry:** Clone before a fallible operation, retry from the clone on failure
- **Opaque extension:** S3 streams carry internal state (continuation tokens, multipart upload IDs) that clone preserves without exposing through the type

### 3.4 StreamHandle<T> — Type-Erased Pull Source

```cpp
template <typename T>
class StreamHandle {
    void*                  ctx_;
    const StreamVTable<T>* vtable_;
public:
    auto pull()  -> Result<std::optional<Chunk<T>>>;  // advance
    auto clone() const -> StreamHandle<T>;             // Prototype
    auto valid() const -> bool;

    // Move-only (linear consumption); explicit clone() for copies
    StreamHandle(StreamHandle&&) noexcept;
    StreamHandle(const StreamHandle&) = delete;
    ~StreamHandle();  // RAII: vtable_->destroy_fn(ctx_)
};
```

**Why Prototype over copy constructor:**

| | Implicit copy | Explicit `clone()` |
|-|---|---|
| Cost visibility | Hidden — looks like a value copy | Explicit — caller knows it allocates |
| Semantic clarity | "Same value" (wrong for streams) | "Independent fork of position" (correct) |
| Move-only default | Can't enforce | Natural: move is default, clone is opt-in |
| Composition safety | Accidental copies create aliased cursors | No aliasing unless caller explicitly forks |

### 3.5 make_stream_handle — Constexpr Vtable Construction

```cpp
template <typename T, typename Impl>
auto make_stream_handle(Impl* impl) -> StreamHandle<T> {
    static constexpr StreamVTable<T> vtable {
        .pull_fn    = [](void* c) { return static_cast<Impl*>(c)->pull(); },
        .clone_fn   = [](const void* c) { return new Impl(*static_cast<const Impl*>(c)); },
        .destroy_fn = [](void* c) { delete static_cast<Impl*>(c); },
    };
    return StreamHandle<T>{static_cast<void*>(impl), &vtable};
}
```

**Requirements on `Impl`:**
- `pull() -> Result<std::optional<Chunk<T>>>` — the stream contract
- Copy constructible — for Prototype `clone()`
- Destructible — for RAII cleanup

No base class, no registration, no macro. Any struct satisfying these requirements can be type-erased into a `StreamHandle<T>`.

### 3.6 Composition Combinators

Streams compose via free functions that return new `StreamHandle<T>` values. Each combinator creates a concrete wrapper type and immediately type-erases it:

```cpp
namespace celer::stream {
    // Transform each element
    auto map(StreamHandle<T>, Fn) -> StreamHandle<U>;

    // Keep elements satisfying predicate
    auto filter(StreamHandle<T>, Pred) -> StreamHandle<T>;

    // Take at most N elements (O(1) chunk slicing)
    auto take(StreamHandle<T>, size_t) -> StreamHandle<T>;

    // For each element, produce a sub-stream; concatenate all
    auto flat_map(StreamHandle<T>, Fn) -> StreamHandle<U>;
}
```

**Composition and Prototype interaction:** When `map(source, fn)` creates a `MapImpl`, the `MapImpl`'s copy constructor calls `source.clone()`. This means cloning a composed stream clones the entire pipeline — each layer independently duplicates its state. The Prototype pattern chains correctly through arbitrary composition depth.

**Allocation cost:** One heap allocation per composition step (the `new MapImpl{...}` inside `make_stream_handle`). This is acceptable because:
1. Composition happens once at pipeline setup, not per-element
2. The vtable is `constexpr` — no allocation for dispatch metadata
3. Chunks flow through the pipeline by shared_ptr (no copy per element)

### 3.7 Known Trade-off: Composition Dispatch Stacking

Each combinator type-erases its result into a new `StreamHandle<T>`. When combinators compose, the vtable indirection **stacks**:

```
map(filter(source)).pull()
  → MapImpl::pull_fn [vtable hop #1, ~2ns]
    → filter_handle.pull()
      → FilterImpl::pull_fn [vtable hop #2, ~2ns]
        → source_handle.pull()
          → S3GetStream::pull_fn [vtable hop #3, ~2ns]
```

A pipeline of depth N incurs N × ~2ns of indirect-call overhead **per pull**. This is the classic cost of "immediate type-erasure at every composition step" — each layer crosses a vtable boundary because the inner handle is already erased.

**Impact analysis:**

| Pipeline depth | Overhead/pull | Relative to S3 (~100ms) | Relative to RocksDB (~5μs) |
|----------------|---------------|-------------------------|----------------------------|
| 1 (no compose) | ~2ns  | 0.000002% | 0.04% |
| 3 (typical)    | ~6ns  | 0.000006% | 0.12% |
| 5 (deep)       | ~10ns | 0.00001%  | 0.2%  |
| 10 (extreme)   | ~20ns | 0.00002%  | 0.4%  |

For S3 the overhead is immeasurably small. For local backends pulling millions of micro-chunks it becomes a nonzero fraction of a microsecond-scale operation — still well under 1%, but worth knowing about.

**v3 optimization path: combinator fusion.** Two strategies exist:

1. **Function composition before erasure.** `map(filter(source, pred), fn)` could detect that `source` is a `FilterImpl` (peek at vtable pointer identity), extract the predicate, and fuse into a single `MapFilterImpl` that applies `pred` then `fn` in one pull — one vtable hop instead of two. This is analogous to Haskell's rewrite rules for `map f . map g = map (f . g)`.

2. **Deferred erasure / staged pipelines.** Instead of erasing at every step, build a typed pipeline descriptor (template chain) and erase only at the terminal. This gives zero intermediate vtable hops but requires the full pipeline type to be known at compile time — incompatible with `StreamHandle<T>` as the public API.

Both are v3 work. For v2, the stacking overhead is acceptable and the simpler "erase immediately" model is correct by construction.

### 3.8 Terminal Operations

```cpp
namespace celer::stream {
    auto collect(StreamHandle<T>&) -> Result<vector<T>>;       // materialize all
    auto collect_string(StreamHandle<char>&) -> Result<string>; // byte stream → string
    auto fold(StreamHandle<T>&, Acc, Fn) -> Result<Acc>;       // reduce
    auto count(StreamHandle<T>&) -> Result<size_t>;            // count elements
    auto drain(StreamHandle<T>&, Fn) -> VoidResult;            // consume, visitor
}
```

Terminal operations consume the stream (pull until Done). After a terminal, the stream handle is exhausted (subsequent pulls return Done). This is linear consumption — the handle is not reusable. Clone before a terminal if you need to re-consume.

### 3.9 Stream Constructors

```cpp
namespace celer::stream {
    auto empty<T>() -> StreamHandle<T>;               // immediately Done
    auto singleton<T>(T) -> StreamHandle<T>;           // emit one element
    auto from_vector<T>(vector<T>) -> StreamHandle<T>; // emit all as one chunk
    auto from_string(string) -> StreamHandle<char>;    // string → char stream
}
```

---

## 4. Design: StorageBackend Concept Extension

### 4.1 New Required Methods

The `StorageBackend` concept gains three streaming methods:

```cpp
template <typename B>
concept StorageBackend = requires(B b, ...) {
    // ... existing 7 methods ...

    // Streaming extensions (RFC-002)
    { b.stream_get(key) }                    -> same_as<Result<StreamHandle<char>>>;
    { b.stream_put(key, StreamHandle<char>)} -> same_as<VoidResult>;
    { b.stream_scan(prefix) }                -> same_as<Result<StreamHandle<KVPair>>>;
};
```

### 4.2 BackendVTable Extension

```cpp
struct BackendVTable {
    // ... existing 8 function pointers ...
    auto (*stream_get_fn)(void*, string_view)                    -> Result<StreamHandle<char>>;
    auto (*stream_put_fn)(void*, string_view, StreamHandle<char>) -> VoidResult;
    auto (*stream_scan_fn)(void*, string_view)                   -> Result<StreamHandle<KVPair>>;
};
```

### 4.3 Default Stubs for Existing Backends

RocksDB and SQLite get materializing stubs that satisfy the concept via delegation to existing methods:

```cpp
// stream_get: get() → wrap result in single-chunk stream
auto stream_get(string_view key) -> Result<StreamHandle<char>> {
    auto r = get(key);
    if (!r) return unexpected(r.error());
    if (!*r) return stream::empty<char>();
    return stream::from_string(std::move(**r));
}

// stream_put: collect stream → put()
auto stream_put(string_view key, StreamHandle<char> input) -> VoidResult {
    auto collected = stream::collect_string(input);
    if (!collected) return unexpected(collected.error());
    return put(key, *collected);
}

// stream_scan: prefix_scan() → wrap vector in single-chunk stream
auto stream_scan(string_view prefix) -> Result<StreamHandle<KVPair>> {
    auto r = prefix_scan(prefix);
    if (!r) return unexpected(r.error());
    return stream::from_vector(std::move(*r));
}
```

**These are intentionally suboptimal.** Future optimization: RocksDB can emit chunks from its iterator without full materialization; SQLite can use cursor-based chunked reads. Marked as future enhancement.

### 4.4 Breaking Change Policy

Adding three methods to `StorageBackend` is a **breaking change** for custom backends. This is intentional: custom backends compiled against celer-mem v2 must implement streaming. The stubs above serve as a copy-paste template for the simplest possible implementation.

---

## 5. Design: S3 Backend

### 5.1 Architecture

```
┌─────────────┐     ┌──────────────────────┐     ┌──────────────┐
│ celer API   │────▶│ S3Backend            │────▶│ AWS S3       │
│ Store/DbRef │     │                      │     │ HTTP REST    │
│ TableRef    │     │ Key prefix routing:  │     │              │
│             │     │ <pfx>/<scope>/<table>/│     │ GetObject    │
└─────────────┘     │                      │     │ PutObject    │
                    │ Native streaming:    │     │ Multipart    │
                    │ - S3GetStream (char) │     │ ListObjectsV2│
                    │ - S3ListStream (KV)  │     │ DeleteObject │
                    │ - Multipart upload   │     └──────────────┘
                    └──────────────────────┘
```

**Key routing:** `<config.prefix><scope>/<table>/<key>` — each logical key maps to one S3 object. A scope's tables share a bucket but have distinct prefixes.

### 5.2 S3GetStream — Chunked Download

```cpp
struct S3GetStream {
    shared_ptr<S3Client> client;
    string bucket, key;
    size_t chunk_size;    // typically 5-8MB
    size_t offset{0};     // ranged GET byte offset
    bool done{false};

    auto pull() -> Result<optional<Chunk<char>>> {
        // Issue: GET with Range: bytes=offset-(offset+chunk_size-1)
        // Return: chunk of received bytes
        // Done: when response < chunk_size or 416 (range not satisfiable)
    }

    // Prototype: copies offset + done state for independent resume
    S3GetStream(const S3GetStream&) = default;
};
```

**Ranged GET strategy:** Instead of streaming the HTTP response body (which requires holding the connection open), each `pull()` issues an independent ranged GET. This is:
- **Retry-safe:** If a pull fails, the next pull retries from the same offset
- **Prototype-safe:** Cloned streams can pull independently (different offsets)
- **Connection-light:** No long-lived HTTP connections needed
- **Trade-off:** One HTTP round-trip per chunk (~50ms latency per pull)

### 5.3 S3ListStream — Paginated Scan

```cpp
struct S3ListStream {
    shared_ptr<S3Client> client;
    string bucket, prefix;
    string continuation_token;
    bool done{false};

    auto pull() -> Result<optional<Chunk<KVPair>>> {
        // Issue: ListObjectsV2 with continuation token
        // Return: chunk of KVPair (key = object key minus prefix, value = object body)
        // Done: when IsTruncated == false
    }

    // Prototype: copies continuation token for independent pagination
    S3ListStream(const S3ListStream&) = default;
};
```

**Pagination:** S3 returns up to 1000 keys per page with a continuation token. Each `pull()` fetches one page. Prototype clone captures the token, allowing two consumers to independently paginate from the same point.

### 5.4 Multipart Upload (stream_put)

`stream_put` consumes a `StreamHandle<char>` via S3 multipart upload:

1. `CreateMultipartUpload` → get upload ID
2. For each `pull()` from the input stream → `UploadPart`
3. On completion → `CompleteMultipartUpload`
4. On error → `AbortMultipartUpload` (RAII safety)

Each chunk from the stream becomes one S3 part. S3 requires parts ≥ 5MB (except the last), so the stream's chunk size should be configured accordingly.

### 5.5 Configuration

```cpp
struct Config {
    string bucket;                          // S3 bucket name
    string prefix;                          // Key prefix (e.g., "celer/prod/")
    string region       = "us-east-1";
    string endpoint_url;                    // Override for MinIO/LocalStack
    size_t chunk_size   = 5 * 1024 * 1024;  // 5MB (S3 minimum part size)
    int    max_retries  = 3;
    int    timeout_ms   = 30000;
    bool   use_path_style = false;          // For MinIO compatibility
};
```

### 5.6 Dependency: AWS SDK for C++

Detection follows the same pattern as RocksDB/SQLite:

```cpp
#if __has_include(<aws/s3/S3Client.h>)
#  define CELER_HAS_S3 1
#else
#  define CELER_HAS_S3 0
#endif
```

When the SDK is absent, `backends::s3::factory()` returns a stub that produces `Error{"NotAvailable", "compiled without S3 support"}`.

---

## 6. Prototype Pattern — Deep Dive

### 6.1 Why Prototype for Streams

The Prototype pattern (GoF §3.4) is "specify the kinds of objects to create using a prototypical instance, and create new objects by copying this prototype." In celer streams:

- **The prototypical instance** is any `StreamHandle<T>` — it holds type-erased state
- **Copying** is via `clone()` — the vtable's `clone_fn` deep-copies the concrete implementation
- **The caller doesn't know the concrete type** — Prototype decouples cloning from type knowledge

### 6.2 Why Not Just Copy Constructor

```
StreamHandle<T> a = ...;
StreamHandle<T> b = a;      // ← Deleted. Not allowed.
StreamHandle<T> c = a.clone(); // ← Explicit. Allocates. Correct.
```

Streams model positions in a sequence. Implicit copy would create aliased cursors — both `a` and `b` think they're at the same position, but pulling one doesn't advance the other. This semantic confusion is a bug factory.

`clone()` is explicit: "I am forking this stream into two independent positions." The name signals the allocation cost and the independence guarantee.

### 6.3 Prototype Enables Opaque Backend Extension

S3 streams carry backend-specific state invisible to the `StreamHandle<T>` type:

| Stream type | Hidden state |
|-------------|-------------|
| `S3GetStream` | Byte offset, done flag, S3 client handle |
| `S3ListStream` | Continuation token, done flag, S3 client handle |
| `MapImpl<T,U,Fn>` | Inner source stream, transform function |

`clone()` deep-copies all of this without the caller knowing about S3 tokens or map functions. This is the Prototype pattern's core value: polymorphic cloning without type knowledge.

### 6.4 Prototype Across Composition

Cloning a composed stream clones the entire pipeline:

```
clone(Map(Filter(S3ListStream{token="abc"})))
  → Map(clone(Filter(S3ListStream{token="abc"})))
  → Map(Filter(clone(S3ListStream{token="abc"})))
  → Map(Filter(S3ListStream{token="abc"}))  // independent copy
```

Each layer's copy constructor calls `source_.clone()` on its inner stream. The Prototype pattern composes: cloning at any depth produces a fully independent pipeline.

---

## 7. Thread Safety

| Component | Safety | Mechanism |
|-----------|--------|-----------|
| `Chunk<T>` | Thread-safe | `shared_ptr<const ...>` — immutable, atomic refcount |
| `StreamHandle<T>` | NOT shared | Move-only; one owner pulls. Clone for fan-out |
| `StreamVTable<T>` | Thread-safe | `static constexpr` — never mutated |
| `S3GetStream` | NOT shared | Mutable offset state; clone for parallel pulls |
| `S3ListStream` | NOT shared | Mutable token state; clone for parallel pages |
| `Aws::S3::S3Client` (shared) | Thread-safe | AWS SDK guarantees thread-safe client |

**Rule:** Same as RFC-001 — the library never spawns threads. A `StreamHandle<T>` is single-owner. To consume from multiple threads, `clone()` first, give each thread its own handle.

---

## 8. Performance

### 8.1 Streaming Overhead

| Operation | Cost |
|-----------|------|
| `pull()` dispatch (single layer) | ~2ns (constexpr vtable indirect call) |
| `pull()` dispatch (N-deep composition) | ~2N ns (vtable hops stack — see §3.7) |
| Chunk creation (from existing buffer) | One `make_shared` + one `vector` move |
| Chunk slice | O(1) — shared_ptr copy + offset arithmetic |
| `clone()` | One heap allocation + deep copy of stream state |
| Composition (map/filter/take) | One heap allocation per combinator |

### 8.2 S3 Backend Targets

| Operation | Target |
|-----------|--------|
| `stream_get` pull (8MB chunk) | < 200ms (network-bound, not CPU-bound) |
| `stream_put` per-part (5MB) | < 300ms (multipart upload) |
| `stream_scan` per-page (1000 keys) | < 500ms (list + fetch per key) |
| `get` (small value, < 1KB) | < 150ms (single HTTP GET) |
| `put` (small value, < 1KB) | < 150ms (single HTTP PUT) |
| `batch` (100 ops) | < 15s (sequential, no S3 batch write) |

### 8.3 Memory Bounds

| Scenario | Memory |
|----------|--------|
| Streaming 1GB file via `stream_get` | ~chunk_size (5-8MB) resident at any time |
| Scanning 1M keys via `stream_scan` | ~1000 KVPairs per page in memory |
| Materializing via `collect()` | Full result in memory (same as RFC-001) |

---

## 9. Build System Changes

### 9.1 New Files

```
include/celer/core/stream.hpp          # Chunk, StreamVTable, StreamHandle, combinators
include/celer/backend/s3.hpp           # S3Backend declaration + Config
src/backend/s3.cpp                     # S3Backend implementation
```

### 9.2 Modified Files

```
include/celer/backend/concept.hpp      # Extended concept + vtable + handle
include/celer/backend/sqlite.hpp       # +3 streaming method declarations
include/celer/backend/rocksdb.hpp      # +3 streaming method declarations
src/backend/sqlite.cpp                 # +3 streaming stub implementations
src/backend/rocksdb.cpp                # +3 streaming stub implementations
include/celer/celer.hpp                # +stream.hpp, +s3.hpp includes
```

### 9.3 New Dependency

| Dependency | Version | Required? | Notes |
|------------|---------|-----------|-------|
| aws-sdk-cpp (s3) | ≥ 1.11 | No | Optional. Auto-detected via `__has_include`. Needed only for S3 backend |

### 9.4 Makefile Integration

```makefile
# Auto-detect AWS SDK
S3_DETECT := $(shell echo '#include <aws/s3/S3Client.h>' | $(CXX) -x c++ -E - 2>/dev/null && echo 1 || echo 0)

# S3 source compiled only if SDK present
ifeq ($(S3_DETECT),1)
  SRCS += src/backend/s3.cpp
  LDFLAGS += -laws-cpp-sdk-s3 -laws-cpp-sdk-core
endif
```

---

## 10. v2 Scope (This RFC)

### In Scope

- [x] `Chunk<T>` — Okasaki-immutable, shared-ownership, O(1) slice
- [x] `StreamVTable<T>` — constexpr vtable with Prototype clone
- [x] `StreamHandle<T>` — type-erased, pull-based, RAII, move-only + clone()
- [x] Composition: `map`, `filter`, `take`, `flat_map`
- [x] Terminals: `collect`, `collect_string`, `fold`, `count`, `drain`
- [x] Constructors: `empty`, `singleton`, `from_vector`, `from_string`
- [x] `StorageBackend` concept extension: `stream_get`, `stream_put`, `stream_scan`
- [x] `BackendVTable` + `BackendHandle` extension
- [x] SQLite streaming stubs (materializing)
- [x] RocksDB streaming stubs (materializing)
- [x] `S3Backend` — native streaming implementation
- [x] S3GetStream — ranged GET chunked download
- [x] S3ListStream — paginated ListObjectsV2 scan
- [x] Multipart upload via stream_put
- [x] MinIO/LocalStack compatibility (endpoint_url, path-style)
- [x] Chunk compression — per-chunk snappy/lz4 with self-framing (see §13)
- [x] Async streaming — pollable stealable stream scheduler with budgeted advancement, demand credits, lock-free Chase-Lev deques, `par_map` / `par_eval` / `concat_map` / `merge_map` combinators (see §14)
- [x] S3 batch delete — `DeleteObjects` API, up to 1000 keys per call (see §15)
- [x] S3 versioned object support — version-aware get/stream_get, version metadata (see §16)

### Out of Scope (v3+)

- [ ] Combinator fusion — collapse `map(filter(...))` into single-hop `MapFilterImpl` via vtable identity check (see §3.7)
- [ ] Deferred erasure / staged pipelines — typed composition chain, erase only at terminal
- [ ] RocksDB native iterator-backed streaming (avoid materialization in stubs)
- [ ] SQLite cursor-backed streaming
- [ ] S3 Select (push-down predicate to S3)
- [ ] Stream-to-stream piping (connect output of one backend to input of another)
- [ ] Backpressure / rate limiting

---

## 11. Open Questions

| # | Question | Status |
|---|----------|--------|
| 1 | **Ranged GET vs. streaming response body?** | Ranged GET chosen for retry-safety and Prototype correctness. Re-evaluate if latency per-chunk is problematic |
| 2 | **Chunk size default?** | 5MB matches S3 minimum multipart part. 8MB may be better for throughput. Configurable either way |
| 3 | **stream_scan fetches values per key?** | Current design fetches each object body during scan. For key-only listing, add `stream_list_keys()` in v3 |
| 4 | **flat_map semantics with chunks?** | Element-by-element flatMap breaks chunk batching. May need chunk-level flatMap variant |
| 5 | **Should StorageBackend concept require streaming?** | Yes (this RFC). Custom backends must implement the 3 methods. Copy-paste stubs provided as template |
| 6 | **Composition dispatch stacking cost?** | Accepted for v2 (~2ns×depth per pull, negligible vs I/O). Combinator fusion tracked for v3 (§3.7) |

---

## 12. Design Comparison: celer::stream vs. fs2

| Feature | fs2 (Scala) | celer::stream (C++23) |
|---------|-------------|----------------------|
| Stream representation | Free monad (`Pull[F, O, R]`) | Type-erased vtable (`StreamHandle<T>`) |
| Effect type | Polymorphic `F[_]` (IO, Task, etc.) | Fixed: `Result` (fallible, synchronous) |
| Chunk type | `Chunk[O]` (immutable, Array-backed) | `Chunk<T>` (immutable, `shared_ptr<const vector<T>>`) |
| Composition | `.map`, `.filter`, `.flatMap` (methods) | `stream::map`, `stream::filter` (free functions) |
| Resource safety | `bracket` / `Resource[F, A]` | RAII: vtable `destroy_fn` + `ResourceStack` |
| Concurrency | `Stream[F, O].parEvalMap` | `clone()` + manual thread dispatch |
| Cloning | Referentially transparent (value semantics) | Prototype pattern (`clone()` explicit deep-copy) |
| Type erasure | HKT + implicits (no vtable) | Constexpr vtable (~2ns dispatch) |
| Lazy vs eager | Fully lazy (description) | Pull-through: lazy per-chunk, eager within chunk |

**Key insight carried over from Pilquist:** Streams are values, not running processes. A `StreamHandle<T>` describes a computation. You can compose it freely. Only `pull()` (or a terminal like `collect()`) actually does I/O. This is the same "embedded DSL" philosophy as fs2, adapted to C++'s ownership model.

---

## 13. Design: Chunk Compression (snappy/lz4)

### 13.1 Motivation

S3 egress costs $0.09/GB. For large values — agent memory snapshots, trace spans, serialized models — per-chunk compression reduces both transfer cost and latency. snappy and lz4 are the two dominant choices in the embedded-store ecosystem (RocksDB uses both internally).

### 13.2 Architecture

Compression is a **stream combinator**, not a backend concern:

```
raw_stream → compress(stream, codec) → [framed compressed bytes] → backend.stream_put()
backend.stream_get() → [framed compressed bytes] → decompress(stream, codec) → raw_stream
```

This design keeps backends codec-agnostic. Mixed storage (some keys compressed, some not) is valid. Codec selection is caller-controlled per-stream.

### 13.3 Frame Format

Each compressed chunk is self-framed:

```
┌──────────────┬───────────────────────┐
│ 4B orig_size │ compressed_data       │
│ (LE uint32)  │ (snappy or lz4 bytes) │
└──────────────┴───────────────────────┘
```

The 4-byte header stores the original uncompressed size in little-endian. LZ4 decompression requires knowing the output buffer size upfront; snappy self-describes but we frame uniformly for codec-generic parsing.

### 13.4 Codec Detection

Compile-time, matching the S3/RocksDB pattern:

```cpp
#if __has_include(<snappy.h>)
#  define CELER_HAS_SNAPPY 1
#endif
#if __has_include(<lz4.h>)
#  define CELER_HAS_LZ4 1
#endif
```

Runtime query: `compression::is_available(Codec::snappy)` returns `constexpr bool`.

### 13.5 Stream Combinators

```cpp
namespace celer::stream {
    auto compress(StreamHandle<char>, compression::Codec) -> StreamHandle<char>;
    auto decompress(StreamHandle<char>, compression::Codec) -> StreamHandle<char>;
}
```

`compress` wraps each pulled chunk through `compress_block()` → framed output chunk.  
`decompress` reads the frame header, decompresses, yields raw output chunk.  
Roundtrip identity: `decompress(compress(s, c), c) ≡ s`.

### 13.6 Performance Targets

| Codec | Compress speed | Decompress speed | Ratio (typical) |
|-------|---------------|------------------|------------------|
| snappy | ~250 MB/s | ~500 MB/s | 1.5-2.0x |
| lz4 | ~500 MB/s | ~1.5 GB/s | 1.5-2.5x |
| none | memcpy | memcpy | 1.0x |

For 5MB S3 chunks, snappy adds ~20ms per chunk; lz4 adds ~10ms. Both are negligible vs. 100ms S3 RTT.

### 13.7 New File

```
include/celer/core/compression.hpp    # Codec enum, compress/decompress_block, stream combinators
```

---

## 14. Design: Async Stealable Streams & Budgeted Stream Scheduler

### 14.1 Motivation

Pull-based streaming is inherently sequential: `pull()` blocks until the chunk arrives. For S3 where each pull is a 50-200ms HTTP roundtrip, this serialization leaves bandwidth on the table.

But the fix is not "thread pool + submit(lambda)." That creates fake async — workers calling blocking `pull()`, threads stalling on I/O, work-stealing degenerating into thread parking. The novelty evaporates.

The real primitive is: **stealable stream continuations with budgeted advancement**.

The pool does not steal generic tasks. It steals:

- a stream continuation
- with remaining demand (backpressure credits)
- with a budget (max chunks, max bytes, max time)
- with locality metadata (worker affinity, steal cost)
- with cancellation state

Instead of "steal lambda task," it becomes: **steal permission to advance this stream for 64KB, 1 chunk, or 50µs**. That gives fairness across hot streams, bounded tail latency, no single producer monopolizing a worker, and real execution identity for streams.

### 14.2 Layer Architecture

RFC-002's sync streaming (§3) becomes **Layer 1: value algebra**. The async system builds on top:

```
Layer 4: Adapters          sync collect(), async for_each(), to_channel(), backend adapters
Layer 3: Stream Scheduler  work-stealing pool, stealable continuations, demand routing
Layer 2: Pollable core     PollResult, StreamBudget, AsyncStreamHandle, demand credits
Layer 1: Value algebra     Chunk<T>, StreamHandle<T>, map/filter/take (sync, unchanged)
```

Layer 1 is untouched. Layer 2 introduces a non-blocking `poll()` contract. Layer 3 is the scheduler. Layer 4 bridges sync and async.

### 14.3 PollResult — The Execution Contract

The sync contract `Result<optional<Chunk<T>>>` can only express three states: Emit, Done, Error. A scheduler needs more:

```cpp
template <typename T>
struct PollResult {
    enum class Kind : std::uint8_t {
        Emit,     // chunk produced — consume it
        Pending,  // blocked on I/O or dependency, continuation registered, wake me
        Yield,    // still runnable but budget exhausted, requeue me
        Done,     // stream exhausted, no more chunks
        Error     // unrecoverable failure
    };

    Kind kind;
    Chunk<T> chunk;   // valid only when kind == Emit
    Error error;      // valid only when kind == Error
};
```

**Pending** means: no chunk now, continuation registered, wake me when event or credit arrives. The worker parks this stream and moves on. This is the key difference from blocking `pull()` — the worker thread is never blocked.

**Yield** means: still runnable, voluntarily yielded after budget exhaustion. The scheduler can requeue locally (same worker, LIFO, cache-warm) or allow another worker to steal it.

### 14.4 StreamBudget — Bounded Advancement

```cpp
struct StreamBudget {
    std::uint32_t max_chunks{1};              // max chunks to emit per poll
    std::uint32_t max_bytes{64 * 1024};       // max bytes to emit per poll (64KB default)
    std::uint64_t max_ns{50'000};             // max wall-clock per poll (50µs default)
};
```

A worker advances a stream **at most** until one of these limits is hit. Then the stream yields, allowing the scheduler to:

- rebalance across workers
- avoid starving other streams
- bound per-stream tail latency
- allow steal of the continuation

Budget values are tuneable per-stream. S3 streams get generous budgets (100ms, 8MB) because each pull is network-bound. Local RocksDB streams get tight budgets (50µs, 64KB) to prevent monopolization.

### 14.5 AsyncStreamVTable — Constexpr Vtable (Extended)

```cpp
template <typename T>
struct AsyncStreamVTable {
    auto (*poll_fn)(void* ctx, StreamBudget budget, TaskContext& cx) -> PollResult<T>;
    void (*request_fn)(void* ctx, std::size_t n);
    void (*cancel_fn)(void* ctx);
    auto (*clone_fn)(const void* ctx) -> void*;     // semantic clone only
    void (*destroy_fn)(void* ctx);
};
```

Same constexpr pattern as `StreamVTable<T>` and `BackendVTable` — one static instance per (T, Impl) pair. No virtual, no std::function, ~2ns dispatch.

**`clone_fn`** is semantic clone only — Prototype pattern for user-visible stream forking. It is NOT used for execution splitting. Workers steal leases, not clones.

### 14.6 AsyncStreamHandle — Pollable, Budget-Aware

```cpp
template <typename T>
class AsyncStreamHandle {
    void* ctx_;
    const AsyncStreamVTable<T>* vtable_;
public:
    auto poll(StreamBudget budget, TaskContext& cx) -> PollResult<T>;
    void request(std::size_t n);   // grant N demand credits
    void cancel();                 // cooperative cancellation
    auto clone() const -> AsyncStreamHandle<T>;  // semantic fork only
    auto valid() const noexcept -> bool;

    // Move-only, same as StreamHandle
    AsyncStreamHandle(AsyncStreamHandle&&) noexcept;
    AsyncStreamHandle(const AsyncStreamHandle&) = delete;
    ~AsyncStreamHandle();
};
```

### 14.7 Demand Credits — First-Class Backpressure

```cpp
struct StreamControl {
    std::atomic<std::uint32_t> requested{0};   // downstream demand
    std::atomic<std::uint32_t> in_flight{0};   // chunks submitted, not yet consumed
    std::atomic<std::uint32_t> buffered{0};    // chunks ready in output queue
    std::atomic<bool> cancelled{false};         // cooperative cancellation
};
```

A worker only advances a stream if:

1. `requested > 0` (there is downstream demand)
2. `buffered < high_watermark` (output queue is not full)
3. `!cancelled`

This prevents dumb work: no prefetching chunks nobody asked for, no over-buffering hot producers, no starving slower consumers. The demand counter is decremented when a chunk is consumed downstream, incremented by `request(n)`.

### 14.8 Semantic Clone vs. Execution Lease

Two fundamentally different operations:

| | Semantic clone (`clone()`) | Execution lease (internal) |
|-|---|---|
| **Who** | User code | Scheduler only |
| **Semantics** | Independent fork of logical stream state | Temporary authority to advance the same logical stream |
| **Result** | Two independent streams at same position | One stream, one worker advancing it at a time |
| **Ownership** | User owns the clone | Lease returned when poll yields/pending |
| **Concurrency** | Both can advance independently | Never concurrent — lease is exclusive |

```cpp
struct StreamLease {
    AsyncStreamHandleBase* stream;   // non-owning — scheduler owns the handle
    StreamBudget budget;
    std::uint32_t worker_affinity;   // last worker that ran this stream
    std::uint32_t steal_cost;        // higher = prefer local, discourage steal
    StreamControl* control;          // demand/cancel shared state
};
```

Workers steal leases, not clones. The lease carries:

- **worker_affinity**: prefer same worker for cache locality
- **steal_cost**: weight factor — I/O-bound streams (S3) have low steal cost (stateless HTTP), CPU-bound streams have high steal cost (hot cache)

### 14.9 Stream Scheduler — Work-Stealing Pool

Not a generic task pool. A **stream-aware scheduler** where the unit of work is a `StreamLease`.

```
┌──────────────────────────────────────────────────────────┐
│ StreamScheduler                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  ...           │
│  │ runnable │  │ runnable │  │ runnable │                 │
│  │  deque   │  │  deque   │  │  deque   │                │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘               │
│       │  steal ←───→ │  steal ←───→ │                    │
│       ▼              ▼              ▼                    │
│   ┌────────────────────────────────────────────────┐     │
│   │  schedule(AsyncStreamHandle&, budget, control) │     │
│   │  → assigns lease to worker, respects affinity  │     │
│   └────────────────────────────────────────────────┘     │
│                                                          │
│   Parked streams: wait set (wake on event/demand)        │
│   Cancelled streams: reap list                           │
└──────────────────────────────────────────────────────────┘
```

Worker loop (pseudocode):

```
loop:
    lease = pop_local()          // LIFO — cache-warm
           ?? steal_random()     // FIFO from random peer — load balance
           ?? pop_global()       // fallback: global submission queue
           ?? park_and_wait()

    if lease.control->cancelled:
        reap(lease)
        continue

    if lease.control->requested == 0:
        park(lease)              // no demand — don't advance
        continue

    result = lease.stream->poll(lease.budget, task_context)

    match result.kind:
        Emit:
            push_chunk(result.chunk, lease.control)
            if budget_remaining(lease):
                push_local(lease)    // keep going, same worker
            else:
                push_local(lease)    // yield locally, may be stolen
        Pending:
            park(lease)              // I/O wait — remove from runnable
        Yield:
            push_local(lease)        // budget exhausted, requeue
        Done:
            complete(lease)
        Error:
            fail(lease, result.error)
```

### 14.10 Lock-Free Deques

Per-worker deques use the Chase-Lev algorithm (lock-free for owner push/pop, CAS for steal):

- **Owner push/pop**: single-thread, no CAS, ~3ns
- **Steal**: one CAS on the bottom pointer, ~8ns on contention
- No mutex anywhere in the hot path

The global submission queue uses a lock-free MPSC (multi-producer single-consumer) queue for external `schedule()` calls.

### 14.11 TaskContext — Scheduler Hooks

```cpp
struct TaskContext {
    std::uint32_t worker_id;
    StreamScheduler* scheduler;     // for re-scheduling, parking, waking

    // Wake a parked stream (called from I/O completion or demand grant)
    void wake(StreamLease& lease);
};
```

Async stream implementations (e.g., S3 I/O) register wake callbacks when returning `Pending`. When the I/O completes, the callback calls `cx.wake(lease)` to move the stream from parked → runnable.

### 14.12 Async Combinators

```cpp
namespace celer::stream {
    // Parallel map: submit chunk transforms to scheduler, preserve ordering.
    auto par_map(AsyncStreamHandle<T>, Fn, StreamScheduler&, std::size_t concurrency = 4)
        -> AsyncStreamHandle<U>;

    // Merge multiple streams: interleave first-available (non-deterministic).
    auto par_eval(std::vector<AsyncStreamHandle<T>>, StreamScheduler&)
        -> AsyncStreamHandle<T>;

    // Ordered concatenation: one child active at a time.
    auto concat_map(AsyncStreamHandle<T>, Fn, StreamScheduler&)
        -> AsyncStreamHandle<U>;

    // Bounded concurrency merge: up to N children active.
    auto merge_map(AsyncStreamHandle<T>, Fn, StreamScheduler&, std::size_t max_concurrent = 4)
        -> AsyncStreamHandle<U>;
}
```

`flat_map` splits into `concat_map` (ordered, one child) and `merge_map` (bounded concurrency, unordered). This is honest about the scheduling policy.

### 14.13 Sync ↔ Async Bridge

```cpp
// Lift a sync StreamHandle into an async one (poll wraps pull, never Pending)
auto to_async(StreamHandle<T>) -> AsyncStreamHandle<T>;

// Block on an async stream, collect synchronously (for terminal use)
auto collect_blocking(AsyncStreamHandle<T>&, StreamScheduler&) -> Result<std::vector<T>>;
```

### 14.14 Performance Targets

| Operation | Target |
|-----------|--------|
| Lease pop (local, no contention) | ~3ns (Chase-Lev hot path) |
| Lease steal (one CAS) | ~8ns |
| poll() dispatch (constexpr vtable) | ~2ns |
| Full poll cycle (pop + poll + requeue) | < 20ns |
| S3 chunk prefetch (4 inflight) | 4× throughput vs. sequential |
| Stream fairness (10 competing streams) | < 2× variance in chunk rate |

### 14.15 New Files

```
include/celer/core/poll_result.hpp     # PollResult, StreamBudget, StreamControl, TaskContext
include/celer/core/async_stream.hpp    # AsyncStreamVTable, AsyncStreamHandle, make_async_stream_handle
include/celer/core/scheduler.hpp       # StreamScheduler, StreamLease, Chase-Lev deque
```

---

## 15. Design: S3 Batch Delete (DeleteObjects API)

### 15.1 Motivation

The current `S3Backend::batch()` executes deletes sequentially — one `DeleteObject` per key. S3's `DeleteObjects` API can delete up to 1000 keys per call, reducing N HTTP roundtrips to ⌈N/1000⌉.

### 15.2 Algorithm

```
batch(ops):
  1. Partition ops into puts[] and deletes[]
  2. Execute puts sequentially (no S3 batch write API)
  3. Chunk deletes[] into groups of 1000
  4. For each group: issue DeleteObjects(bucket, keys)
  5. Collect errors from DeleteObjects error list
  6. If any individual delete failed, return first error
```

### 15.3 API Surface

No public API change — `batch(span<const BatchOp>)` signature unchanged. The optimization is internal to `S3Backend::batch()`. Callers see faster bulk deletes transparently.

### 15.4 Error Handling

`DeleteObjects` returns a list of per-key errors (partial failure). We iterate the error list and return the first failure as `Error{"S3BatchDel", key + ": " + message}`. The remaining deletes in the batch are **not aborted** — S3 already processed them.

### 15.5 Modified File

```
src/backend/s3.cpp    # S3Backend::batch() rewritten
```

---

## 16. Design: S3 Versioned Object Support

### 16.1 Motivation

S3 versioning enables point-in-time recovery: every `PutObject` creates a new version, and old versions are accessible by version ID. For agent memory stores, this enables audit trails and rollback.

### 16.2 Architecture

Versioning is optional and additive:

- `S3Config` gains `versioning_enabled` flag
- `S3Backend` gains `get_versioned(key, version_id)` and `stream_get_versioned(key, version_id)` methods
- `put()` returns the version ID via a new `PutResult` struct (wraps `VoidResult` + optional version_id)
- `list_versions(key)` returns a stream of version metadata

### 16.3 New Types

```cpp
struct VersionInfo {
    std::string version_id;
    std::string last_modified;   // ISO 8601
    std::size_t size;
    bool is_latest;
    bool is_delete_marker;
};

struct PutResult {
    std::string version_id;  // empty if versioning disabled
};
```

### 16.4 Extended S3Backend API

```cpp
class S3Backend {
    // ... existing methods ...

    // Version-aware get: retrieve a specific version of an object
    auto get_versioned(std::string_view key, std::string_view version_id)
        -> Result<std::optional<std::string>>;

    // Version-aware streaming get
    auto stream_get_versioned(std::string_view key, std::string_view version_id)
        -> Result<StreamHandle<char>>;

    // List all versions of an object (paginated stream)
    auto list_versions(std::string_view key)
        -> Result<StreamHandle<VersionInfo>>;

    // Put with version metadata return
    auto put_versioned(std::string_view key, std::string_view value)
        -> Result<PutResult>;
};
```

### 16.5 S3VersionStream — Paginated Version Listing

```cpp
struct S3VersionStream {
    shared_ptr<S3Client> client;
    string bucket, key_prefix;
    string key_marker, version_id_marker;
    bool done{false};

    auto pull() -> Result<optional<Chunk<VersionInfo>>>;
    S3VersionStream(const S3VersionStream&) = default;  // Prototype
};
```

Uses `ListObjectVersions` with key/versionId markers for pagination.

### 16.6 Backward Compatibility

Versioning methods are **additive** — they do not change the `StorageBackend` concept. They are S3-specific extensions on `S3Backend` directly. Non-versioned `get()`/`put()` continue to operate on the latest version (S3 default behavior).

### 16.7 Config Extension

```cpp
struct Config {
    // ... existing fields ...
    bool versioning_enabled = false;  // opt-in
};
```

### 16.8 Modified/New Files

```
include/celer/backend/s3.hpp    # VersionInfo, PutResult, extended S3Backend
src/backend/s3.cpp              # Version-aware implementations
```

---

*End of RFC 002*
