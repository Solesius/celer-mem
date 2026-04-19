# RFC 004: Necto — Brokerless Actor IPC for celer-mem

## Status: Accepted
## Author: Khalil Warren (🦎 Sal)
## Date: 2026-04-15 (promoted 2026-04-19)
## Depends: RFC-001 (celer-mem core), RFC-002 (streaming primitive)

---

## 1. Abstract

This RFC introduces **`celer::necto`** — a brokerless, pure-actor inter-process communication layer built on top of celer-mem's existing streaming and backend primitives.

Necto (Latin: *"I bind, I connect"* — root of *nexus*) provides:

1. **A minimal Actor concept** — any struct implementing `on_receive(Envelope, ActorContext&)` is a first-class actor.
2. **Per-actor MPSC mailboxes** — one inbox per actor, idempotent delivery via `(sender, seq)` dedup. Fully isolated — no shared mutable state.
3. **No broker** — actors live in a flat `vector<ActorHandle>`, addressed by lightweight `ActorRef` index. The system tick-drives them; they route messages themselves.
4. **Constexpr vtable type-erasure** — same pattern as `StreamHandle<T>` and `BackendHandle` from RFC-001/002.

```cpp
#include <celer/necto/actor.hpp>

// Define actors — each implements on_receive
struct EnergyAnalyst {
    std::vector<StockReport> reports;

    void on_receive(necto::Envelope env, necto::ActorContext& ctx) {
        auto report = deserialize<StockReport>(env.payload);
        reports.push_back(report);
        // Forward result to monitor
        ctx.send(monitor_ref, serialize(report));
    }
};

// Build the system — no broker, just a vector
necto::ActorSystem sys;
auto energy  = sys.spawn<EnergyAnalyst>();
auto tech    = sys.spawn<TechAnalyst>();
auto finance = sys.spawn<FinanceAnalyst>();
auto monitor = sys.spawn<Monitor>(3);  // expects 3 reports

// Inject external trigger
sys.inject(energy,  serialize(SectorRequest{"energy"}));
sys.inject(tech,    serialize(SectorRequest{"tech"}));
sys.inject(finance, serialize(SectorRequest{"finance"}));

// Drain — tick all actors until quiescent
sys.drain();
// Monitor now holds aggregated report
```

**Scope:** This is the open-source IPC primitive for celer-mem. WAL-backed durable messaging, Raft consensus, and multi-node cluster coordination are explicitly out of scope (commercial layer).

---

## 2. Problem Statement

### 2.1 Agent-to-Agent Communication Has No Good C++ Primitive

The landscape for C++ IPC in agent/AI systems:

| Library | Issue |
|---------|-------|
| gRPC | Heavy runtime, schema compilation step, not embeddable |
| ZeroMQ | External dependency, push-based, no built-in ordering/dedup |
| Boost.Interprocess | Low-level shared memory, no message abstraction, no actor model |
| CAF (C++ Actor Framework) | Large framework, own scheduler, doesn't compose with existing event loops |
| std::jthread + queue | Roll your own — everyone does, everyone gets it wrong |

Developers building multi-agent C++ systems (swarms, pipelines, cooperative tool-use) need a minimal, embeddable actor primitive that:
- Requires no external daemon
- Has no framework lock-in
- Composes with celer-mem's existing streaming and storage layers
- Is small enough to audit in an hour

### 2.2 Why Actor Model

| Property | Benefit for Agent Systems |
|----------|--------------------------|
| **Isolation** | Each agent owns its state. No shared memory, no data races by construction |
| **Location transparency** | Same API whether actors are in-process, cross-process (future), or cross-node (commercial) |
| **Fault containment** | One actor panics → its mailbox drains, others continue. No cascading failures |
| **Natural concurrency** | No mutex-based synchronization in user code. The system schedules delivery |
| **Idempotent delivery** | Exactly-once semantics via `(sender, seq)` dedup — safe for retry without side-effects |

### 2.3 Why Brokerless

Traditional actor frameworks (Akka, Erlang/OTP, CAF) use a central broker or supervisor hierarchy. This adds:
- A routing table lookup per message
- A single-point-of-failure coordinator
- Framework-owned thread pools that conflict with the user's scheduler

Necto's approach: **no broker**. The `ActorSystem` is just a `vector<ActorHandle>`. Routing is O(1) array indexing by `ActorRef`. The tick loop is the user's — call `tick_all()` from your event loop, your thread pool, or `drain()` for batch processing. The system owns no threads.

