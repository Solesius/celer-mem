#pragma once
/// celer materialization layer (RFC-005) — public umbrella header.
///
/// Pulls in:
///   - codec.hpp        (CPO + FNV-1a + Key/CompositeKey)
///   - store_ref.hpp    (StoreRef<T>, capabilities, Record<T>)
///   - stream.hpp       (RecordStream<T> with where/map/take/inspect/batch)
///   - planner.hpp      (JoinKind, JoinStrategy, JoinOptions, JoinPlanner)
///   - join.hpp         (join() + Joined<L,R> + 5 executors)
///   - materialize.hpp  (materialize() + 5 modes + WatermarkRecord)
///
/// All types live in namespace ``celer::materialization``.

#include "celer/materialization/codec.hpp"
#include "celer/materialization/join.hpp"
#include "celer/materialization/materialize.hpp"
#include "celer/materialization/planner.hpp"
#include "celer/materialization/store_ref.hpp"
#include "celer/materialization/stream.hpp"
