// agent_name_table.h — Issue #2078: per-Evaluator orch agent name bookkeeping.
//
// Header-only with inline method definitions. The orch headers transitively
// pull in serve/fiber.h; this header is NOT included from evaluator.ixx's
// global fragment (only from implementation TUs and tests) so orch types
// stay out of the evaluator module interface — avoids ODR collisions with
// service.ixx's direct include of serve/fiber.h (g_current_fiber thread_local
// weak symbol generation).
//
// evaluator.ixx forward-declares AgentNameTable and holds it as
// `std::unique_ptr<AgentNameTable> agent_names_;` (forward decl suffices for
// unique_ptr at member declaration). Evaluator's destructor in
// evaluator_ctor.cpp includes this header so the unique_ptr destruction is
// emitted with AgentNameTable complete.
//
// Replaces the TU-local static OrchAgentNameTable that lived in
// evaluator_primitives_agent.cpp — multiple CompilerService instances
// collided on the same agent name and tests were non-hermetic.

#ifndef AURA_COMPILER_AGENT_NAME_TABLE_H
#define AURA_COMPILER_AGENT_NAME_TABLE_H

#include "orch/orch.h" // aura::orch::AgentHandle
#include "core/transparent_string_hash.hh"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aura::compiler {

// Per-Evaluator orch agent name table (Issue #2078). Header-only so the
// implementation does not need a .cpp file (avoids C++20 modules
// "declarations in both fragments" conflict when the .h is included from
// evaluator.ixx's global fragment + a .cpp in the same module's purview).
//
// Issue #3216 identity plane: this is the name-table plane (Aura
// orch:spawn-agent / orch:agent-join). Distinct from AgentScope::handles_
// (scope-handle) and directory_snapshot (directory). Not a process-global
// table — one AgentNameTable per Evaluator. Cross-Evaluator handoff uses
// observation-only HandoffToken (#3148), not a merge of this table.
// Issue #3442: message prims (orch:agent-send / recv / ask / agent-join)
// resolve this table first, then AgentScope::find on the same Evaluator.
// Fallback is resolve-only — this table never auto-puts scope handles
// (AgentHandle is move-only; reservation stays with the scope). Same-name
// in both planes: name-table wins.
struct AgentNameTable {
    AgentNameTable()
        : impl_(std::make_unique<Impl>()) {}
    AgentNameTable(const AgentNameTable&) = delete;
    AgentNameTable& operator=(const AgentNameTable&) = delete;
    AgentNameTable(AgentNameTable&&) noexcept = default;
    AgentNameTable& operator=(AgentNameTable&&) noexcept = default;
    ~AgentNameTable() = default;

    // Returns the stored slot on accept, nullptr on typed deny.
    //
    // Issue #3467: same-name put over a slot that still owes Reclaimed
    // cleanup (must_wait_reclaimed / reclaimed_deferred_cleanup — the
    // production auto-wait Timeout posture of #3012/#3220) is DENIED.
    // The old move-assign ran ~AgentHandle / move-assign cleanup
    // (finish_reclaimed_cleanup_on_dtor) on the pending handle, releasing
    // the arena reservation while the body may still be live
    // (reclaimed_dtor_under_account_total, #3297). Fail closed: the
    // pending slot is left untouched; the denied handle is destroyed by
    // the caller (its own flags are false on the fresh-spawn path, so
    // its destructor is an idempotent no-op; #2661 mailbox / body-stack
    // are never touched). Clean / done slots still replace — prior
    // behavior preserved (#2078 "same-name spawn overrides prior").
    // Cost when both flags are false: two bool loads, no atomic (AC2).
    aura::orch::AgentHandle* put(aura::orch::AgentHandle&& h) {
        std::lock_guard<std::mutex> lock(impl_->mu_);
        auto name = h.name.empty() ? ("agent-" + std::to_string(h.id)) : h.name;
        h.name = name;
        auto it = impl_->agents_.find(name);
        if (it != impl_->agents_.end()) {
            // Issue #3467: typed deny — do not move-assign over a slot
            // that still owes Reclaimed cleanup. Caller keeps `h`.
            if (it->second.must_wait_reclaimed || it->second.reclaimed_deferred_cleanup)
                return nullptr;
            it->second = std::move(h);
            return &it->second;
        }
        auto [ins, _] = impl_->agents_.emplace(std::move(name), std::move(h));
        return &ins->second;
    }

    aura::orch::AgentHandle* find(const std::string& name) {
        std::lock_guard<std::mutex> lock(impl_->mu_);
        auto it = impl_->agents_.find(name);
        return it == impl_->agents_.end() ? nullptr : &it->second;
    }

    // Snapshot for cleanup at ~Evaluator. Caller owns the returned vector;
    // AgentHandle destructors release arena reservation. The map is
    // cleared so the destructor doesn't double-release.
    std::vector<aura::orch::AgentHandle> drain_for_cleanup() {
        std::lock_guard<std::mutex> lock(impl_->mu_);
        std::vector<aura::orch::AgentHandle> out;
        out.reserve(impl_->agents_.size());
        for (auto& [name, handle] : impl_->agents_) {
            (void)name;
            out.push_back(std::move(handle));
        }
        impl_->agents_.clear();
        return out;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(impl_->mu_);
        return impl_->agents_.size();
    }

private:
    struct Impl {
        // mutable so const observability methods (size()) can lock without
        // discarding qualifiers — the mutex protects agents_, not the
        // table's own identity.
        mutable std::mutex mu_;
        std::unordered_map<std::string, aura::orch::AgentHandle, aura::core::TransparentStringHash,
                           std::equal_to<>>
            agents_;
    };
    std::unique_ptr<Impl> impl_;
};

} // namespace aura::compiler

#endif // AURA_COMPILER_AGENT_NAME_TABLE_H