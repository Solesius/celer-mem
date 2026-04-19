#pragma once
/// celer::necto — Brokerless actor model with constexpr prototype vtable.
///
/// Contract:
///   Every actor implements the PrototypableActor concept:
///     on_receive(Envelope, ActorContext&)  +  std::copy_constructible.
///   The vtable is full (on_receive, clone, destroy) or it doesn't exist.
///   No nullptr clone slot. No runtime "is prototypable?" check.
///   Hard compile error if the actor isn't copyable.
///
///   IPC is pull-based: each actor owns a StreamHandle<Envelope> (pull side
///   of a bounded MPSC ChannelImpl). Senders push into ChannelPush<Envelope>.
///   Dedup is per-actor (DedupSet attached to the handle).
///
///   Actors are addressed by ActorRef (uint32 index) on hot path and by
///   name via FlatSymbolTable for management/routing.
///
///   AgentLifecycle (Active/Dormant) is orthogonal to role.
///   ClusterId scopes an actor to a sub-cluster or the dormant pool.
///
/// Pattern:
///   Type-erased ActorHandle with constexpr 3-slot vtable (mirrors StreamHandle<T>).
///   ActorSystem owns vector<ActorHandle> and drives tick/drain loops.
///   spawn_n() clones a prototype N times — fresh channel/ref/name per clone.

#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "celer/core/stream.hpp"
#include "celer/core/symbol_table.hpp"
#include "celer/necto/channel.hpp"

namespace celer::necto {

class ActorSystem;

// ════════════════════════════════════════════════════════════════
// ActorRef — lightweight index handle into the actor vector
// ════════════════════════════════════════════════════════════════

struct ActorRef {
    uint32_t index{UINT32_MAX};

    [[nodiscard]] constexpr bool valid() const noexcept { return index != UINT32_MAX; }
    constexpr bool operator==(const ActorRef&) const noexcept = default;
};

// ════════════════════════════════════════════════════════════════
// ClusterId — scoping handle for sub-clusters
// ════════════════════════════════════════════════════════════════

struct ClusterId {
    uint32_t index{UINT32_MAX};

    [[nodiscard]] constexpr bool valid() const noexcept { return index != UINT32_MAX; }
    constexpr bool operator==(const ClusterId&) const noexcept = default;
};

// ════════════════════════════════════════════════════════════════
// AgentLifecycle — orthogonal to role (what you are vs whether you tick)
// ════════════════════════════════════════════════════════════════

enum class AgentLifecycle : uint8_t { Active, Dormant };

// ════════════════════════════════════════════════════════════════
// Envelope — the unit of actor communication
// ════════════════════════════════════════════════════════════════

struct Envelope {
    uint64_t          seq{0};      // per-sender monotonic
    ActorRef          from{};      // sender ref
    ActorRef          to{};        // receiver ref
    std::vector<char> payload;     // opaque — actor decides encoding
};

// ════════════════════════════════════════════════════════════════
// DedupSet — per-actor idempotent delivery guard
// ════════════════════════════════════════════════════════════════

class DedupSet {
    struct Key {
        uint32_t from;
        uint64_t seq;
        bool operator==(const Key&) const noexcept = default;
    };

    struct Hash {
        auto operator()(const Key& k) const noexcept -> std::size_t {
            return std::hash<uint64_t>{}(k.seq) ^
                   (std::hash<uint32_t>{}(k.from) * 2654435761u);
        }
    };

    std::unordered_set<Key, Hash> seen_;

public:
    /// Returns true if this envelope is new (not a duplicate).
    auto check_and_insert(const Envelope& env) -> bool {
        return seen_.insert(Key{env.from.index, env.seq}).second;
    }

    void trim(std::size_t max_entries = 100'000) {
        if (seen_.size() > max_entries) seen_.clear();
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t { return seen_.size(); }
    void clear() noexcept { seen_.clear(); }
};

// ════════════════════════════════════════════════════════════════
// ActorContext — injected send capability during on_receive
// ════════════════════════════════════════════════════════════════

class ActorContext {
    friend class ActorSystem;

