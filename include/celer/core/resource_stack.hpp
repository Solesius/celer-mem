#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "celer/core/result.hpp"

namespace celer {

struct ResourceEntry {
    void*         resource;
    void        (*destroy)(void*);
    std::string   label;
};

class ResourceStack {
public:
    ResourceStack() = default;

    ResourceStack(ResourceStack&& o) noexcept
        : stack_(std::move(o.stack_)) {}

    auto operator=(ResourceStack&& o) noexcept -> ResourceStack& {
        if (this != &o) {
            teardown();
            stack_ = std::move(o.stack_);
        }
        return *this;
    }

    ResourceStack(const ResourceStack&)            = delete;
    auto operator=(const ResourceStack&) -> ResourceStack& = delete;

    ~ResourceStack() { teardown(); }

    auto push(std::string label, void* resource, void(*destroy)(void*)) -> void {
        stack_.push_back(ResourceEntry{resource, destroy, std::move(label)});
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return stack_.size();
    }

    /// Teardown in reverse order. Called by destructor; safe to call explicitly.
    auto teardown() -> VoidResult {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            if (it->destroy && it->resource) {
                it->destroy(it->resource);
            }
        }
        stack_.clear();
        return {};
    }

private:
    std::vector<ResourceEntry> stack_;
};

} // namespace celer