---

## 3. Design

### 3.1 Core Types

```
ActorRef                  — uint32_t index into the actor vector
Envelope                  — {seq, from, to, payload} — the atom of communication
Mailbox                   — per-actor MPSC queue with idempotent dedup
ActorContext              — injected send() capability during on_receive
ActorVTable               — constexpr {on_receive_fn, destroy_fn}
ActorHandle               — type-erased, owning wrapper: void* + vtable + mailbox
ActorSystem               — vector<ActorHandle> + tick/drain loop
```

### 3.2 ActorRef — Lightweight Handle

```cpp
struct ActorRef {
    uint32_t index{UINT32_MAX};

    [[nodiscard]] constexpr bool valid() const noexcept;
    constexpr bool operator==(const ActorRef&) const noexcept = default;
};
```

An `ActorRef` is a 4-byte value type. It indexes into the `ActorSystem`'s flat vector. `UINT32_MAX` is the sentinel (invalid/null ref). No heap allocation, no reference counting, no lifetime tracking — the system owns all actors and refs are valid for the system's lifetime.

**Why not a pointer or ID?** Pointers break on vector reallocation. String IDs require hash lookups. Integer indices are O(1), trivially copyable, and fit in a register.

**Capacity:** 2^32 - 2 actors per system (~4 billion). If you need more actors than this in a single process, you have a different kind of problem.

### 3.3 Envelope — The Communication Atom

```cpp
struct Envelope {
    uint64_t          seq{0};      // per-sender monotonic sequence number
    ActorRef          from{};      // sender ref (UINT32_MAX-1 for external)
    ActorRef          to{};        // receiver ref
    std::vector<char> payload;     // opaque bytes — actor decides encoding
};
```

**Why opaque payload?** Actors are heterogeneous — an energy analyst sends `StockReport`, a monitor sends `AggregatedSummary`. Typing the envelope to `T` would require all actors to agree on a common message type. Opaque bytes push serialization to the edge: the sender serializes, the receiver deserializes. This matches real-world agent protocols (JSON-RPC, protobuf, MsgPack) and composes with celer-mem's existing `serde::Codec<T>`.

**Why per-sender monotonic seq?** The `(from.index, seq)` pair is a globally unique message identifier without requiring a central sequence generator. Each sender maintains its own counter. The receiver's mailbox deduplicates on this pair.

### 3.4 Mailbox — Per-Actor MPSC Inbox

```cpp
class Mailbox {
    mutable std::mutex mtx_;
    std::deque<Envelope> queue_;
    std::unordered_set<DedupKey, DedupHash> seen_;

public:
    auto push(Envelope env) -> bool;         // MPSC: any thread can push
    auto pop() -> std::optional<Envelope>;   // SPSC: only owning actor pops
    auto empty() const -> bool;
    auto pending() const -> std::size_t;
    void trim_dedup(std::size_t max = 100'000);
};
```

**MPSC design:** Multiple producers (any actor can send to any actor via `deliver()`) but single consumer (only the owning actor pops during its `on_receive` tick). The mutex protects the queue and dedup set — both are accessed during push (any thread) and pop (tick thread).

**Idempotent delivery:** `push()` checks `(from.index, seq)` against the `seen_` set. Duplicates return `false` and are dropped. This enables safe retry semantics: if a sender is unsure whether a message was delivered, it resends with the same seq. The receiver processes it exactly once.

```cpp
struct DedupKey {
    uint32_t from;
    uint64_t seq;
    bool operator==(const DedupKey&) const noexcept = default;
};
```

**Dedup trimming:** The `seen_` set grows monotonically. `trim_dedup()` clears it when it exceeds a threshold. This is safe because sequence numbers are monotonic — a cleared entry at seq=5 won't be replayed unless the sender wraps at 2^64 (heat death of the universe at 1M msgs/sec ≈ 584,942 years).

### 3.5 ActorContext — Injected Send Capability

```cpp
class ActorContext {
    ActorRef self_;
    uint64_t next_seq_{0};
    std::function<bool(Envelope)> deliver_;

public:
    [[nodiscard]] auto self() const noexcept -> ActorRef;
    auto send(ActorRef to, std::vector<char> payload) -> bool;
};
```

**Why injected?** Actors don't hold a reference to the system. The system constructs an `ActorContext` before each tick and passes it to `on_receive`. This enforces:

1. **Isolation** — actors can't access the system's internals or other actors' state
2. **Testability** — mock the context to unit-test an actor without a system
3. **No circular dependency** — actors don't `#include` ActorSystem

**Sequence tracking:** The context owns `next_seq_` for the actor's outgoing messages. After `on_receive` returns, the system writes back the updated seq counter to the `ActorHandle`. This avoids the actor needing to manage its own sequence state.

### 3.6 Actor Concept + Constexpr VTable

```cpp
template <typename A>
concept Actor = requires(A& a, Envelope env, ActorContext& ctx) {
    { a.on_receive(std::move(env), ctx) };
};
```

**One method.** That's the entire contract. No `on_start`, no `on_stop`, no `pre_receive`, no supervision strategy. Actors are structs with `on_receive`. Everything else is the actor's own business.

```cpp
struct ActorVTable {
    void (*on_receive)(void* self, Envelope env, ActorContext& ctx);
    void (*destroy)(void* self);
};

template <Actor A>
inline constexpr ActorVTable vtable_for = {
    .on_receive = [](void* self, Envelope env, ActorContext& ctx) {
        static_cast<A*>(self)->on_receive(std::move(env), ctx);
    },
    .destroy = [](void* self) { delete static_cast<A*>(self); },
};
```

Same pattern as `StreamVTable<T>` (RFC-002 §3.3) and `BackendVTable` (RFC-001 §4.2):
- One `static constexpr` instance per actor type
- No heap allocation for the vtable itself
- ~2ns indirect call per dispatch
- No `std::function`, no `std::any`, no virtual inheritance

### 3.7 ActorHandle — Type-Erased Owning Wrapper

```cpp
class ActorHandle {
    void*                    impl_{nullptr};
    const ActorVTable*       vt_{nullptr};
    std::unique_ptr<Mailbox> inbox_;
    ActorRef                 self_{};
    uint64_t                 next_seq_{0};
};
```

**Owns:** the concrete actor (via `impl_`), its mailbox, and its outgoing seq counter.

**Move-only:** actors are not copyable. There is exactly one instance of each actor, owned by exactly one system. No Prototype pattern here — unlike streams, actors are stateful entities, not position-in-a-sequence descriptions. Cloning an actor would duplicate side effects.

**Lifetime:** `ActorHandle` destruction calls `vt_->destroy(impl_)` — RAII cleanup of the concrete actor. The mailbox is `unique_ptr` and self-destructs.

### 3.8 ActorSystem — The Flat Vector

```cpp
class ActorSystem {
    std::vector<ActorHandle> actors_;

public:
    template <Actor A, typename... Args>
    auto spawn(Args&&... args) -> ActorRef;

    auto deliver(Envelope env) -> bool;
    auto inject(ActorRef to, std::vector<char> payload) -> bool;

    auto tick(ActorRef ref) -> bool;
    auto tick_all() -> std::size_t;
    auto drain(std::size_t max_rounds = 10'000) -> std::size_t;

    auto operator[](ActorRef ref) -> ActorHandle&;
    [[nodiscard]] auto size() const -> std::size_t;
};
```

**`spawn()`:** Constructs the actor, wraps it in a handle, pushes it onto the vector. Returns the index as `ActorRef`. Actors are never removed — the vector is append-only. Dead actors are handled by the actor itself (ignore messages, or future: tombstone flag).

**`deliver()`:** Routes an envelope to the target's mailbox via `actors_[env.to.index].mailbox().push()`. O(1) array index — no routing table, no hash lookup, no broker.

**`inject()`:** External entry point. Creates an envelope with a sentinel sender ref (`UINT32_MAX - 1`) and seq 0. Used to kick-start the system from outside.

**`tick(ref)`:** Pop one message from the actor's mailbox, construct an `ActorContext`, call `on_receive`. Returns `false` if the mailbox was empty (nothing to do). The context captures a lambda that calls `deliver()` — this is how actors send messages to each other.

**`tick_all()`:** One pass over all actors, tick each once. Returns total messages processed. Fair scheduling — every actor gets one turn per round.

**`drain(max_rounds)`:** Repeatedly `tick_all()` until quiescent (no messages processed in a round) or `max_rounds` exceeded (livelock breaker). This is the batch processing entry point.

### 3.9 Execution Model

Necto does **not** own threads. The tick loop runs wherever the caller puts it:

| Embedding | Pattern |
|-----------|---------|
| Batch processing | `sys.drain()` — run to completion on calling thread |
| Game loop | `sys.tick_all()` once per frame |
| Thread pool | One `sys.tick(ref)` per work item in a TaskPool |
| Event loop | `sys.tick_all()` on each event loop iteration |
| Async runtime | `sys.tick_all()` inside a poll body, yielding between rounds |

**No hidden threads, no hidden allocations, no hidden I/O.** The system is a data structure with a `tick()` method. The caller decides when and where it executes.

**Fairness:** `tick_all()` gives each actor exactly one message per round. An actor that generates many outgoing messages doesn't starve others — those messages queue in receivers' mailboxes and are processed in future rounds.

**Livelock protection:** `drain()` caps iterations at `max_rounds`. Two actors ping-ponging messages forever will hit the cap and return. The caller decides what to do (log, break, escalate).

---

## 4. Invariants

| # | Invariant | Mechanism |
|---|-----------|-----------|
| 1 | **Isolation** | Actors never access each other's state. The only interface is `send(ref, payload)` |
| 2 | **Idempotent delivery** | `(from, seq)` dedup in Mailbox. Duplicate push returns false |
| 3 | **Monotonic ordering per sender** | seq is per-sender monotonic. Receiver sees sender's messages in seq order |
| 4 | **No broker** | Routing is `actors_[ref.index]` — O(1), no indirection |
| 5 | **System owns all actors** | ActorHandle is move-only. No dangling refs if system is alive |
| 6 | **Bounded drain** | `max_rounds` parameter prevents livelock in cyclic message patterns |
| 7 | **External inject is safe** | Sentinel sender ref avoids collision with real actor indices |

---

## 5. Thread Safety

| Component | Safety | Mechanism |
|-----------|--------|-----------|
| `ActorRef` | Thread-safe | Trivially copyable value type, no mutable state |
| `Envelope` | Thread-safe | Value type, moved on send |
| `Mailbox::push()` | Thread-safe | Mutex-protected MPSC |
| `Mailbox::pop()` | Single-consumer | Only called during tick by the system's tick thread |
| `ActorHandle` | NOT shared | Move-only, owned by system |
| `ActorVTable` | Thread-safe | `static constexpr`, never mutated |
| `ActorSystem::tick()` | NOT re-entrant | Single-threaded tick loop. Do not call tick() from on_receive |
| `ActorSystem::deliver()` | Thread-safe | Delegates to Mailbox::push() which is mutex-protected |

**Rule:** The system is single-owner. One thread drives `tick_all()` or `drain()`. Actors may be heterogeneous types, but they all execute on the tick thread — no parallelism within the system. For parallel execution, partition actors across multiple `ActorSystem` instances and bridge with `inject()`.

**Future (out of scope):** Parallel tick — partition actors into lanes, tick lanes on separate threads. The mailbox is already MPSC-safe, so the only change would be ensuring `on_receive` for different actors can run concurrently.

---

## 6. Performance

### 6.1 Operation Costs

| Operation | Cost |
|-----------|------|
| `spawn()` | One heap allocation (actor) + one heap allocation (mailbox) + vector push_back |
| `deliver()` | O(1) array index + mutex lock + deque push_back + dedup set insert |
| `tick()` | Mutex lock + deque pop_front + one vtable indirect call (~2ns) |
| `tick_all()` | N × tick() where N = actor count |
| `drain()` | R × tick_all() where R ≤ max_rounds |
| `inject()` | Same as deliver() with sentinel sender |

### 6.2 Memory

| Component | Per-actor cost |
|-----------|---------------|
| ActorHandle overhead | 40 bytes (impl_ + vt_ + inbox ptr + self_ + next_seq_) |
| Mailbox | 136 bytes base (mutex + deque + unordered_set), grows with queued messages |
| Actor state | User-defined |
| Dedup set | ~48 bytes per unique (sender, seq) entry |

### 6.3 Target Micro-benchmarks

| Benchmark | Target |
|-----------|--------|
| send + deliver + tick (1 message) | < 500ns |
| 10K messages through 4-actor pipeline | < 5ms |
| drain() with 100 actors, 1000 messages total | < 10ms |
| Dedup rejection (duplicate send) | < 200ns |

---

## 7. Demonstration: Stock Sector Analysis

Three analyst agents run sector reports concurrently. A fourth monitor agent watches the WAL (mailbox) for completion, then publishes the aggregated result.

