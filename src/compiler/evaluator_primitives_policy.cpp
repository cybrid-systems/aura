// evaluator_primitives_policy.cpp — P0 step 20: memory-policy / capability primitives
// extracted from Evaluator constructor.

module;

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

void register_policy_primitives(PrimRegistrar add, Evaluator& ev) {

    // (set-memory-policy hash) — Configure the auto-governance policy
    // for memory pressure. The hash may contain any of:
    //   "auto-gc":              #t / #f       ; default #f
    //   "warn-pct":             int (0-100)   ; default 80
    //   "critical-pct":         int (0-100)   ; default 95
    //   "sample-every":         int (>= 1)    ; default 1000
    //   "cooldown-evals":       int (>= 1)    ; default 5000
    //   "recent-gc-temp-window": int (>= 1)   ; default 100
    // Returns the previous policy as a hash. Pass #f to disable
    // auto-governance (resets to defaults).
    add("set-memory-policy", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Snapshot the current policy to return as "previous".
        auto prev = ev.memory_policy_;
        // Reset to defaults first; then apply overrides from the hash.
        ev.memory_policy_ = Evaluator::MemoryPolicy{};

        if (a.size() >= 1 && is_hash(a[0])) {
            auto hidx = as_hash_idx(a[0]);
            if (hidx < g_hash_tables.size() && g_hash_tables[hidx]) {
                auto* ht = g_hash_tables[hidx];
                // The hash stores keys as the encoded EvalValue (int64) of
                // the key string at the time the hash was built. The current
                // ev.string_heap_ may have a different interning. So we have
                // to compare by content: for each slot, decode the key
                // back to a string and compare to the target.
                auto hget = [&](const std::string& k) -> EvalValue {
                    for (std::uint64_t i = 0; i < ht->capacity; ++i) {
                        if (ht->metadata()[i] == 0xFF)
                            continue;
                        EvalValue kev(ht->keys()[i]);
                        if (is_string(kev)) {
                            auto kidx = as_string_idx(kev);
                            if (kidx < ev.string_heap_.size() && ev.string_heap_[kidx] == k) {
                                return EvalValue(ht->values()[i]);
                            }
                        }
                    }
                    return make_void();
                };
                auto try_int = [&](const std::string& k, int& out) {
                    auto v = hget(k);
                    if (is_int(v)) {
                        out = static_cast<int>(as_int(v));
                        return true;
                    }
                    return false;
                };
                auto try_bool = [&](const std::string& k, bool& out) {
                    auto v = hget(k);
                    if (is_bool(v)) {
                        out = as_bool(v);
                        return true;
                    }
                    return false;
                };
                int v_i = 0;
                bool v_b = false;
                if (try_bool("auto-gc", v_b))
                    ev.memory_policy_.auto_gc = v_b;
                if (try_int("warn-pct", v_i))
                    ev.memory_policy_.warn_pct = v_i;
                if (try_int("critical-pct", v_i))
                    ev.memory_policy_.critical_pct = v_i;
                if (try_int("sample-every", v_i)) {
                    ev.memory_policy_.sample_every = static_cast<std::size_t>(v_i);
                }
                if (try_int("cooldown-evals", v_i)) {
                    ev.memory_policy_.cooldown_evals = static_cast<std::size_t>(v_i);
                }
                if (try_int("recent-gc-temp-window", v_i)) {
                    ev.memory_policy_.recent_gc_temp_window = static_cast<std::size_t>(v_i);
                }
            }
        }
        // If #f was passed (or empty), the policy stays at defaults.

        // Reset cooldown so the new policy starts fresh.
        ev.last_auto_gc_eval_depth_ = 0;
        ev.sample_counter_ = 0;
        ev.last_warn_level_.clear();

        return ev.build_policy_hash(prev);
    });

    // (get-memory-policy) — Return the current policy as a hash.
    add("get-memory-policy",
                    [&ev](const auto&) -> EvalValue { return ev.build_policy_hash(ev.memory_policy_); });

    // ── Capability primitives (with-capability / capability? / check-capability) ──

    add("with-capability", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            auto es = ev.string_heap_.size();
            ev.string_heap_.push_back(
                "with-capability: first argument must be a string or list of strings");
            auto err_idx = ev.error_values_.size();
            ev.error_values_.push_back(make_string(es));
            return make_error(err_idx);
        }
        if (a.size() < 2) {
            auto es = ev.string_heap_.size();
            ev.string_heap_.push_back("with-capability: requires at least 2 args");
            auto err_idx = ev.error_values_.size();
            ev.error_values_.push_back(make_string(es));
            return make_error(err_idx);
        }
        auto cap_val = a[0];
        std::vector<std::string> caps;
        if (types::is_string(cap_val)) {
            auto sidx = types::as_string_idx(cap_val);
            if (sidx < ev.string_heap_.size())
                caps.push_back(ev.string_heap_[sidx]);
        } else if (types::is_pair(cap_val)) {
            auto cidx = types::as_pair_idx(cap_val);
            while (cidx < ev.pairs_.size()) {
                auto& p = ev.pairs_[cidx];
                if (types::is_string(p.car)) {
                    auto sidx2 = types::as_string_idx(p.car);
                    if (sidx2 < ev.string_heap_.size())
                        caps.push_back(ev.string_heap_[sidx2]);
                }
                if (types::is_int(p.cdr) && types::as_int(p.cdr) == 0)
                    break;
                if (types::is_pair(p.cdr))
                    cidx = types::as_pair_idx(p.cdr);
                else
                    break;
            }
        }
        // Push capability context
        ev.capability_stack_.push_back(caps);
        // Evaluate body expression (the last arg)
        auto body = a[1];
        EvalValue result = make_void();
        if (types::is_closure(body) && ev.workspace_flat_ && ev.workspace_pool_) {
            auto cid = types::as_closure_id(body);
            auto it = ev.closures_.find(cid);
            if (it != ev.closures_.end() && it->second.body_id != ast::NULL_NODE)
                result =
                    ev.eval_flat(*ev.workspace_flat_, *ev.workspace_pool_, it->second.body_id, ev.top_env())
                        .value_or(make_void());
        } else {
            result = body;
        }
        // Pop capability context
        if (!ev.capability_stack_.empty())
            ev.capability_stack_.pop_back();
        return result;
    });

    add("capability?",
                    [](const auto& a) -> EvalValue { return types::make_bool(false); });

    add("check-capability", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            auto es = ev.string_heap_.size();
            ev.string_heap_.push_back("check-capability: first argument must be a string");
            auto err_idx = ev.error_values_.size();
            ev.error_values_.push_back(make_string(es));
            return make_error(err_idx);
        }
        auto sidx = types::as_string_idx(a[0]);
        std::string needed;
        if (sidx < ev.string_heap_.size())
            needed = ev.string_heap_[sidx];
        // Check each capability context in reverse order for proper scoping
        for (auto it = ev.capability_stack_.rbegin(); it != ev.capability_stack_.rend(); ++it) {
            for (auto& c : *it) {
                if (c == needed || c == "*") {
                    return types::make_bool(true);
                }
            }
        }
        return types::make_bool(false);
    });

    add("capability-stack", [&ev](const auto&) -> EvalValue {
        // Collect all unique caps from stack
        std::vector<std::string> caps;
        for (auto& layer : ev.capability_stack_) {
            for (auto& cap : layer) {
                bool dup = false;
                for (auto& c : caps)
                    if (c == cap) {
                        dup = true;
                        break;
                    }
                if (!dup)
                    caps.push_back(cap);
            }
        }
        // Build list from BACK to FRONT (append to head)
        EvalValue result = make_void(); // '()
        for (int i = static_cast<int>(caps.size()) - 1; i >= 0; --i) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(caps[i]);
            auto new_pair_idx = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(new_pair_idx);
        }
        return result;
    });

}

} // namespace aura::compiler::primitives_detail
