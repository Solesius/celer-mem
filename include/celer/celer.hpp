#pragma once

/// celer-mem — Composite-pattern embedded memory framework for C++23
/// Khalil Warren · Apache-2.0 License

// Core
#include "celer/core/result.hpp"
#include "celer/core/types.hpp"
#include "celer/core/arc_buf.hpp"
#include "celer/core/aws_credentials.hpp"
#include "celer/core/symbol_table.hpp"
#include "celer/core/stream.hpp"
#include "celer/core/compression.hpp"
#include "celer/core/poll_result.hpp"
#include "celer/core/scheduler.hpp"
#include "celer/core/async_stream.hpp"
#include "celer/core/resource_stack.hpp"
#include "celer/core/composite.hpp"
#include "celer/core/dispatch.hpp"
#include "celer/core/tree_builder.hpp"

// Backend
#include "celer/backend/concept.hpp"
#include "celer/backend/rocksdb.hpp"
#include "celer/backend/sqlite.hpp"
#include "celer/backend/qpdf.hpp"
#include "celer/backend/s3.hpp"
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