    ActorRef self_;
    ClusterId cluster_;
    uint64_t next_seq_{0};
    std::function<bool(Envelope)> deliver_;

    ActorContext(ActorRef self, ClusterId cluster, uint64_t seq,
                 std::function<bool(Envelope)> deliver)
        : self_(self), cluster_(cluster), next_seq_(seq),
          deliver_(std::move(deliver)) {}

public:
    [[nodiscard]] auto self() const noexcept -> ActorRef { return self_; }
    [[nodiscard]] auto cluster() const noexcept -> ClusterId { return cluster_; }

    /// Send payload to another actor. Idempotent at the receiver.
    auto send(ActorRef to, std::vector<char> payload) -> bool {
        if (!deliver_) return false;
        Envelope env{next_seq_++, self_, to, std::move(payload)};
        return deliver_(std::move(env));
    }
};

// ════════════════════════════════════════════════════════════════
// Concepts — Actor is on_receive; PrototypableActor adds copy
// ════════════════════════════════════════════════════════════════

template <typename A>
concept Actor = requires(A& a, Envelope env, ActorContext& ctx) {
    { a.on_receive(std::move(env), ctx) };
};

template <typename A>
concept PrototypableActor = Actor<A> && std::copy_constructible<A>;

// ════════════════════════════════════════════════════════════════
// ActorVTable — constexpr 3-slot vtable (on_receive, clone, destroy)
// ════════════════════════════════════════════════════════════════
//
// The vtable is full or it doesn't exist.
// vtable_for<A> requires PrototypableActor — hard compile error otherwise.
// No nullable clone slot. No runtime "is prototypable?" check.

struct ActorVTable {
    void  (*on_receive)(void* self, Envelope env, ActorContext& ctx);
    void* (*clone)(const void* self);
    void  (*destroy)(void* self);
};

template <PrototypableActor A>
inline constexpr ActorVTable vtable_for = {
    .on_receive = [](void* self, Envelope env, ActorContext& ctx) {
        static_cast<A*>(self)->on_receive(std::move(env), ctx);
    },
    .clone = [](const void* self) -> void* {
        return new A(*static_cast<const A*>(self));
    },
    .destroy = [](void* self) { delete static_cast<A*>(self); },
};

// ════════════════════════════════════════════════════════════════
// ActorHandle — type-erased, owning actor wrapper
// ════════════════════════════════════════════════════════════════
//
// Owns:
//   impl*                       → the behavior
//   const ActorVTable*          → constexpr vtable (on_receive, clone, destroy)
//   StreamHandle<Envelope>      → inbox pull side (from ChannelImpl)
//   ChannelPush<Envelope>       → inbox push side (shared with senders)
//   DedupSet                    → idempotent delivery guard
//   ActorRef                    → index in flat vector
//   std::string name            → registered in FlatSymbolTable
//   uint64_t next_seq           → per-actor monotonic counter
//   AgentLifecycle lifecycle    → Active or Dormant
//   ClusterId cluster           → sub-cluster assignment

class ActorHandle {
    void*                          impl_{nullptr};
    const ActorVTable*             vt_{nullptr};
    StreamHandle<Envelope>         inbox_;
    ChannelPush<Envelope>          push_;
    DedupSet                       dedup_;
    ActorRef                       self_{};
    std::string                    name_;
    uint64_t                       next_seq_{0};
    AgentLifecycle                 lifecycle_{AgentLifecycle::Active};
    ClusterId                      cluster_{};

    friend class ActorSystem;

public:
    ActorHandle() = default;
    ~ActorHandle() { if (impl_ && vt_) vt_->destroy(impl_); }

    ActorHandle(ActorHandle&& o) noexcept
        : impl_(std::exchange(o.impl_, nullptr))
        , vt_(std::exchange(o.vt_, nullptr))
        , inbox_(std::move(o.inbox_))
        , push_(std::move(o.push_))
        , dedup_(std::move(o.dedup_))
        , self_(o.self_)
        , name_(std::move(o.name_))
        , next_seq_(o.next_seq_)
        , lifecycle_(o.lifecycle_)
        , cluster_(o.cluster_) {}

