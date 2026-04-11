#pragma once
/// Core data types shared across the dispatch and backend layers.
/// Extracted to break the include cycle: composite → concept → dispatch → composite.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace celer {

struct KVPair {
    std::string key;
    std::string value;
};

struct BatchOp {
    enum class Kind { put, del };
    Kind                        kind;
    std::string                 cf_name;
    std::string                 key;
    std::optional<std::string>  value;
};

/// C-style visitor for zero-copy foreach scans.
/// Called once per key/value pair; cursor lifetime is internal.
using ScanVisitor = void(*)(void* ctx, std::string_view key, std::string_view value);

} // namespace celer
