# RFC 0001: Cross-Store Joins and Materialization

Status: Draft  
Target Project: `celer-mem`  
Format: Repository Markdown specification  
Primary Goal: Fast cross-store joins and materializations using typed monadic lambdas over the existing `celer-mem` storage interface.

---

## 1. Abstract

This RFC defines a join and materialization layer for `celer-mem`.

The purpose is to let `celer-mem` join records across heterogeneous stores without forcing all data into one physical database engine.

The core idea is:

```cpp
Store<A>
    .scan(...)
    .where(...)
    .join(Store<B>, left_key, right_key)
    .map(...)
    .materialize(Store<C>);
```

The join system is not SQL-first.

The join system is a typed C++ execution primitive built from composable transformations over record streams.

The system should support:

- in-memory stores
- RocksDB-backed stores
- append-log stores
- vector metadata stores
- graph edge stores
- future custom stores

The interface must make cross-store read models feel native.

---

## 2. Problem

Agent memory is not one table.

A real agent memory system contains many related data shapes:

- chat turns
- sessions
- tool calls
- tool results
- documents
- chunks
- embeddings
- entities
- facts
- graph edges
- scores
- summaries
- provenance records
- cache entries

A useful agent needs to correlate these records.

Examples:

- join chat turns to sessions
- join documents to chunks
- join chunks to embeddings
- join retrieval hits to source documents
- join tool results to tool-call provenance
- join facts to entity nodes
- join graph edges to source observations
- join prompt cache entries to model metadata

Without a real join primitive, every materializer becomes bespoke glue.

That creates:

- duplicate lookup loops
- hand-written fanout logic
- inconsistent error handling
- repeated serialization work
- no shared optimization point
- no shared batching policy
- no common metrics
- no common planner
- no stable abstraction for future stores

The goal is to define one fast common execution surface.

---

## 3. Design Principles

### 3.1 Joins are typed, not string-based

A join should be expressed with lambdas or function objects:

```cpp
auto view =
    turns.join(
        sessions,
        [](const ChatTurn& t) { return t.session_id; },
        [](const Session& s) { return s.id; }
    );
```

The compiler should help.

The caller should not build string predicates unless an adapter explicitly supports that.

### 3.2 Stores expose capabilities

Not every backend supports every access pattern.

A store may support:

- full scan
- prefix scan
- point get
- batch get
- ordered range scan
- secondary index lookup
- approximate nearest-neighbor lookup
- append-only replay
- snapshot reads

The join planner should ask stores what they can do.

### 3.3 Cross-store does not mean slow

The default planner should avoid nested full scans when possible.

Preferred plans:

- hash join when both sides are streamable
- index nested-loop join when one side has point or batch lookup
- merge join when both sides provide ordered scans
- vector-assisted join when one side is retrieval-driven
- bounded nested-loop only for very small cardinalities

### 3.4 Materialization is first-class

The output of a join should be materializable into another store.

Materialization should support:

- replace
- upsert
- append
- compare-and-swap
- tombstone
- incremental update
- rebuild from scratch

### 3.5 Streaming is mandatory

The system must not require loading entire stores into memory.

The API should allow lazy streaming, bounded buffering, and backpressure.

### 3.6 Snapshots matter

A join over multiple stores should be explicit about consistency.

Supported consistency modes:

- `best_effort`
- `left_snapshot`
- `right_snapshot`
- `independent_snapshots`
- `coordinated_snapshot`
- `monotonic_materialization`

The first implementation may support only `best_effort` and `independent_snapshots`, but the interface must leave room for stronger modes.

---

## 4. Definitions

### 4.1 Store

A `Store<T>` is a typed collection of records of type `T`.

A store may be backed by memory, RocksDB, a file log, a graph index, or another physical engine.

Minimum conceptual interface:

```cpp
template<class T>
class Store {
public:
    Result<RecordStream<T>> scan(ScanOptions options = {});
    Result<std::optional<T>> get(Key key);
    Result<BatchResult<T>> get_many(std::span<const Key> keys);
    StoreCapabilities capabilities() const;
};
```

### 4.2 RecordStream

