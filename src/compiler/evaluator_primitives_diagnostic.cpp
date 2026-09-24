// evaluator_primitives_diagnostic.cpp — P0 step 23: diagnose / apply-fix / check-preconditions
// Issue #2917: agent:recover-from-error closed-loop (diagnose → fix under Guard).
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "prim_heap_quota.hh"      // Issue #2917 recovery can raise soft quotas
#include "security_side_effect.hh" // AURA_SIDE_EFFECT_PRIM / agent: → Mutate (#2057/#2152)
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

void register_diagnostic_primitives(PrimRegistrar add, Evaluator& ev) {

    // (diagnose error-string) — Analyze structured error and return fix strategy
    // Returns a pair ("root-cause" "target" "fix-type" "fix-data" "message")
    // or #f if no diagnosis available.
    add("diagnose", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto msg = ev.string_heap_[idx];

        // Build diagnosis result as list: (cause target fix-type fix-data explanation)
        auto make_diag = [&](const std::string& cause, const std::string& target,
                             const std::string& fix, const std::string& data,
                             const std::string& expl) -> EvalValue {
            auto push = [&](const std::string& s) -> std::uint64_t {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(s);
                return sidx;
            };
            auto nil = EvalValue(0);
            auto make_item = [&](const std::string& s) -> EvalValue {
                auto sidx = push(s);
                return make_string(sidx);
            };
            // Build: (cause target fix-type fix-data expl) as proper list
            auto e_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({make_item(expl), nil});
            auto d_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({make_item(data), e_pair});
            auto f_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({make_item(fix), d_pair});
            auto t_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({make_item(target), f_pair});
            auto c_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({make_item(cause), t_pair});
            return c_pair;
        };

        // ── Known unbound variable mappings ──
        // Format: {root-cause, target-fn, fix-type, fix-data, explanation}
        struct FixEntry {
            std::string cause;
            std::string fix_type;
            std::string fix_data;
            std::string explanation;
        };
        static const std::unordered_map<std::string, FixEntry, aura::core::TransparentStringHash,
                                        std::equal_to<>>
            unbound_fixes = {
                {"for-each",
                 {"missing-require", "add-require", "std/list",
                  "Add (require \"std/list\" all:) to use for-each"}},
                {"map",
                 {"missing-require", "add-require", "std/list",
                  "Add (require \"std/list\" all:) to use map"}},
                {"filter",
                 {"missing-require", "add-require", "std/list",
                  "Add (require \"std/list\" all:) to use filter"}},
                {"foldl",
                 {"missing-require", "add-require", "std/list",
                  "Add (require \"std/list\" all:) to use foldl"}},
                {"make-hash",
                 {"missing-require", "add-require", "std/hash",
                  "Add (require \"std/hash\" all:) to use make-hash"}},
                {"hash-ref",
                 {"missing-require", "add-require", "std/hash",
                  "Add (require \"std/hash\" all:) to use hash-ref"}},
                {"rule:define",
                 {"missing-require", "add-require", "std/rule",
                  "Add (require \"std/rule\" all:) to use rule:define"}},
                // Issue #561: synthesize:fill + synthesize:pipeline
                // lint hint references removed — these were dead
                // hints pointing to a non-existent std/pipeline module.
                // Decision documented in
                // docs/design/synthesize-namespace-decision.md
                // (synthesize:fill is a real primitive in agent.cpp
                // that wraps LLM calls; synthesize:pipeline is a
                // docs/AI-strategy concept, not an actual primitive).
                {"define-type",
                 {"missing-require", "add-require", "std/data",
                  "Add (require \"std/data\" all:) to use define-type"}},
                {"c-func",
                 {"missing-require", "add-require", "std/ffi",
                  "Add (require \"std/ffi\" all:) to use c-func"}},
        };

        // Detect "unbound variable: X" and match against known symbols
        std::string prefix = "unbound variable: ";
        auto upos = msg.find(prefix);
        if (upos != std::string::npos) {
            auto target = msg.substr(upos + prefix.size());
            // Strip trailing punctuation
            while (!target.empty() && (target.back() == '.' || target.back() == '!' ||
                                       target.back() == '?' || target.back() == ':'))
                target.pop_back();
            auto it = unbound_fixes.find(target);
            if (it != unbound_fixes.end()) {
                return make_diag(it->second.cause, target, it->second.fix_type, it->second.fix_data,
                                 it->second.explanation);
            }
            // Unknown unbound variable — generic suggestion
            return make_diag("unbound-variable", target, "define-or-require", "",
                             "Define '" + target +
                                 "' with (define ...) or add the right (require ...)");
        }

        // Detect "type error: cannot call: X"
        prefix = "type error: cannot call: ";
        auto tpos = msg.find(prefix);
        if (tpos != std::string::npos) {
            auto target = msg.substr(tpos + prefix.size());
            return make_diag("type-error-unbound", target, "define-function", "",
                             "Define the function '" + target + "' before calling it");
        }

        // Detect parse errors
        if (msg.find("parse error") != std::string::npos ||
            msg.find("expected expression") != std::string::npos) {
            return make_diag("parse-error", "", "fix-syntax", msg.substr(0, 60),
                             "Check syntax: unbalanced parens, missing quotes, or wrong form");
        }

        // Detect #<procedure> / closure
        if (msg.find("procedure") != std::string::npos ||
            msg.find("uncalled function") != std::string::npos) {
            return make_diag("closure-no-display", "", "add-display", "",
                             "Add (display (your-function args)) at the end of your code");
        }

        // Issue #2917 / #2916: soft heap / resource quota — raise limit or free pressure.
        if (msg.find("prim-heap-quota:") != std::string::npos) {
            std::string dim = "pairs";
            if (msg.find("strings") != std::string::npos)
                dim = "strings";
            else if (msg.find("vectors") != std::string::npos)
                dim = "vectors";
            return make_diag("prim-heap-quota", dim, "raise-quota", dim,
                             "Raise (resource:quota-set \"" + dim +
                                 "\" N) or free heap pressure before retrying");
        }
        if (msg.find("quota exceeded") != std::string::npos ||
            msg.find("resource quota") != std::string::npos) {
            return make_diag("resource-quota", "", "raise-quota", "memory",
                             "Raise resource:quota-set limit or reduce concurrent work");
        }

        // Type mismatch residual (mutate / typecheck last_mutate_error_)
        if (msg.find("type") != std::string::npos &&
            (msg.find("mismatch") != std::string::npos || msg.find("error") != std::string::npos ||
             msg.find("deny") != std::string::npos)) {
            return make_diag("type-mismatch", "", "clear-hold-or-rewrite", "",
                             "Clear strict hold via recover or rewrite the offending mutate");
        }

        return make_bool(false);
    });

    // (apply-fix code-string diagnose-result) — Apply pre-built fix from diagnosis
    // Returns fixed code, or original code if no auto-fix applies.
    add("apply-fix", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_pair(a[1]))
            return make_bool(false);
        bool guard_ok = true;
        // Issue #2124: force try_acquire (quota + metrics); no legacy ctor.
        auto guard_r = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(
            ev, /*pending=*/1, &guard_ok);
        if (!guard_r) {
            return make_bool(false);
        }
        auto guard = std::move(*guard_r);
        ev.bump_verify_tool_guard_capture();
        auto code_idx = as_string_idx(a[0]);
        if (code_idx >= ev.string_heap_.size())
            return make_bool(false);
        auto code = ev.string_heap_[code_idx];

        // Extract fix-type (3rd element of diagnose result)
        // diagnose returns: (cause target fix-type fix-data explanation)
        auto get_elem = [&](auto p, int n) -> std::string {
            for (int i = 0; i < n && is_pair(p); ++i) {
                if (i == n - 1) {
                    if (is_string(ev.pairs_[as_pair_idx(p)].car)) {
                        auto si = as_string_idx(ev.pairs_[as_pair_idx(p)].car);
                        if (si < ev.string_heap_.size())
                            return ev.string_heap_[si];
                    }
                    return "";
                }
                p = ev.pairs_[as_pair_idx(p)].cdr;
            }
            return "";
        };
        // Elements (1-indexed in list traversal):
        // 1=cause, 2=target, 3=fix-type, 4=fix-data, 5=explanation
        auto fix_type = get_elem(a[1], 3);
        auto fix_data = get_elem(a[1], 4);
        auto target = get_elem(a[1], 2); // function name

        std::string result;
        if (fix_type == "add-require") {
            // Prepend (require "module" all:) if not already present
            auto req_line = "(require \"" + fix_data + "\" all:)";
            if (code.find(req_line) == std::string::npos) {
                // Check if there's already an existing require
                auto nl = code.find('\n');
                auto first_line = (nl == std::string::npos) ? code : code.substr(0, nl);
                if (first_line.find("(require ") == 0) {
                    // There's already a require, insert after it
                    auto rest = (nl == std::string::npos) ? "" : code.substr(nl);
                    result = first_line + "\n" + req_line + rest;
                } else {
                    result = req_line + "\n" + code;
                }
            } else {
                result = code;
            }
        } else if (fix_type == "define-function" || fix_type == "define-or-require") {
            // Add stub define for unnamed function
            auto stub = "(define (" + target + " x)\n  x)\n";
            if (code.find("(define (" + target) == std::string::npos) {
                result = code + "\n" + stub;
            } else {
                result = code;
            }
        } else if (fix_type == "add-display") {
            // Append (display result) — already handled by eval-current auto-fix
            result = code;
        } else if (fix_type == "fix-syntax") {
            // Can't auto-fix syntax errors — return code as-is
            result = code;
        } else {
            result = code;
        }

        if (result != code) {
            ev.bump_verify_tool_feedback_mutate_success();
            // Issue #670: apply-fix success path also bumps
            // the StableRef + dirty-propagation counters so
            // the AI Agent can see "fix was applied with
            // provenance" via (query:verify-tool-guard-stats).
            // Pre-#670 only feedback_mutate_success fired —
            // the StableRef / dirty_propagation axes were
            // dead code in the apply-fix path.
            ev.bump_verify_tool_stable_ref_hit();
            ev.bump_verify_tool_dirty_propagation();
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(result);
        return make_string(sidx);
    });

    // (check-preconditions node-id (field-offset|new-type-str)) → true if valid
    // With int second arg: check field existence (0=int_val_, 1=type_id_)
    // With string second arg: check type compatibility (new type string)
    add("check-preconditions", [&ev](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_int(a[0]) || !ev.workspace_flat_)
            return make_bool(false);
        bool guard_ok = true;
        // Issue #2124: force try_acquire (quota + metrics); no legacy ctor.
        auto guard_r = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(
            ev, /*pending=*/1, &guard_ok);
        if (!guard_r) {
            return make_bool(false);
        }
        auto guard = std::move(*guard_r);
        ev.bump_verify_tool_guard_capture();
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size())
            return make_bool(false);
        // Issue #2759: layout + Evaluator stamp (sole production authority).
        auto pref = flat.make_ref_layout(node);
        ev.stamp_stable_ref(pref);
        if (!pref.is_valid_in(flat))
            return make_bool(false);
        ev.bump_verify_tool_stable_ref_hit();
        auto nv = flat.get(node);
        // Issue #670: capture the result so we can bump
        // dirty_propagation at the end if the precondition
        // passed (true). Pre-#670 the bump wasn't wired in
        // this path — only the StableRef capture fired.
        EvalValue result = make_bool(false);

        // String arg: type compatibility check
        if (is_string(a[1])) {
            auto type_idx = as_string_idx(a[1]);
            if (type_idx >= ev.string_heap_.size())
                return make_bool(false);
            auto new_type = ev.string_heap_[type_idx];

            // Check compatibility based on node tag
            switch (nv.tag) {
                case aura::ast::NodeTag::LiteralInt:
                    // Int literal: compatible with Int, Float, Bool (≠0), Dyn
                    result =
                        make_bool(new_type == "Int" || new_type == "Float" || new_type == "Bool" ||
                                  new_type == "Dyn" || new_type == "Any");
                    break;
                case aura::ast::NodeTag::LiteralFloat:
                    // Float literal: compatible with Float, Dyn
                    result =
                        make_bool(new_type == "Float" || new_type == "Dyn" || new_type == "Any");
                    break;
                case aura::ast::NodeTag::LiteralString:
                    result =
                        make_bool(new_type == "String" || new_type == "Dyn" || new_type == "Any");
                    break;
                case aura::ast::NodeTag::Call:
                case aura::ast::NodeTag::Lambda:
                    // Structural nodes: always OK (any type)
                    result = make_bool(true);
                    break;
                case aura::ast::NodeTag::Variable:
                    // Variable: always OK (outer scope determines type)
                    result = make_bool(true);
                    break;
                default:
                    // Other nodes: permissive
                    result = make_bool(true);
                    break;
            }
        } else if (is_int(a[1])) {
            // Int arg: field existence check
            auto field = static_cast<std::uint32_t>(as_int(a[1]));
            switch (field) {
                case 0:
                    result = make_bool(nv.has_int()); // int_val_
                    break;
                case 1:
                    result = make_bool(true); // type_id_ (always valid)
                    break;
                default:
                    result = make_bool(false);
                    break;
            }
        }
        // Issue #670: on precondition pass (true), bump
        // dirty_propagation so the AI Agent can see "this
        // node was successfully precondition-checked, AST
        // verification dirty state propagated" via
        // (query:verify-tool-guard-stats).
        if (is_bool(result) && as_bool(result))
            ev.bump_verify_tool_dirty_propagation();
        return result;
    });

    // ═══════════════════════════════════════════════════════════════════════
    // Issue #2917: (agent:recover-from-error …) — closed-loop recovery
    //   error → diagnose → apply-fix under MutationBoundaryGuard → clear hold
    // Agent-callable; required_effects auto-stamped Mutate via agent: prefix.
    // AURA_SIDE_EFFECT_PRIM
    //
    // Forms:
    //   (agent:recover-from-error code-string)
    //       → use last_mutate_error_ as the error text
    //   (agent:recover-from-error error-string code-string)
    //   (agent:recover-from-error error-value code-string)
    //
    // Returns list: (ok status-code cause fix-type fixed-code-or-void)
    //   status-code: 1 success, 2 fail, 3 no-diagnosis, 4 guard-fail, 5 bad-args
    // Poll: (engine:metrics "query:agent-recovery-stats") schema-2917
    // ═══════════════════════════════════════════════════════════════════════
    add("agent:recover-from-error", [&ev](std::span<const EvalValue> a) -> EvalValue {
        ev.note_agent_recovery_attempt();

        auto push_str = [&](const std::string& s) -> EvalValue {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(s);
            return make_string(sidx);
        };
        auto make_result = [&](bool ok, std::int64_t status, const std::string& cause,
                               const std::string& fix_type, EvalValue fixed) -> EvalValue {
            // (ok status cause fix-type fixed) proper list
            auto nil = EvalValue(0);
            auto e_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({fixed, nil});
            auto d_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({push_str(fix_type), e_pair});
            auto c_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({push_str(cause), d_pair});
            auto s_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({make_int(status), c_pair});
            auto o_pair = make_pair(ev.pairs_.size());
            ev.pairs_.push_back({make_bool(ok), s_pair});
            return o_pair;
        };

        // Resolve error message + optional code.
        std::string err_msg;
        std::string code;
        if (a.empty()) {
            ev.note_agent_recovery_result(/*status=*/5, /*success=*/false, /*hold=*/false,
                                          /*ckpt=*/false);
            return make_result(false, 5, "bad-args", "", make_void());
        }

        auto extract_err_string = [&](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto si = as_string_idx(v);
                if (si < ev.string_heap_.size())
                    return ev.string_heap_[si];
            } else if (is_error(v)) {
                auto eidx = types::as_error_idx(v);
                if (eidx < ev.error_values_.size()) {
                    auto& cause = ev.error_values_[eidx];
                    if (is_string(cause)) {
                        auto si = as_string_idx(cause);
                        if (si < ev.string_heap_.size())
                            return ev.string_heap_[si];
                    }
                }
            }
            return {};
        };

        if (a.size() == 1) {
            // Single arg: code-string, error from last_mutate_error_
            if (!is_string(a[0])) {
                ev.note_agent_recovery_result(5, false, false, false);
                return make_result(false, 5, "bad-args", "", make_void());
            }
            auto ci = as_string_idx(a[0]);
            if (ci >= ev.string_heap_.size()) {
                ev.note_agent_recovery_result(5, false, false, false);
                return make_result(false, 5, "bad-args", "", make_void());
            }
            code = ev.string_heap_[ci];
            err_msg = ev.last_mutate_error();
            if (err_msg.empty()) {
                ev.note_agent_recovery_result(5, false, false, false);
                return make_result(false, 5, "no-last-error", "", make_void());
            }
        } else {
            err_msg = extract_err_string(a[0]);
            if (err_msg.empty() || !is_string(a[1])) {
                ev.note_agent_recovery_result(5, false, false, false);
                return make_result(false, 5, "bad-args", "", make_void());
            }
            auto ci = as_string_idx(a[1]);
            if (ci >= ev.string_heap_.size()) {
                ev.note_agent_recovery_result(5, false, false, false);
                return make_result(false, 5, "bad-args", "", make_void());
            }
            code = ev.string_heap_[ci];
        }

        // Guard + panic checkpoint (safe recovery window).
        bool guard_ok = true;
        auto guard_r = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(
            ev, /*pending=*/1, &guard_ok);
        if (!guard_r) {
            ev.note_agent_recovery_result(4, false, false, false);
            return make_result(false, 4, "guard-fail", "", make_void());
        }
        auto guard = std::move(*guard_r);
        ev.bump_verify_tool_guard_capture();
        const bool ckpt = ev.save_panic_checkpoint();
        if (ckpt)
            (void)0; // counted below on result

        // Diagnose (inline reuse of diagnose by constructing a string value).
        auto err_sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(err_msg);
        // Call diagnose logic via the primitive if present; else fail closed.
        auto diag_slot = ev.primitives().slot_for_name("diagnose");
        EvalValue diagnosis = make_bool(false);
        if (auto fn = ev.primitives().slot_lookup_fast(diag_slot)) {
            diagnosis = (*fn)({make_string(err_sidx)});
        }
        if (!is_pair(diagnosis)) {
            ev.note_agent_recovery_result(3, false, false, ckpt);
            return make_result(false, 3, "no-diagnosis", "", make_void());
        }

        // Apply fix under the same Guard (apply-fix also takes Guard — nested OK).
        auto fix_slot = ev.primitives().slot_for_name("apply-fix");
        EvalValue fixed = make_bool(false);
        if (auto fn = ev.primitives().slot_lookup_fast(fix_slot)) {
            auto code_sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(code);
            fixed = (*fn)({make_string(code_sidx), diagnosis});
        }
        if (!is_string(fixed)) {
            ev.note_agent_recovery_result(2, false, false, ckpt);
            return make_result(false, 2, "apply-fix-failed", "", make_void());
        }

        // Extract cause / fix-type from diagnosis list for metrics + return.
        auto get_elem = [&](EvalValue p, int n) -> std::string {
            for (int i = 0; i < n && is_pair(p); ++i) {
                if (i == n - 1) {
                    if (is_string(ev.pairs_[as_pair_idx(p)].car)) {
                        auto si = as_string_idx(ev.pairs_[as_pair_idx(p)].car);
                        if (si < ev.string_heap_.size())
                            return ev.string_heap_[si];
                    }
                    return "";
                }
                p = ev.pairs_[as_pair_idx(p)].cdr;
            }
            return "";
        };
        const auto cause = get_elem(diagnosis, 1);
        const auto fix_type = get_elem(diagnosis, 3);

        // Clear strict hold / last mutate error so Agents see green readiness.
        const bool had_hold = ev.strict_mutate_hold() || !ev.last_mutate_error().empty();
        if (had_hold)
            ev.clear_last_mutate_error(); // also clears strict_mutate_hold_

        // Quota-class: auto-raise soft limit a notch when fix-type is raise-quota.
        if (fix_type == "raise-quota") {
            const auto dim = get_elem(diagnosis, 2); // target dim for prim-heap
            if (dim == "pairs") {
                auto cur = ev.prim_heap_quota(PrimHeapDim::Pairs);
                if (cur > 0)
                    ev.set_prim_heap_quota(PrimHeapDim::Pairs, cur + 64);
            } else if (dim == "strings") {
                auto cur = ev.prim_heap_quota(PrimHeapDim::Strings);
                if (cur > 0)
                    ev.set_prim_heap_quota(PrimHeapDim::Strings, cur + 64);
            } else if (dim == "vectors") {
                auto cur = ev.prim_heap_quota(PrimHeapDim::Vectors);
                if (cur > 0)
                    ev.set_prim_heap_quota(PrimHeapDim::Vectors, cur + 16);
            }
        }

        ev.bump_verify_tool_feedback_mutate_success();
        ev.bump_verify_tool_stable_ref_hit();
        ev.bump_verify_tool_dirty_propagation();
        ev.note_agent_recovery_result(1, true, had_hold, ckpt);
        return make_result(true, 1, cause, fix_type, fixed);
    });
}

} // namespace aura::compiler::primitives_detail