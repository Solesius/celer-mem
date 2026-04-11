#pragma once

#include <string>
#include <string_view>

#include "celer/backend/concept.hpp"
#include "celer/core/result.hpp"

namespace celer {

/// Load a backend from a shared object (.so) via C-ABI contract.
/// The .so must export: extern "C" celer_backend_vtable* celer_create_backend(const char* config_json);
[[nodiscard]] auto load_dynamic_backend(std::string_view so_path, std::string_view config_json)
    -> Result<BackendHandle>;

} // namespace celer