A `RecordStream<T>` is a lazy stream of typed records.

It should support:

- `map`
- `where`
- `flat_map`
- `take`
- `batch`
- `inspect`
- `join`
- `materialize`

Conceptual interface:

```cpp
template<class T>
class RecordStream {
public:
    template<class Predicate>
    RecordStream<T> where(Predicate pred);

    template<class U, class Fn>
    RecordStream<U> map(Fn fn);

    template<class U, class LeftKeyFn, class RightKeyFn>
    JoinedStream<T, U> join(
        Store<U>& right,
        LeftKeyFn left_key,
        RightKeyFn right_key
    );

    template<class Sink>
    MaterializeResult materialize(
        Sink& sink,
        MaterializeOptions options = {}
    );
};
```

### 4.3 Join

A join combines records from two inputs according to compatible keys.

Initial join types:

- inner join
- left join
- semi join
- anti join

Future join types:

- full outer join
- temporal join
- window join
- nearest-neighbor join
- graph-expand join

### 4.4 Materialization

Materialization writes the result of a stream into a target store.

Example:

```cpp
turns
    .join(sessions, turn_session_id, session_id)
    .map(make_session_turn_view)
    .materialize(session_turn_views, MaterializeMode::upsert);
```

### 4.5 Monadic lambda

In this RFC, a monadic lambda means a composable operation that transforms a value inside an execution context.

The execution context may contain:

- lazy stream state
- `Result<T>` error propagation
- cancellation
- batching
- allocator state
- metrics
- snapshot state
- execution plan state

Example:

```cpp
stream
    .where([](const Turn& t) { return t.role == "user"; })
    .map([](const Turn& t) { return UserTurnView{...}; })
    .materialize(out);
```

This keeps the API readable while allowing the engine to optimize execution underneath.

---

## 5. Core API Shape

### 5.1 Store interface

A minimal store should expose:

```cpp
template<class T>
class IStore {
public:
    using value_type = T;

    virtual StoreId id() const = 0;
    virtual StoreCapabilities capabilities() const = 0;

    virtual Result<RecordStream<T>> scan(const ScanOptions& options) = 0;

    virtual Result<std::optional<T>> get(const Key& key) = 0;

    virtual Result<BatchGetResult<T>> get_many(std::span<const Key> keys) = 0;

    virtual VoidResult put(const Key& key, const T& value) = 0;

    virtual VoidResult erase(const Key& key) = 0;

    virtual Result<SnapshotToken> snapshot() = 0;

    virtual ~IStore() = default;
};
```

This shape is conceptual. The concrete implementation should remain compatible with the existing `TableRef`, `ResultSet<T>`, `StorageBackend`, and `Result<T>` design.

### 5.2 StoreCapabilities

Capabilities should be explicit.

```cpp
struct StoreCapabilities {
    bool supports_full_scan = false;
    bool supports_prefix_scan = false;
    bool supports_ordered_scan = false;
    bool supports_point_get = false;
    bool supports_batch_get = false;
    bool supports_secondary_index = false;
    bool supports_snapshot = false;
    bool supports_transactions = false;
    bool supports_tombstones = false;
    bool supports_vector_search = false;
    bool supports_graph_expand = false;

    size_t preferred_batch_size = 256;
    size_t max_batch_size = 4096;

    CostHint scan_cost;
    CostHint point_get_cost;
    CostHint batch_get_cost;
};
```

### 5.3 Join options

```cpp
enum class JoinKind {
    inner,
    left,
    semi,
    anti
};

enum class JoinStrategy {
    auto_plan,
    hash_join,
    index_nested_loop,
    batch_index_nested_loop,
    merge_join,
    nested_loop
};

struct JoinOptions {
    JoinKind kind = JoinKind::inner;
    JoinStrategy strategy = JoinStrategy::auto_plan;

    size_t memory_budget_bytes = 64 * 1024 * 1024;
    size_t batch_size = 512;

    bool allow_full_scan_right = false;
    bool allow_nested_loop = false;

    ConsistencyMode consistency = ConsistencyMode::best_effort;
};
```