```cpp
#include <celer/necto/actor.hpp>
#include <celer/serde/codec.hpp>

// ── Domain types ──
struct SectorRequest {
    std::string sector;
};

struct StockReport {
    std::string sector;
    double      score;
    std::string summary;
};

struct AggregatedResult {
    std::vector<StockReport> reports;
    double                   avg_score;
};

// ── Analyst actor: receives request, produces report, sends to monitor ──
struct Analyst {
    std::string sector;
    ActorRef monitor;

    void on_receive(necto::Envelope env, necto::ActorContext& ctx) {
        // Run analysis (domain-specific)
        auto report = analyze(sector);
        ctx.send(monitor, serialize(report));
    }

    static auto analyze(const std::string& sector) -> StockReport {
        // ... real analysis here ...
        return {sector, 85.0, "Strong performance in " + sector};
    }
};

// ── Monitor actor: collects N reports, publishes aggregate ──
struct Monitor {
    std::size_t expected;
    std::vector<StockReport> collected;
    std::optional<AggregatedResult> result;

    explicit Monitor(std::size_t n) : expected(n) {}

    void on_receive(necto::Envelope env, necto::ActorContext& /*ctx*/) {
        auto report = deserialize<StockReport>(env.payload);
        collected.push_back(std::move(report));
        if (collected.size() == expected) {
            double sum = 0;
            for (auto& r : collected) sum += r.score;
            result = AggregatedResult{collected, sum / collected.size()};
        }
    }
};

// ── Wire it up ──
int main() {
    necto::ActorSystem sys;

    auto monitor = sys.spawn<Monitor>(3);
    auto energy  = sys.spawn<Analyst>("energy",  monitor);
    auto tech    = sys.spawn<Analyst>("tech",    monitor);
    auto finance = sys.spawn<Analyst>("finance", monitor);

    // Inject sector requests
    sys.inject(energy,  serialize(SectorRequest{"energy"}));
    sys.inject(tech,    serialize(SectorRequest{"tech"}));
    sys.inject(finance, serialize(SectorRequest{"finance"}));

    // Drain to completion
    auto processed = sys.drain();
    // processed == 6 (3 injects → 3 analyst ticks → 3 sends → 3 monitor ticks)

    // Read result from monitor
    auto& mon = static_cast<Monitor&>(sys[monitor].impl());
    assert(mon.result.has_value());
    assert(mon.result->reports.size() == 3);
}
```

### 7.1 Message Flow

```
Round 1:                            Round 2:
  inject → [energy.inbox]            Analyst.energy → send(monitor, report)
  inject → [tech.inbox]              Analyst.tech   → send(monitor, report)
  inject → [finance.inbox]           Analyst.finance→ send(monitor, report)
  tick_all(): processes 3 injects    tick_all(): processes 3 reports
                                     Monitor: collected.size() == 3 → aggregate

drain() returns after 2 rounds, 6 messages total.
```

### 7.2 Connecting to celer::stream

Actors can consume streams internally — an analyst that reads from a celer-mem store:

```cpp
struct StreamingAnalyst {
    celer::Store* store;
    ActorRef monitor;

    void on_receive(necto::Envelope env, necto::ActorContext& ctx) {
        auto req = deserialize<SectorRequest>(env.payload);
        // Pull historical data from celer-mem
        auto stream = store->db("market")
            ->table(req.sector)
            ->stream_scan("2026-04/");
        auto data = celer::stream::collect(*stream);
        // Analyze and forward
        auto report = analyze(req.sector, *data);
        ctx.send(monitor, serialize(report));
    }
};
```

The stream is consumed synchronously within `on_receive`. The actor model provides isolation; the streaming model provides efficient data access. They compose orthogonally.

---

## 8. Build System Changes

### 8.1 New Files

```
include/celer/necto/actor.hpp       # ActorRef, Envelope, Mailbox, ActorContext,
                                    # ActorVTable, ActorHandle, ActorSystem
tests/test_necto.cpp                # Unit + integration tests
```

### 8.2 Modified Files

```
include/celer/celer.hpp             # + necto/actor.hpp include
CMakeLists.txt                      # + test_necto target
```

### 8.3 Dependencies

None. Necto uses only C++23 standard library. No external dependencies.

---

## 9. Scope

### In Scope (This RFC)