    ActorHandle& operator=(ActorHandle&& o) noexcept {
        if (this != &o) {
            if (impl_ && vt_) vt_->destroy(impl_);
            impl_      = std::exchange(o.impl_, nullptr);
            vt_        = std::exchange(o.vt_, nullptr);
            inbox_     = std::move(o.inbox_);
            push_      = std::move(o.push_);
            dedup_     = std::move(o.dedup_);
            self_      = o.self_;
            name_      = std::move(o.name_);
            next_seq_  = o.next_seq_;
            lifecycle_ = o.lifecycle_;
            cluster_   = o.cluster_;
        }
        return *this;
    }

    ActorHandle(const ActorHandle&) = delete;
    ActorHandle& operator=(const ActorHandle&) = delete;

    [[nodiscard]] auto ref() const noexcept -> ActorRef { return self_; }
    [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }
    [[nodiscard]] auto lifecycle() const noexcept -> AgentLifecycle { return lifecycle_; }
    [[nodiscard]] auto cluster() const noexcept -> ClusterId { return cluster_; }
    [[nodiscard]] auto push_handle() -> ChannelPush<Envelope>& { return push_; }
    [[nodiscard]] auto push_handle() const -> const ChannelPush<Envelope>& { return push_; }
};

// ════════════════════════════════════════════════════════════════
// ActorSystem — flat vector of actors, FlatSymbolTable name registry
// ════════════════════════════════════════════════════════════════

class ActorSystem {
    std::vector<ActorHandle>  actors_;
    std::vector<std::string>  name_keys_;
    FlatSymbolTable           names_;
    bool                      names_dirty_{true};
    std::size_t               channel_capacity_{1024};

    void rebuild_names() {
        if (!names_dirty_) return;
        std::vector<std::size_t> values;
        values.reserve(name_keys_.size());
        for (std::size_t i = 0; i < name_keys_.size(); ++i)
            values.push_back(i);
        names_ = FlatSymbolTable::build(name_keys_, values);
        names_dirty_ = false;
    }

    auto make_handle_for(void* impl, const ActorVTable* vt, std::string name) -> ActorRef {
        auto idx = static_cast<uint32_t>(actors_.size());
        ActorRef ref{idx};

        auto [push, inbox] = make_channel<Envelope>(channel_capacity_);

        ActorHandle h;
        h.impl_  = impl;
        h.vt_    = vt;
        h.inbox_ = std::move(inbox);
        h.push_  = std::move(push);
        h.self_  = ref;
        h.name_  = std::move(name);

        actors_.push_back(std::move(h));
        name_keys_.push_back(actors_.back().name_);
        names_dirty_ = true;
        return ref;
    }

public:
    ActorSystem() = default;
    explicit ActorSystem(std::size_t channel_cap) : channel_capacity_(channel_cap) {}

    /// Spawn a named actor. Returns its ref.
    template <PrototypableActor A, typename... Args>
    auto spawn(std::string name, Args&&... args) -> ActorRef {
        auto* impl = new A(std::forward<Args>(args)...);
        return make_handle_for(impl, &vtable_for<A>, std::move(name));
    }

    /// Clone-spawn from an existing actor (Prototype pattern).
    /// Fresh channel, ref, name, dedup — behavior deep-copied via vtable.clone.
    auto clone_spawn(ActorRef proto, std::string name) -> ActorRef {
        if (proto.index >= actors_.size()) return ActorRef{};
        auto& src = actors_[proto.index];
        void* cloned = src.vt_->clone(src.impl_);
        return make_handle_for(cloned, src.vt_, std::move(name));
    }

    /// Clone-spawn N copies from prototype. Names: "{prefix}_{i}".
    auto spawn_n(ActorRef proto, uint32_t count, std::string_view prefix) -> std::vector<ActorRef> {
        std::vector<ActorRef> refs;
        refs.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            std::string name{prefix};
            name += '_';
            name += std::to_string(i);
            refs.push_back(clone_spawn(proto, std::move(name)));
        }
        return refs;
    }