### 5.4 Joined record shape

The default joined record should be explicit.

```cpp
template<class L, class R>
struct Joined {
    L left;
    std::optional<R> right;
};
```

For inner joins, `right` is always present.

For left joins, `right` may be empty.

For semi joins, only `left` may be emitted.

For anti joins, only `left` may be emitted.

The API may provide separate optimized record shapes later.

### 5.5 Basic join example

```cpp
auto joined =
    chat_turns
        .scan()
        .join(
            sessions,
            [](const ChatTurn& t) { return t.session_id; },
            [](const Session& s) { return s.id; },
            JoinOptions{
                .kind = JoinKind::inner,
                .strategy = JoinStrategy::auto_plan
            }
        );
```

### 5.6 Materialization example

```cpp
auto result =
    chat_turns
        .scan()
        .join(sessions, turn_session_id, session_id)
        .map([](const Joined<ChatTurn, Session>& row) {
            return SessionTurnView{
                .session_id = row.left.session_id,
                .turn_id = row.left.id,
                .role = row.left.role,
                .content = row.left.content,
                .created_at_ms = row.left.created_at_ms,
                .session_title = row.right->title
            };
        })
        .materialize(
            session_turn_views,
            MaterializeOptions{
                .mode = MaterializeMode::upsert
            }
        );
```

---

## 6. Join Planner

### 6.1 Planner inputs

The planner receives:

- left stream metadata
- right store capabilities
- key selector metadata if available
- estimated cardinality
- estimated key cardinality
- memory budget
- join type
- consistency mode
- ordering information
- available indexes
- batch size constraints

### 6.2 Strategy selection

The planner should prefer strategies in this order:

1. Merge join if both sides are ordered by join key.
2. Batch index nested-loop if the right side supports `get_many`.
3. Index nested-loop if the right side supports `get`.
4. Hash join if the smaller side can fit in memory.
5. Nested-loop only if explicitly allowed or cardinality is known tiny.

### 6.3 Index nested-loop join

Best when:

- left side is streamed
- right side supports `get(key)`
- left cardinality is moderate
- right lookups are cheap

Execution:

```text
for each left row:
    key = left_key(row)
    right = right_store.get(key)
    emit if join condition passes
```

Problem:

- one point lookup per left row can be too slow

Mitigation:

- batch keys
- deduplicate keys
- use `get_many`
- preserve output ordering if requested

### 6.4 Batch index nested-loop join

Best default for RocksDB-backed joins.

Execution:

1. Read left rows in batches.
2. Compute right keys.
3. Deduplicate keys.
4. Call `right.get_many(keys)`.
5. Build temporary key-to-right map.
6. Emit joined rows.

This should be the default strategy when the right side supports batch lookup.

### 6.5 Hash join

Best when:

- one input can fit in memory
- both sides can be scanned
- right side lacks index lookup
- materialization requires high throughput

Execution:

1. Build hash table from smaller side.
2. Stream larger side.
3. Probe hash table.
4. Emit matches.

This requires memory accounting.

If memory budget is exceeded, the engine should either:

- fail
- spill
- switch strategy if allowed

The first implementation may fail with a clear error.

### 6.6 Merge join

Best when both inputs can scan in key order.

Execution:

1. Stream both inputs ordered by key.
2. Advance the side with the lower key.
3. Emit matching groups.

This is excellent for RocksDB prefix/range layouts.

### 6.7 Nested loop

Nested loop is dangerous.

It should require either:

- `allow_nested_loop = true`
- or proven tiny cardinality

The engine should reject accidental large nested-loop plans.

Example error:

```text
JoinPlanRejected:
    nested-loop join would require full scan of right store for each left row.
    Set allow_nested_loop=true only for bounded inputs.
```

---

## 7. Materialization

### 7.1 Materialize modes

```cpp
enum class MaterializeMode {
    append,
    upsert,
    replace_all,
    compare_and_swap,
    tombstone_missing
};
```

### 7.2 Materialize options