- [x] `ActorRef` — uint32_t index handle
- [x] `Envelope` — `{seq, from, to, payload}` value type
- [x] `Mailbox` — MPSC queue with `(from, seq)` dedup
- [x] `ActorContext` — injected send capability
- [x] `Actor` concept — `on_receive(Envelope, ActorContext&)`
- [x] `ActorVTable` — constexpr vtable: on_receive + destroy
- [x] `ActorHandle` — type-erased, move-only, owns actor + mailbox
- [x] `ActorSystem` — flat vector, spawn, deliver, inject, tick, tick_all, drain
- [x] Stock sector analysis demo (3 analysts + 1 monitor)
- [x] Unit tests: spawn, send, receive, dedup, drain, livelock cap

### Out of Scope (Future / Commercial)

- [ ] WAL-backed durable mailboxes (persist Envelope to celer-mem backend, replay on crash)
- [ ] Raft consensus for multi-node actor placement
- [ ] Cross-process IPC (shared memory ring buffers, futex wake)
- [ ] Cross-node networking (TCP/QUIC transport for Envelope)
- [ ] Actor supervision / restart policies
- [ ] Typed channels / topic-based pub/sub
- [ ] Parallel tick (multi-threaded lane partitioning)
- [ ] API key authentication / SDK for external clients (C++, Python, JavaScript)
- [ ] Backpressure / mailbox size limits
- [ ] Actor migration (move actor between systems)

---

## 10. Design Decisions

### 10.1 No Broker — Why

| Approach | Routing cost | Single point of failure | Framework coupling |
|----------|-------------|------------------------|-------------------|
| Central broker (Akka, Erlang) | O(log N) or O(1) hash | Yes — broker crash kills routing | High |
| Supervisor hierarchy (OTP) | O(depth) tree walk | Cascading — supervisor failure propagates | High |
| **Flat vector (Necto)** | **O(1) array index** | **No — system is the vector** | **None** |

The flat vector is the simplest correct thing. Actors address each other by index. There is nothing to crash except the system itself, and if the system is gone, the process is gone.

### 10.2 No Typed Messages — Why

Typed mailboxes (`Mailbox<StockReport>`) would require:
- Template parameter on every actor → viral generics
- Sum types for actors that receive multiple message types → `std::variant` explosion
- Homogeneous mailbox per actor → can't receive different message types

Opaque `vector<char>` payload with edge serialization is simpler, more flexible, and matches the wire format for future cross-process/cross-node extension. The cost is one serialize + one deserialize per message — dominated by mailbox mutex overhead for small payloads.

### 10.3 No on_start / on_stop — Why

Actor lifecycle hooks add framework weight for minimal benefit. An actor that needs initialization does it in its constructor. An actor that needs cleanup does it in its destructor. C++ already has deterministic lifetime semantics — we don't need to reinvent them as framework callbacks.

### 10.4 Dedup Set Growth — Trade-off

The `seen_` set grows without bound. Alternatives:

| Strategy | Pro | Con |
|----------|-----|-----|
| **Unbounded set + periodic trim** (chosen) | Zero false positives, simple | Memory grows with message volume |
| Bloom filter | Constant memory | False positives → dropped valid messages |
| Sliding window per sender | Bounded memory | Complex bookkeeping, fails on out-of-order |
| No dedup | Minimal memory | Duplicate processing, non-idempotent |

Chosen approach: unbounded with `trim_dedup()`. At 48 bytes per entry, 100K entries = ~4.8MB — negligible. The trim threshold is configurable. For long-running systems, call `trim_dedup()` periodically (e.g., every 10K ticks).

---

## 11. Relation to Existing celer-mem Layers

```
┌────────────────────────────────────────────────────────┐
│                    celer::necto (RFC-004)                │
│    ActorSystem → vector<ActorHandle>                    │
│    Actors send/receive via Envelope + Mailbox            │
│                                                          │
│    Actors internally use:                                │
│    ┌──────────────────────────────────────────────────┐  │
│    │  celer::stream (RFC-002)                         │  │
│    │  StreamHandle<T> for data access in on_receive   │  │
│    │  celer::serde for Envelope payload (de)serialize │  │
│    │  celer::Store for backend I/O                    │  │
│    └──────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────┘
```

Necto is orthogonal to the streaming and storage layers. It does not depend on them. But actors naturally compose with them: an actor's `on_receive` can pull from streams, read/write to stores, compress data — using the same APIs as non-actor code.

---

*End of RFC 004*
