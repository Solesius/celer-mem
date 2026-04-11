#include "celer/backend/dynamic.hpp"

namespace celer {

auto load_dynamic_backend(std::string_view /*so_path*/, std::string_view /*config_json*/)
    -> Result<BackendHandle> {
    return std::unexpected(Error{"NotImplemented", "dynamic backend loading not yet wired"});
}

} // namespace celer