```cpp
struct MaterializeOptions {
    MaterializeMode mode = MaterializeMode::upsert;

    size_t batch_size = 512;
    bool fsync = false;
    bool collect_metrics = true;
    bool stop_on_error = true;

    std::optional<std::string> materialization_id;
    ConsistencyMode consistency = ConsistencyMode::best_effort;
};
```

### 7.3 Materialization contract

Materialization should return structured output:

```cpp
struct MaterializeResult {
    size_t read_count = 0;
    size_t write_count = 0;
    size_t skipped_count = 0;
    size_t error_count = 0;

    Duration elapsed;
    std::vector<MaterializeError> errors;
    ExecutionMetrics metrics;
};
```

### 7.4 Incremental materialization

Future support should allow:

- since-watermark replay
- append-log replay
- dirty-key tracking
- dependency tracking
- materialized-view invalidation

Example future shape:

```cpp
materializer
    .from(watermark)
    .join(...)
    .write_to(view_store)
    .commit_watermark();
```

---

## 8. Key Selection

### 8.1 Key selectors

A key selector maps a record to a join key.

```cpp
auto turn_session_id = [](const ChatTurn& t) {
    return Key::from_string(t.session_id);
};

auto session_id = [](const Session& s) {
    return Key::from_string(s.id);
};
```

### 8.2 Composite keys

Composite keys should be supported.

Example:

```cpp
auto user_doc_key = [](const AccessGrant& g) {
    return CompositeKey::of(g.user_id, g.document_id);
};
```

Composite keys must have stable serialization.

### 8.3 Key requirements

Join keys must be:

- deterministic
- comparable for equality
- hashable for hash join
- serializable for index lookup
- optionally orderable for merge join

---

## 9. Error Model

Errors should be values, not random exceptions across execution.

Core errors:

- `StoreUnavailable`
- `SnapshotUnavailable`
- `UnsupportedCapability`
- `JoinPlanRejected`
- `SerializationError`
- `DeserializationError`
- `KeySelectionError`
- `MaterializationWriteError`
- `MemoryBudgetExceeded`
- `Cancelled`
- `ConsistencyViolation`

The API may expose exceptions at the outer boundary only if the existing public style changes. Internally, execution should preserve structured `Result<T>` errors.

---

## 10. Cancellation and Backpressure

Long joins must be cancellable.

Execution context should carry:

- cancellation token
- memory budget
- batch budget
- deadline
- metrics sink

Conceptual shape:

```cpp
ExecutionContext ctx;
ctx.cancel_token = token;
ctx.memory_budget_bytes = 64_MB;
ctx.deadline = now + 5s;

stream.materialize(out, options, ctx);
```

Backpressure should be natural through pull-based streams or bounded queues.

The first implementation should prefer pull-based iteration.

---

## 11. Snapshots and Consistency

### 11.1 Consistency modes

```cpp
enum class ConsistencyMode {
    best_effort,
    independent_snapshots,
    coordinated_snapshot,
    monotonic_materialization
};
```

### 11.2 `best_effort`

No snapshot guarantee.

Fastest.

Useful for caches, approximate retrieval, and agent scratch memory.

### 11.3 `independent_snapshots`

Each store provides its own snapshot token.

The join runs against stable per-store views.

This does not guarantee global atomicity, but it prevents each individual store from changing during scan.

### 11.4 `coordinated_snapshot`

Future mode.

Requires stores to share a transaction or snapshot coordinator.

### 11.5 `monotonic_materialization`

Future mode.

Materialized views should never go backward in observed watermark.

Useful for append-log-driven projections.

---

## 12. Performance Requirements

### 12.1 First implementation targets

The first implementation should optimize for:

- RocksDB-backed right-side lookup
- memory-backed left-side scan
- batch get
- key deduplication
- typed map after join
- upsert materialization

### 12.2 Required fast path

This should be the default fast path:

```text
left.scan()
    -> batch left rows
    -> compute right keys
    -> deduplicate keys
    -> right.get_many(keys)
    -> join in memory
    -> map output
    -> batch write materialization
```

### 12.3 Avoid repeated serialization

The planner should avoid serializing the same key repeatedly inside a batch.

Batch join execution should cache:

- extracted key
- serialized key
- right lookup result