    /// Deliver an envelope. Dedup at receiver. Respects dormant policy (drop).
    auto deliver(Envelope env) -> bool {
        if (env.to.index >= actors_.size()) return false;
        auto& h = actors_[env.to.index];
        if (h.lifecycle_ == AgentLifecycle::Dormant) return false;  // drop policy
        if (!h.dedup_.check_and_insert(env)) return false;          // idempotent
        return h.push_.push(std::move(env));
    }

    /// External inject — send into the system from outside (no sender actor).
    auto inject(ActorRef to, std::vector<char> payload) -> bool {
        static constexpr ActorRef external{UINT32_MAX - 1};
        return deliver(Envelope{0, external, to, std::move(payload)});
    }

    /// Tick one actor: pull inbox chunk, call on_receive for each envelope.
    /// Skips dormant actors.
    auto tick(ActorRef ref) -> std::size_t {
        if (ref.index >= actors_.size()) return 0;
        auto& h = actors_[ref.index];
        if (h.lifecycle_ == AgentLifecycle::Dormant) return 0;

        auto result = h.inbox_.pull();
        if (!result || !result->has_value()) return 0;
        auto& chunk = **result;
        if (chunk.empty()) return 0;

        std::size_t processed = 0;
        ActorContext ctx(h.self_, h.cluster_, h.next_seq_,
            [this](Envelope env) -> bool { return deliver(std::move(env)); });

        for (std::size_t i = 0; i < chunk.size(); ++i) {
            // Chunk is const — copy envelope out for processing
            Envelope msg;
            msg.seq     = chunk[i].seq;
            msg.from    = chunk[i].from;
            msg.to      = chunk[i].to;
            msg.payload = chunk[i].payload;
            h.vt_->on_receive(h.impl_, std::move(msg), ctx);
            ++processed;
        }
        h.next_seq_ = ctx.next_seq_;
        return processed;
    }

    /// Tick all active actors. Returns total messages processed.
    auto tick_all() -> std::size_t {
        std::size_t total = 0;
        for (uint32_t i = 0; i < actors_.size(); ++i) {
            total += tick(ActorRef{i});
        }
        return total;
    }

    /// Drain all channels until quiescent. Bounded to prevent livelock.
    auto drain(std::size_t max_rounds = 10'000) -> std::size_t {
        std::size_t total = 0;
        for (std::size_t round = 0; round < max_rounds; ++round) {
            auto n = tick_all();
            if (n == 0) break;
            total += n;
        }
        return total;
    }

    /// Resolve actor by name via FlatSymbolTable. O(1) amortized.
    auto resolve(std::string_view name) -> std::optional<ActorRef> {
        rebuild_names();
        auto idx = names_.find(name);
        if (!idx) return std::nullopt;
        return ActorRef{static_cast<uint32_t>(*idx)};
    }

    /// Direct push to an actor's channel (bypass deliver/dedup — for system-level ops).
    auto channel_push(ActorRef ref, Envelope env) -> bool {
        if (ref.index >= actors_.size()) return false;
        return actors_[ref.index].push_.push(std::move(env));
    }

    /// Drain an actor's channel (dismiss path).
    void channel_drain(ActorRef ref) {
        if (ref.index < actors_.size()) {
            actors_[ref.index].push_.drain();
            actors_[ref.index].dedup_.clear();
        }
    }

    /// Lifecycle mutations.
    void set_lifecycle(ActorRef ref, AgentLifecycle lc) {
        if (ref.index < actors_.size()) actors_[ref.index].lifecycle_ = lc;
    }

    void set_cluster(ActorRef ref, ClusterId cid) {
        if (ref.index < actors_.size()) actors_[ref.index].cluster_ = cid;
    }

    auto operator[](ActorRef ref) -> ActorHandle& { return actors_[ref.index]; }
    auto operator[](ActorRef ref) const -> const ActorHandle& { return actors_[ref.index]; }

    [[nodiscard]] auto size() const -> std::size_t { return actors_.size(); }
    [[nodiscard]] auto empty() const -> bool { return actors_.empty(); }
};

} // namespace celer::necto
