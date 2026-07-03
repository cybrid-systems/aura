// evaluator_security.cpp — Issue #676 sandbox + audit implementation.

module;

#include <chrono>
#include <cstring>

#include "security_capabilities.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;

namespace aura::compiler {

using namespace security;

bool Evaluator::has_capability(std::string_view needed) const noexcept {
    if (!sandbox_mode_)
        return true;
    if (needed.empty())
        return false;
    const auto matches = [&](const std::string& held) {
        return held == needed || held == kCapWildcard;
    };
    for (const auto& cap : granted_capabilities_) {
        if (matches(cap))
            return true;
    }
    for (const auto& layer : capability_stack_) {
        for (const auto& cap : layer) {
            if (matches(cap))
                return true;
        }
    }
    return false;
}

void Evaluator::grant_capability(std::string cap) {
    for (const auto& existing : granted_capabilities_) {
        if (existing == cap)
            return;
    }
    granted_capabilities_.push_back(std::move(cap));
}

void Evaluator::emit_mutation_audit(std::uint32_t nodes_changed, std::uint32_t epoch_delta,
                                    std::string_view op, ast::NodeId target_node) noexcept {
    const auto seq = mutation_audit_seq_.fetch_add(1, std::memory_order_relaxed);
    auto& slot = mutation_audit_ring_[seq % kMutationAuditRingSize];
    slot.seq = seq;
    slot.timestamp_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    slot.fiber_id = static_cast<std::int64_t>(aura_fiber_current_id());
    slot.nodes_changed = nodes_changed;
    slot.epoch_delta = epoch_delta;
    slot.target_node = static_cast<std::uint32_t>(target_node);
    const auto n = std::min(op.size(), sizeof(slot.op) - 1);
    std::memcpy(slot.op, op.data(), n);
    slot.op[n] = '\0';
    mutation_audit_total_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace aura::compiler