### 12.4 Memory budgeting

Any plan that builds an in-memory hash table must account for memory.

Hash join must respect `memory_budget_bytes`.

If memory is exceeded, return `MemoryBudgetExceeded` unless spilling is implemented.

### 12.5 Metrics

Every join should optionally collect:

- `rows_left_read`
- `rows_right_read`
- `keys_extracted`
- `keys_deduplicated`
- `point_get_count`
- `batch_get_count`
- `rows_emitted`
- `rows_materialized`
- `bytes_read_estimate`
- `bytes_written_estimate`
- `planner_strategy`
- `elapsed_ms`

---

## 13. Agent Memory Examples

### 13.1 Session turn view

Input stores:

- `Store<ChatTurn> chat_turns`
- `Store<Session> sessions`

Output store:

- `Store<SessionTurnView> session_turn_views`

Pipeline:

```cpp
chat_turns
    .scan()
    .join(sessions, turn_session_id, session_id)
    .map(make_session_turn_view)
    .materialize(session_turn_views);
```

### 13.2 Retrieval hit hydration

Input stores:

- `Store<RetrievalHit> hits`
- `Store<DocumentChunk> chunks`
- `Store<Document> documents`

Pipeline:

```cpp
hits
    .scan()
    .join(chunks, hit_chunk_id, chunk_id)
    .map(make_hit_chunk)
    .join(documents, hit_chunk_document_id, document_id)
    .map(make_hydrated_retrieval_result);
```

### 13.3 Tool result provenance

Input stores:

- `Store<ToolResult> tool_results`
- `Store<ToolCall> tool_calls`
- `Store<ChatTurn> turns`

Pipeline:

```cpp
tool_results
    .scan()
    .join(tool_calls, result_call_id, call_id)
    .join(turns, call_turn_id, turn_id)
    .map(make_tool_provenance_view)
    .materialize(tool_provenance_views);
```

### 13.4 Graph edge expansion

Input stores:

- `Store<EntityEdge> edges`
- `Store<EntityNode> nodes`

Pipeline:

```cpp
edges
    .scan(Prefix(entity_id))
    .join(nodes, edge_target_id, node_id)
    .map(make_neighbor_view);
```

---

## 14. Minimum Viable Implementation

### 14.1 Phase 1

Implement:

- `IStore<T>` or equivalent typed adapter over `TableRef`
- `RecordStream<T>` or streamed `ResultSet<T>` successor
- `where`
- `map`
- inner join
- left join
- batch index nested-loop join
- materialize upsert
- materialize append
- basic metrics
- basic errors

Required store capability:

- `scan`
- `get`
- `get_many`
- `put`

### 14.2 Phase 2

Add:

- hash join
- semi join
- anti join
- memory budget enforcement
- independent snapshots
- replace-all materialization
- tombstone-missing materialization

### 14.3 Phase 3

Add:

- merge join
- secondary-index metadata
- incremental materialization
- watermarks
- graph-expand join
- vector metadata hydration
- spillable hash join

---

## 15. Reference API Sketch

This is not final code. This is the intended shape.

```cpp
template<class T>
class StoreRef {
public:
    Result<RecordStream<T>> scan(ScanOptions options = {});
    Result<std::optional<T>> get(const Key& key);
    Result<BatchGetResult<T>> get_many(std::span<const Key> keys);
    StoreCapabilities capabilities() const;
};

template<class T>
class RecordStream {
public:
    template<class Predicate>
    RecordStream<T> where(Predicate predicate);

    template<class U, class Mapper>
    RecordStream<U> map(Mapper mapper);

    template<class R, class LK, class RK>
    RecordStream<Joined<T, R>> join(
        StoreRef<R> right,
        LK left_key,
        RK right_key,
        JoinOptions options = {}
    );

    template<class OutStore>
    MaterializeResult materialize(
        OutStore& out,
        MaterializeOptions options = {}
    );
};
```

---

## 16. Reference Execution Sketch: Batch Index Join

Pseudo-code:

```text
while left_batch = left.next_batch(batch_size):

    extracted = []

    for left in left_batch:
        key = left_key(left)
        extracted.push({ left, key })

    unique_keys = dedupe(extracted.keys)

    right_rows = right.get_many(unique_keys)

    right_map = map_by_key(right_rows, right_key)

    for item in extracted:
        match = right_map.find(item.key)

        if match:
            emit Joined{ item.left, match.value }

        else if join_kind == left:
            emit Joined{ item.left, nullopt }

        else:
            skip
```

---

## 17. Non-Goals

This RFC does not require:

- SQL parser
- distributed joins
- network query engine
- global ACID transactions
- cost-perfect optimizer
- full relational algebra
- arbitrary expression compiler
- query language syntax

This is intentionally smaller and sharper.

---

## 18. Security and Safety

### 18.1 Untrusted lambdas

In normal C++ usage, lambdas are trusted code.

If future scripting is introduced, script predicates and mappers must be sandboxed.

### 18.2 Prompt injection

Materialized tool results may contain adversarial content.

The join layer should not interpret record content as instructions.

Records are data.

Any agent consuming materialized views must preserve provenance and trust boundaries.

### 18.3 Resource exhaustion

The planner must prevent accidental unbounded joins.

Required protections:

- reject unplanned nested loops
- enforce batch size
- enforce memory budget
- expose cancellation
- expose deadline
- count emitted rows
- optionally cap output rows

---

## 19. Naming

Suggested public names:

- `StoreRef<T>`
- `RecordStream<T>`
- `Joined<L, R>`
- `JoinOptions`
- `JoinKind`
- `JoinStrategy`
- `MaterializeOptions`
- `MaterializeMode`
- `MaterializeResult`
- `StoreCapabilities`
- `ExecutionContext`

Suggested internal names:

- `JoinPlanner`
- `JoinPlan`
- `BatchIndexJoinExecutor`
- `HashJoinExecutor`
- `MergeJoinExecutor`
- `MaterializeExecutor`
- `KeyExtractor`
- `KeyCodec`
- `SnapshotScope`

---

## 20. Acceptance Criteria

This RFC is accepted when `celer-mem` can express and execute:

- `Store<ChatTurn>` joined with `Store<Session>`
- `Store<RetrievalHit>` joined with `Store<DocumentChunk>`
- join result mapped into a typed read model
- read model materialized into another store
- planner chooses batch index nested-loop when the right side supports `get_many`
- accidental full nested-loop joins are rejected by default
- metrics report selected strategy and row counts
- API remains readable in normal C++ application code

---

## 21. Example Final Target Usage

```cpp
AgentMemory mem{"./celer-agent.db"};

auto turns = mem.store<ChatTurn>("chat_turns");
auto sessions = mem.store<Session>("sessions");
auto views = mem.store<SessionTurnView>("session_turn_views");

auto result =
    turns.scan()
        .where([](const ChatTurn& t) {
            return !t.session_id.empty();
        })
        .join(
            sessions,
            [](const ChatTurn& t) {
                return Key::from_string(t.session_id);
            },
            [](const Session& s) {
                return Key::from_string(s.id);
            },
            JoinOptions{
                .kind = JoinKind::inner,
                .strategy = JoinStrategy::auto_plan,
                .batch_size = 512,
                .allow_nested_loop = false
            }
        )
        .map([](const Joined<ChatTurn, Session>& row) {
            return SessionTurnView{
                .session_id = row.left.session_id,
                .turn_id = row.left.id,
                .role = row.left.role,
                .content = row.left.content,
                .created_at_ms = row.left.created_at_ms,
                .session_title = row.right->title
            };
        })
        .materialize(
            views,
            MaterializeOptions{
                .mode = MaterializeMode::upsert,
                .batch_size = 512
            }
        );
```

---

## 22. Design Position

This is not a database cosplay layer.

This is a projection engine for agent memory.

The important primitive is not query.

The important primitive is:

```text
derive typed knowledge from multiple stores,
do it fast,
do it incrementally later,
and make the result materializable.
```

That is the line.

`celer-mem` should make this feel like writing normal C++ while hiding the ugly storage fanout behind a planner.
