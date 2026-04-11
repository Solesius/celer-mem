#pragma once

/// celer-mem — Composite-pattern embedded memory framework for C++23
/// Khalil Warren · MIT License

// Core
#include "celer/core/result.hpp"
#include "celer/core/types.hpp"
#include "celer/core/resource_stack.hpp"
#include "celer/core/composite.hpp"
#include "celer/core/dispatch.hpp"
#include "celer/core/tree_builder.hpp"

// Backend
#include "celer/backend/concept.hpp"
#include "celer/backend/rocksdb.hpp"
#include "celer/backend/dynamic.hpp"

// Schema
#include "celer/schema/binding.hpp"

// Serde
#include "celer/serde/reflect.hpp"
#include "celer/serde/codec.hpp"

// API
#include "celer/api/result_set.hpp"
#include "celer/api/table_ref.hpp"
#include "celer/api/db_ref.hpp"
#include "celer/api/store.hpp"
#include "celer/api/global.hpp"
