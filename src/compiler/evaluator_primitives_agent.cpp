// evaluator_primitives_agent.cpp — P0 step 18: auto-evolve / synthesize / intend / strategy
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
// Issue #444: CompilerMetrics is the central observability
// struct; the strategy pheromone counters live there.
// observability_metrics.h is a plain header (not a
// module), so we include it directly here in the module
// preamble (avoids the import-only restriction on .h).
#include "observability_metrics.h"
#include "hash_meta.h" // FNV constants (#901)

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

namespace {
    std::vector<std::pair<std::string, std::string>> g_template_patterns;
    std::vector<std::vector<std::string>> g_template_params;
} // namespace

void register_auto_evolve_primitives(PrimRegistrar add, Evaluator& ev) {

    // Issue #97 Action 2: Auto-evolve closed loop
    // (auto-evolve-once detect-fn fix-fn) → runs one cycle:
    //   1. calls detect-fn → list of "gap" records
    //   2. for each gap, calls fix-fn gap → #t if fixed
    //   3. returns the number of fixes
    add("auto-evolve-once", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_closure(a[0]) || !is_closure(a[1]))
            return make_int(0);
        auto detect_cid = as_closure_id(a[0]);
        auto fix_cid = as_closure_id(a[1]);
        auto detect_result = ev.apply_closure(detect_cid, {});
        if (!detect_result)
            return make_int(0);
        EvalValue current = *detect_result;
        std::int64_t fixed = 0;
        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= ev.pairs_.size())
                break;
            auto gap = ev.pairs_[idx].car;
            auto fix_result = ev.apply_closure(fix_cid, {gap});
            if (fix_result) {
                if (is_bool(*fix_result) && as_bool(*fix_result))
                    ++fixed;
            }
            current = ev.pairs_[idx].cdr;
        }
        return make_int(fixed);
    });

    // (auto-evolve-loop "interval" detect-fn fix-fn) → starts background loop
    add("auto-evolve-loop", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_closure(a[1]) || !is_closure(a[2]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        double interval = 1.0;
        try {
            interval = std::stod(ev.string_heap_[idx]);
        } catch (...) {
            // [SILENCE-PRIM-#615] Rate-limit interval parse failure is
            // non-user-visible; default of 1.0 below is the documented
            // behavior. Upgrading to PRIM_ERROR would force callers to
            // wrap a system-internal call.
        }
        if (interval < 0.1)
            interval = 0.1;
        ev.auto_evolve_running_ = true;
        ev.auto_evolve_interval_ = interval;
        ev.auto_evolve_detect_closure_ = as_closure_id(a[1]);
        ev.auto_evolve_fix_closure_ = as_closure_id(a[2]);
        ev.auto_evolve_cycle_count_ = 0;
        ev.auto_evolve_total_fixed_ = 0;
        return make_int(0);
    });

    add("auto-evolve-stop", [&ev](const auto&) -> EvalValue {
        if (!ev.auto_evolve_running_)
            return make_bool(false);
        ev.auto_evolve_running_ = false;
        return make_bool(true);
    });

    add("auto-evolve-running?",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.auto_evolve_running_); });

    add("auto-evolve-tick", [&ev](const auto&) -> EvalValue {
        if (!ev.auto_evolve_running_)
            return make_bool(false);
        if (ev.auto_evolve_detect_closure_ == 0 || ev.auto_evolve_fix_closure_ == 0)
            return make_bool(false);
        ++ev.auto_evolve_cycle_count_;
        std::fprintf(stderr, "[DBG tick] detect=%zu fix=%zu\n",
                     (size_t)ev.auto_evolve_detect_closure_, (size_t)ev.auto_evolve_fix_closure_);
        auto detect_result = ev.apply_closure(ev.auto_evolve_detect_closure_, {});
        if (!detect_result) {
            std::fprintf(stderr, "  no detect result\n");
            return make_bool(true);
        }
        std::fprintf(stderr, "  detect.val=0x%lx\n", (long)(*detect_result).val);
        EvalValue current = *detect_result;
        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= ev.pairs_.size())
                break;
            auto gap = ev.pairs_[idx].car;
            auto fix_result = ev.apply_closure(ev.auto_evolve_fix_closure_, {gap});
            if (fix_result) {
                if (is_bool(*fix_result) && as_bool(*fix_result))
                    ++ev.auto_evolve_total_fixed_;
            }
            current = ev.pairs_[idx].cdr;
        }
        return make_bool(true);
    });

    add("auto-evolve-cycle-count", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ev.auto_evolve_cycle_count_));
    });

    add("auto-evolve-total-fixed", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ev.auto_evolve_total_fixed_));
    });

    // ── Issue #444: strategy evolution controller ────────
    //
    // 3 built-in strategies. Each mutation success bumps
    // the strategy's "successes" counter; a plateau
    // bumps "escalations" + switches the active strategy.
    //
    //   "coverage-greedy" — pick the node with the most
    //     coverage gaps (highest density). Best when the
    //     verify-dirty count is high.
    //   "bug-fix-priority" — pick the node that's been
    //     failing assertions. Best when verify-dirty has
    //     kAssertionDirty set on many nodes.
    //   "minimal-mutation" — pick the smallest mutation
    //     that might help (single-line change). Best when
    //     prior mutations caused regressions.
    //
    // The controller is rule-based (no PID in the MVP —
    // the PID + pheromone accumulation comes in a follow-
    // up issue once the baseline coverage curves are
    // established). The escalation rule:
    //   if (coverage_delta < threshold) &&
    //      (successes_in_window == 0):
    //     escalate (e.g. coverage-greedy → bug-fix-priority)
    add("strategy:set-strategy", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        const std::string& name = ev.string_heap_[idx];
        if (name != "coverage-greedy" && name != "bug-fix-priority" && name != "minimal-mutation") {
            return make_void();
        }
        ev.active_strategy_ = name;
        // Bump the hits counter for the new strategy.
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            if (name == "coverage-greedy")
                m->strategy_greedy_hits.fetch_add(1, std::memory_order_relaxed);
            else if (name == "bug-fix-priority")
                m->strategy_bugfix_hits.fetch_add(1, std::memory_order_relaxed);
            else if (name == "minimal-mutation")
                m->strategy_minimal_hits.fetch_add(1, std::memory_order_relaxed);
        }
        return make_int(static_cast<std::int64_t>(ev.active_strategy_.size()));
    });

    add("strategy:active", [&ev](const auto&) -> EvalValue {
        if (ev.active_strategy_.empty())
            return make_void();
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.active_strategy_);
        return make_string(sidx);
    });

    // (strategy:report-success strategy-name) — call
    // after a successful mutation driven by the given
    // strategy. Bumps the strategy's success counter.
    add("strategy:report-success", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        const std::string& name = ev.string_heap_[idx];
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            if (name == "coverage-greedy")
                m->strategy_greedy_successes.fetch_add(1, std::memory_order_relaxed);
            else if (name == "bug-fix-priority")
                m->strategy_bugfix_successes.fetch_add(1, std::memory_order_relaxed);
            else if (name == "minimal-mutation")
                m->strategy_minimal_successes.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(true);
    });

    // (strategy:escalate reason) — record a strategy
    // escalation event (the controller decided to switch
    // strategies due to a coverage plateau or a
    // regression). Reason is a string for observability.
    add("strategy:escalate", [&ev](const auto& a) -> EvalValue {
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->strategy_escalations.fetch_add(1, std::memory_order_relaxed);
        }
        (void)a;
        return make_bool(true);
    });

    // (query:strategy-evolution-stats) — Issue #444:
    // hash variant of the strategy pheromone counters.
    // 7 fields:
    //   - active-strategy         (string, current)
    //   - greedy-hits / greedy-successes
    //   - bugfix-hits / bugfix-successes
    //   - minimal-hits / minimal-successes
    //   - escalations
    add("query:strategy-evolution-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::uint64_t greedy_h = 0, greedy_s = 0;
        std::uint64_t bugfix_h = 0, bugfix_s = 0;
        std::uint64_t minimal_h = 0, minimal_s = 0;
        std::uint64_t escalations = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            greedy_h = m->strategy_greedy_hits.load(std::memory_order_relaxed);
            greedy_s = m->strategy_greedy_successes.load(std::memory_order_relaxed);
            bugfix_h = m->strategy_bugfix_hits.load(std::memory_order_relaxed);
            bugfix_s = m->strategy_bugfix_successes.load(std::memory_order_relaxed);
            minimal_h = m->strategy_minimal_hits.load(std::memory_order_relaxed);
            minimal_s = m->strategy_minimal_successes.load(std::memory_order_relaxed);
            escalations = m->strategy_escalations.load(std::memory_order_relaxed);
        }
        // active-strategy as a string field.
        std::string active = ev.active_strategy_;
        auto active_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(active);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"active-strategy", make_string(active_idx)},
            {"greedy-hits", make_int(static_cast<std::int64_t>(greedy_h))},
            {"greedy-successes", make_int(static_cast<std::int64_t>(greedy_s))},
            {"bugfix-hits", make_int(static_cast<std::int64_t>(bugfix_h))},
            {"bugfix-successes", make_int(static_cast<std::int64_t>(bugfix_s))},
            {"minimal-hits", make_int(static_cast<std::int64_t>(minimal_h))},
            {"minimal-successes", make_int(static_cast<std::int64_t>(minimal_s))},
            {"escalations", make_int(static_cast<std::int64_t>(escalations))},
        };
        return build_hash(kv);
    });
}

void register_synthesize_primitives(PrimRegistrar add, Evaluator& ev,
                                    std::function<void()> destroy_defuse_index) {

    // ═══════════════════════════════════════════════════════════════
    // P15: Synthesize Template Strategy (P0)
    // ═══════════════════════════════════════════════════════════════

    // (synthesize:register-template name pattern param-names...)
    add("synthesize:register-template", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        auto pat_idx = as_string_idx(a[1]);
        if (name_idx >= ev.string_heap_.size() || pat_idx >= ev.string_heap_.size())
            return make_bool(false);

        std::string name = ev.string_heap_[name_idx];
        std::string pattern = ev.string_heap_[pat_idx];
        std::vector<std::string> params;
        for (std::size_t i = 2; i < a.size(); ++i) {
            if (is_string(a[i])) {
                auto pidx = as_string_idx(a[i]);
                if (pidx < ev.string_heap_.size())
                    params.push_back(ev.string_heap_[pidx]);
            }
        }

        // Replace or append
        bool found = false;
        for (auto& t : g_template_patterns) {
            if (t.first == name) {
                t.second = pattern;
                found = true;
                break;
            }
        }
        if (!found) {
            g_template_patterns.push_back({name, pattern});
            g_template_params.push_back(params);
        } else {
            // Find the params index
            for (std::size_t i = 0; i < g_template_patterns.size(); ++i) {
                if (g_template_patterns[i].first == name) {
                    g_template_params[i] = params;
                    break;
                }
            }
        }
        return make_bool(true);
    });

    // (synthesize:fill template-name arg-values...)
    add("synthesize:fill", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        if (name_idx >= ev.string_heap_.size())
            return make_void();
        std::string name = ev.string_heap_[name_idx];

        // Find template
        int ti = -1;
        for (std::size_t i = 0; i < g_template_patterns.size(); ++i) {
            if (g_template_patterns[i].first == name) {
                ti = static_cast<int>(i);
                break;
            }
        }
        if (ti < 0)
            return make_bool(false);

        // Build substitution map
        std::unordered_map<std::string, std::string> subst;
        if (static_cast<std::size_t>(ti) < g_template_params.size()) {
            auto& pnames = g_template_params[ti];
            for (std::size_t i = 0; i < pnames.size() && i + 1 < a.size(); ++i) {
                if (is_string(a[i + 1])) {
                    auto vidx = as_string_idx(a[i + 1]);
                    if (vidx < ev.string_heap_.size())
                        subst[pnames[i]] = ev.string_heap_[vidx];
                }
            }
        }

        // Apply {param} substitutions
        std::string pattern = g_template_patterns[ti].second;
        std::string filled;
        std::size_t pos = 0;
        while (pos < pattern.size()) {
            auto open = pattern.find('{', pos);
            if (open == std::string::npos) {
                filled.append(pattern, pos, std::string::npos);
                break;
            }
            filled.append(pattern, pos, open - pos);
            auto close = pattern.find('}', open);
            if (close == std::string::npos) {
                filled.append(pattern, open);
                break;
            }
            auto pname = pattern.substr(open + 1, close - open - 1);
            auto it = subst.find(pname);
            if (it != subst.end())
                filled.append(it->second);
            else
                filled += "{" + pname + "}";
            pos = close + 1;
        }

        // Apply filled code to workspace via set-code
        auto code_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(filled);
        auto sc_fn = ev.primitives_.lookup("set-code");
        if (!sc_fn)
            return make_void();
        auto result = (*sc_fn)({make_string(code_idx)});
        // Return the source or true
        if (is_bool(result))
            return result;
        return make_bool(true);
    });

    // Issue #561: (synthesize:list-templates) demoted to
    // stdlib (lib/std/synthesize.aura (synthesize:list-templates)
    // wraps the new (query:templates) primitive below).
    // The C++ primitive was a pure read of g_template_patterns
    // — no state mutation, no LLM call — so it's a textbook
    // stdlib candidate per the decision framework.

    // (query:templates) — Issue #561 engine-level accessor.
    // Returns the list of registered synthesize template names.
    // Exposed so std/stats.aura / std/synthesize.aura can
    // enumerate without touching the static g_template_patterns
    // map directly.
    add("query:templates", [&ev](const auto&) -> EvalValue {
        EvalValue list = make_void();
        for (auto it = g_template_patterns.rbegin(); it != g_template_patterns.rend(); ++it) {
            auto idx = ev.string_heap_.size();
            ev.string_heap_.push_back(it->first);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(idx), list});
            list = make_pair(pid);
        }
        return list;
    });

    // ═══════════════════════════════════════════════════════════════
    // ═══════════════════════════════════════════════════════════════
    // ═══════════════════════════════════════════════════════════════
    // P16: Synthesize LLM Strategy — synthesize:define
    // ═══════════════════════════════════════════════════════════════

    // (synthesize:define name sig [key :val ...])
    //   Generates a function using LLM.
    add("synthesize:define", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        auto sig_idx = as_string_idx(a[1]);
        if (name_idx >= ev.string_heap_.size() || sig_idx >= ev.string_heap_.size())
            return make_void();

        std::string name = ev.string_heap_[name_idx];
        std::string sig = ev.string_heap_[sig_idx];
        std::string prompt_text;
        std::string model = "deepseek-chat";
        std::string examples_text;
        int max_attempts = 5;

        // Parse keyword arguments
        for (std::size_t i = 2; i + 1 < a.size(); i += 2) {
            if (!is_string(a[i]))
                continue;
            auto k_idx = as_string_idx(a[i]);
            if (k_idx >= ev.string_heap_.size())
                continue;
            std::string key = ev.string_heap_[k_idx];

            if (key == ":prompt" && is_string(a[i + 1])) {
                auto pidx = as_string_idx(a[i + 1]);
                if (pidx < ev.string_heap_.size())
                    prompt_text = ev.string_heap_[pidx];
            } else if (key == ":model" && is_string(a[i + 1])) {
                auto midx = as_string_idx(a[i + 1]);
                if (midx < ev.string_heap_.size())
                    model = ev.string_heap_[midx];
            } else if (key == ":examples" && is_string(a[i + 1])) {
                auto eidx = as_string_idx(a[i + 1]);
                if (eidx < ev.string_heap_.size())
                    examples_text = ev.string_heap_[eidx];
            } else if (key == ":max-attempts" && is_int(a[i + 1])) {
                max_attempts = static_cast<int>(as_int(a[i + 1]));
            }
        }

        // Build prompt: construct a simple instruction string
        std::string instruction =
            "Define a function named " + name + " in Aura Lisp with signature: " + sig + ".\n";
        if (!prompt_text.empty())
            instruction += "Task: " + prompt_text + ".\n";
        if (!examples_text.empty())
            instruction += "Examples: " + examples_text + "\n";

        // Get API key
        auto getenv_fn = ev.primitives_.lookup("getenv");
        std::string api_key;
        if (getenv_fn) {
            auto kidx = ev.string_heap_.size();
            ev.string_heap_.push_back("LLM_API_KEY");
            auto kr = (*getenv_fn)({make_string(kidx)});
            if (is_string(kr)) {
                auto ai = as_string_idx(kr);
                if (ai < ev.string_heap_.size())
                    api_key = ev.string_heap_[ai];
            }
        }
        if (api_key.empty())
            return make_string(0);

        // Get http-post primitive
        auto http_fn = ev.primitives_.lookup("http-post");
        if (!http_fn)
            return make_void();

        // Get API URL
        std::string api_url = "https://api.deepseek.com/v1/chat/completions";
        if (getenv_fn) {
            auto uidx = ev.string_heap_.size();
            ev.string_heap_.push_back("LLM_API_URL");
            auto ur = (*getenv_fn)({make_string(uidx)});
            if (is_string(ur)) {
                auto ui = as_string_idx(ur);
                if (ui < ev.string_heap_.size() && !ev.string_heap_[ui].empty())
                    api_url = ev.string_heap_[ui];
            }
        }

        // Auto-fix loop
        std::string last_error;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            // Build JSON payload manually (avoid escaping issues)
            std::string body;
            body += "{\n";
            body += "  \"model\": \"" + model + "\",\n";
            body += "  \"messages\": [\n";
            body += "    {\"role\": \"system\", \"content\": \"You are Aura Lisp. Return ONLY "
                    "valid Aura code. No markdown.\"},\n";
            body += "    {\"role\": \"user\", \"content\": \"" + instruction;
            if (!last_error.empty())
                body += " Previous attempt error: " + last_error + ". Fix it.";
            body += "\"}\n";
            body += "  ]\n";
            body += "}\n";

            auto bi = ev.string_heap_.size();
            ev.string_heap_.push_back(body);
            auto ui2 = ev.string_heap_.size();
            ev.string_heap_.push_back(api_url);
            auto ki = ev.string_heap_.size();
            ev.string_heap_.push_back(api_key);

            auto resp = (*http_fn)({make_string(ui2), make_string(bi), make_string(ki)});
            if (!is_string(resp))
                continue;
            auto ri = as_string_idx(resp);
            if (ri >= ev.string_heap_.size())
                continue;
            auto& response = ev.string_heap_[ri];

            // Extract code from JSON response
            std::string code;
            auto cp = response.find("content");
            if (cp == std::string::npos)
                continue;
            auto cq = response.find('"', cp + 9);
            if (cq == std::string::npos)
                continue;
            cq++;
            bool esc = false;
            for (; cq < response.size(); ++cq) {
                char c = response[cq];
                if (esc) {
                    if (c == 'n')
                        code += '\n';
                    else if (c == 't')
                        code += '\t';
                    else if (c == '"')
                        code += '"';
                    else if (c == '\\')
                        code += '\\';
                    else
                        code += c;
                    esc = false;
                } else if (c == '\\') {
                    esc = true;
                } else if (c == '"') {
                    break;
                } else {
                    code += c;
                }
            }

            if (code.empty())
                continue;

            // Try set-code
            auto ci = ev.string_heap_.size();
            ev.string_heap_.push_back(code);
            auto sc_fn = ev.primitives_.lookup("set-code");
            if (!sc_fn)
                continue;
            auto sc_r = (*sc_fn)({make_string(ci)});
            if (!is_bool(sc_r) || !as_bool(sc_r)) {
                last_error = "syntax error";
                continue;
            }

            // Try typecheck (Issue #107 part 4: inline, no lock).
            // Going through the typecheck-current primitive would
            // re-enter workspace_mtx_ and deadlock.
            {
                std::string msg = ev.run_typecheck_no_lock();
                if (msg.find("error") != std::string::npos ||
                    msg.find("unbound") != std::string::npos) {
                    last_error = msg;
                    continue;
                }
            }

            // Success
            ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
            ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
            auto src_fn = ev.primitives_.lookup("current-source");
            if (src_fn) {
                auto src = (*src_fn)({});
                return src;
            }
            return make_bool(true);
        }

        return make_bool(false);
    });

    // ═══════════════════════════════════════════════════════════════
    // P17: Synthesize Genetic Strategy — synthesize:optimize
    // ═══════════════════════════════════════════════════════════════

    // (synthesize:optimize name [key :val ...])
    //   Uses genetic algorithm to optimize a function.
    //   Keywords: :population, :generations, :mutation-rate,
    //             :fitness or :benchmark (benchmark expression)
    //   Default fitness: runs function with synthetic test inputs
    //   (correctness = 90% weight, code length = 10% tiebreaker).
    //   Creates variants, evaluates fitness, returns best.
    add("synthesize:optimize",
        [&ev, destroy_defuse_index](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_string(a[0]))
                return make_void();
            auto name_idx = as_string_idx(a[0]);
            if (name_idx >= ev.string_heap_.size())
                return make_void();
            std::string fn_name = ev.string_heap_[name_idx];
            int pop_size = 8;
            int generations = 3;
            double mutation_rate = 0.3;
            std::string fitness_expr; // optional user-provided fitness expr

            for (std::size_t i = 1; i + 1 < a.size(); i += 2) {
                if (!is_string(a[i]))
                    continue;
                auto k_idx = as_string_idx(a[i]);
                if (k_idx >= ev.string_heap_.size())
                    continue;
                std::string key = ev.string_heap_[k_idx];
                if (key == ":population" && is_int(a[i + 1]))
                    pop_size = static_cast<int>(as_int(a[i + 1]));
                else if (key == ":generations" && is_int(a[i + 1]))
                    generations = static_cast<int>(as_int(a[i + 1]));
                else if (key == ":mutation-rate" && is_float(a[i + 1]))
                    mutation_rate = as_float(a[i + 1]);
                else if ((key == ":fitness" || key == ":benchmark") && is_string(a[i + 1])) {
                    auto fi = as_string_idx(a[i + 1]);
                    if (fi < ev.string_heap_.size())
                        fitness_expr = ev.string_heap_[fi];
                }
            }
            if (pop_size < 2)
                pop_size = 2;
            if (pop_size > 50)
                pop_size = 50;
            if (generations < 1)
                generations = 1;

            // Get baseline source — use :workspace to read the user's set-code'd
            // script (the function to optimize), NOT the per-eval source (which
            // would be the surrounding synthesize:optimize call itself, causing
            // mutations to recursive-call this primitive → stack overflow).
            // Same root cause as colony-no-fns (commit 6d88544).
            auto src_fn = ev.primitives_.lookup("current-source");
            if (!src_fn)
                return make_void();
            // Intern :workspace keyword (lookup or add to ev.keyword_table_).
            std::uint64_t ws_kw = 0;
            bool ws_found = false;
            for (; ws_kw < ev.keyword_table_.size(); ++ws_kw) {
                if (ev.keyword_table_[ws_kw] == ":workspace") {
                    ws_found = true;
                    break;
                }
            }
            if (!ws_found) {
                ws_kw = ev.keyword_table_.size();
                ev.keyword_table_.push_back(":workspace");
            }
            auto cs_result = (*src_fn)({types::make_keyword(ws_kw)});
            if (!is_string(cs_result))
                return make_void();
            auto cs_idx = as_string_idx(cs_result);
            if (cs_idx >= ev.string_heap_.size())
                return make_void();
            std::string baseline = ev.string_heap_[cs_idx];

            // Fitness: generate synthetic test inputs and eval the function
            // to measure correctness + performance.
            //
            // Strategy:
            // 1. Parse variant source, count function args by scanning for fn_name
            // 2. Generate probe inputs (ints, pairs, etc.) based on arg count
            // 3. Eval each probe: (fn_name arg...), score = fraction of probes that
            //    return a valid value without error
            // 4. Code length is a tiebreaker only — correctness dominates
            //
            // If :fitness keyword is provided, use that expression instead.
            auto compute_fitness = [&](const std::string& src) -> double {
                if (!fitness_expr.empty()) {
                    // User-provided fitness: eval the expression
                    auto sv = ev.string_heap_.size();
                    ev.string_heap_.push_back(src);
                    auto eval_fn = ev.primitives_.lookup("eval");
                    if (eval_fn) {
                        auto r = (*eval_fn)({make_string(sv)});
                        if (is_float(r))
                            return as_float(r);
                        if (is_int(r))
                            return static_cast<double>(as_int(r));
                    }
                    return 0.0;
                }

                // Default fitness: eval the current workspace (which contains
                // the variant code) to bind the function in top_, then probe
                // via the eval primitive.
                //
                // The calling code has already set up the workspace via
                // set-code + typecheck-current before we're called, so
                // eval-current binds the function using workspace flats
                // (safe — no dangling closure pointers).
                //
                // Detect argument count by scanning for (define (fn_name ...))
                int arg_count = 0;
                {
                    auto def_pos = src.find("(define (" + fn_name);
                    if (def_pos != std::string::npos) {
                        auto after_fn = src.find(' ', def_pos);
                        if (after_fn != std::string::npos) {
                            auto param_start = src.find(' ', after_fn + 1);
                            if (param_start != std::string::npos) {
                                auto close_pos = src.find(')', param_start);
                                if (close_pos != std::string::npos) {
                                    auto params =
                                        src.substr(param_start + 1, close_pos - param_start - 1);
                                    if (!params.empty()) {
                                        arg_count = 1;
                                        for (auto c : params) {
                                            if (c == ' ')
                                                ++arg_count;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Eval-current to bind functions in top_ (uses workspace flats, safe)
                auto ec_fn = ev.primitives_.lookup("eval-current");
                if (ec_fn) {
                    (*ec_fn)({});
                }

                // Probe via eval (function is now bound in top_ from workspace eval)
                // Temporary flats in eval are OK for call expressions — they don't
                // create closures, just look up and apply.
                static const std::int64_t probe_ints[] = {0, 1, -1, 2};
                int successes = 0;
                int total_tests = 0;
                auto eval_fn = ev.primitives_.lookup("eval");

                auto try_probe = [&](const std::string& call_src) {
                    ++total_tests;
                    auto ci = ev.string_heap_.size();
                    ev.string_heap_.push_back(call_src);
                    if (eval_fn) {
                        auto r = (*eval_fn)({make_string(ci)});
                        if (!types::is_error(r))
                            ++successes;
                    }
                };

                if (arg_count <= 0) {
                    try_probe("(" + fn_name + ")");
                } else if (arg_count == 1) {
                    for (auto v : probe_ints) {
                        if (total_tests >= 4)
                            break;
                        try_probe("(" + fn_name + " " + std::to_string(v) + ")");
                    }
                } else if (arg_count == 2) {
                    for (int i = 0; i < 4 && i + 1 < 4; ++i) {
                        try_probe("(" + fn_name + " " + std::to_string(probe_ints[i]) + " " +
                                  std::to_string(probe_ints[i + 1]) + ")");
                    }
                } else {
                    std::string call_src = "(" + fn_name;
                    for (int i = 0; i < arg_count; ++i)
                        call_src += " 0";
                    call_src += ")";
                    try_probe(call_src);
                }

                // Score: correctness dominates (up to 1000), then small length bonus
                double correctness = total_tests > 0 ? (1000.0 * static_cast<double>(successes) /
                                                        static_cast<double>(total_tests))
                                                     : 0.0;
                double length_bonus = 1.0 / static_cast<double>(src.size() + 1);
                return correctness + length_bonus;
            };

            std::string best_code = baseline;
            double best_fitness = compute_fitness(baseline);
            int best_gen = 0;

            // Store multiple candidates for crossover (elitism)
            std::vector<std::pair<std::string, double>> elite;

            for (int gen = 0; gen < generations; ++gen) {
                for (int p = 0; p < pop_size; ++p) {
                    std::string variant = best_code;

                    // Apply mutations
                    for (int m = 0; m < 5; ++m) {
                        if (static_cast<double>(std::rand()) / RAND_MAX >= mutation_rate)
                            continue;
                        // Operator swap
                        for (const char* op = "+-*/"; *op; ++op) {
                            auto opos = variant.find(*op);
                            if (opos != std::string::npos && opos > 0) {
                                variant[opos] = "+-*/"[std::rand() % 4];
                                break;
                            }
                        }
                        // Numeric mutation
                        auto npos = variant.find_first_of("0123456789");
                        if (npos == std::string::npos)
                            break;
                        auto nend = variant.find_first_not_of("0123456789", npos);
                        if (nend == std::string::npos)
                            nend = variant.size();
                        std::string old_n = variant.substr(npos, nend - npos);
                        if (old_n.empty())
                            continue;
                        int val = std::stoi(old_n);
                        val += (std::rand() % 21) - 10;
                        if (val < 0)
                            val = 0;
                        variant.replace(npos, nend - npos, std::to_string(val));
                    }

                    // Crossover: text-level or AST-level
                    if (!elite.empty() && std::rand() % 3 == 0) {
                        auto& other = elite[std::rand() % elite.size()].first;
                        // Try AST expression-level crossover via node swapping
                        // Use a child workspace and mutate:replace-value
                        if (ev.workspace_tree_ && std::rand() % 2 == 0) {
                            auto* tree =
                                static_cast<aura::compiler::WorkspaceTree*>(ev.workspace_tree_);
                            // Create a temporary workspace, set-code the variant,
                            // find a LiteralInt node, replace it with one from other
                            auto ws_id = tree->create_child("xover", tree->active_idx(),
                                                            ev.workspace_flat_, ev.workspace_pool_);
                            if (ws_id > 0) {
                                tree->ensure_local_flat(ws_id);
                                auto& ws = tree->nodes_[ws_id];
                                auto saved_f = ev.workspace_flat_;
                                auto saved_p = ev.workspace_pool_;
                                ev.workspace_flat_ = ws.flat;
                                ev.workspace_pool_ = ws.pool;

                                // Set variant as current code
                                auto vi = ev.string_heap_.size();
                                ev.string_heap_.push_back(variant);
                                auto sc_fn = ev.primitives_.lookup("set-code");
                                if (sc_fn) {
                                    auto sr = (*sc_fn)({make_string(vi)});
                                    if (is_bool(sr) && as_bool(sr)) {
                                        // Find LiteralInt nodes and swap value with other variant
                                        for (aura::ast::NodeId nid = 0;
                                             nid <
                                             (ev.workspace_flat_ ? ev.workspace_flat_->size() : 0);
                                             ++nid) {
                                            if (std::rand() % 5 != 0)
                                                continue; // 20% chance per node
                                            auto v = ev.workspace_flat_->get(nid);
                                            if (v.tag == aura::ast::NodeTag::LiteralInt) {
                                                // Extract a random int from "other"
                                                auto nums = other;
                                                auto npos = nums.find_first_of("0123456789");
                                                if (npos != std::string::npos) {
                                                    auto nend =
                                                        nums.find_first_not_of("0123456789", npos);
                                                    if (nend == std::string::npos)
                                                        nend = nums.size();
                                                    int new_val =
                                                        std::stoi(nums.substr(npos, nend - npos));
                                                    if (new_val >= 0) {
                                                        auto rv_fn = ev.primitives_.lookup(
                                                            "mutate:replace-value");
                                                        if (rv_fn) {
                                                            (*rv_fn)({make_int(nid),
                                                                      make_int(new_val),
                                                                      make_string(vi)});
                                                        }
                                                    }
                                                }
                                                break; // Mutate one node
                                            }
                                        }

                                        // Typecheck after crossover (Issue #107 part 4: inline)
                                        if (true) {
                                            auto tc_r = ev.run_typecheck_no_lock_bool();
                                            if (tc_r) {
                                                // Successful crossover: get the new source
                                                // (use :workspace to read user's set-code'd script,
                                                // not the per-eval source = the surrounding call)
                                                auto src_fn =
                                                    ev.primitives_.lookup("current-source");
                                                if (src_fn) {
                                                    std::uint64_t ws_kw = 0;
                                                    bool ws_found = false;
                                                    for (; ws_kw < ev.keyword_table_.size();
                                                         ++ws_kw) {
                                                        if (ev.keyword_table_[ws_kw] ==
                                                            ":workspace") {
                                                            ws_found = true;
                                                            break;
                                                        }
                                                    }
                                                    if (!ws_found) {
                                                        ws_kw = ev.keyword_table_.size();
                                                        ev.keyword_table_.push_back(":workspace");
                                                    }
                                                    auto src =
                                                        (*src_fn)({types::make_keyword(ws_kw)});
                                                    if (is_string(src)) {
                                                        auto si = as_string_idx(src);
                                                        if (si < ev.string_heap_.size())
                                                            variant = ev.string_heap_[si];
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                ev.workspace_flat_ = saved_f;
                                ev.workspace_pool_ = saved_p;
                                tree->delete_child(ws_id);
                            }
                        } else {
                            // Text-level crossover (fallback)
                            auto b1 = variant.find("(lambda");
                            auto b2 = other.find("(lambda");
                            if (b1 != std::string::npos && b2 != std::string::npos) {
                                auto e1 = variant.find(')', b1);
                                auto e2 = other.find(')', b2);
                                if (e1 != std::string::npos && e2 != std::string::npos && e1 > b1 &&
                                    e2 > b2) {
                                    auto body1_end = variant.find_last_of(')');
                                    auto body2_end = other.find_last_of(')');
                                    if (body1_end > b1 && body2_end > b2) {
                                        std::string body1 = variant.substr(b1, body1_end - b1 + 1);
                                        std::string body2 = other.substr(b2, body2_end - b2 + 1);
                                        if (body1 != body2) {
                                            variant = variant.substr(0, b1) + body2 +
                                                      variant.substr(body1_end + 1);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (variant == best_code)
                        continue;

                    // Evaluate in a child workspace (isolation)
                    // Use workspace tree if available, otherwise fall back to set-code
                    bool evaluated = false;
                    double f = 0.0;
                    auto sc_fn = ev.primitives_.lookup("set-code");
                    if (!sc_fn)
                        continue;

                    if (ev.workspace_tree_) {
                        // Use child workspace for isolation
                        auto* tree =
                            static_cast<aura::compiler::WorkspaceTree*>(ev.workspace_tree_);
                        auto ws_id = tree->create_child("evolve-variant", tree->active_idx(),
                                                        ev.workspace_flat_, ev.workspace_pool_);
                        // Switch to child and try the variant
                        if (ws_id > 0) {
                            tree->ensure_local_flat(ws_id);
                            auto& ws = tree->nodes_[ws_id];
                            auto saved_flat = ev.workspace_flat_;
                            auto saved_pool = ev.workspace_pool_;
                            ev.workspace_flat_ = ws.flat;
                            ev.workspace_pool_ = ws.pool;

                            auto vi = ev.string_heap_.size();
                            ev.string_heap_.push_back(variant);
                            auto sc_r = (*sc_fn)({make_string(vi)});

                            if (is_bool(sc_r) && as_bool(sc_r)) {
                                // Issue #107 part 4: inline typecheck (no lock).
                                // Going through the primitive would re-enter
                                // workspace_mtx_ and deadlock.
                                bool valid = ev.run_typecheck_no_lock_bool();
                                if (valid) {
                                    f = compute_fitness(variant);
                                    evaluated = true;
                                }
                            }

                            ev.workspace_flat_ = saved_flat;
                            ev.workspace_pool_ = saved_pool;
                            tree->delete_child(ws_id);
                        }
                    }

                    if (!evaluated) {
                        // Fallback: direct set-code
                        auto vi = ev.string_heap_.size();
                        ev.string_heap_.push_back(variant);
                        auto sc_r = (*sc_fn)({make_string(vi)});
                        if (!is_bool(sc_r) || !as_bool(sc_r))
                            continue;

                        // Issue #107 part 4: inline typecheck (no lock).
                        // Going through the primitive would re-enter
                        // workspace_mtx_ and deadlock.
                        bool valid = ev.run_typecheck_no_lock_bool();
                        if (!valid)
                            continue;
                        f = compute_fitness(variant);
                        // Restore
                        auto bi = ev.string_heap_.size();
                        ev.string_heap_.push_back(baseline);
                        (*sc_fn)({make_string(bi)});
                    }

                    if (f > best_fitness) {
                        best_fitness = f;
                        best_code = variant;
                        best_gen = gen + 1;
                    }
                }

                // Update elite from this generation
                elite.clear();
                elite.push_back({best_code, best_fitness});
            }

            // Apply best to workspace
            auto bi = ev.string_heap_.size();
            ev.string_heap_.push_back(best_code);
            auto sc_fn = ev.primitives_.lookup("set-code");
            if (sc_fn)
                (*sc_fn)({make_string(bi)});
            ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);
            ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
            // (ASAN fix #107 leak) delete the old index.
            destroy_defuse_index();

            auto gs = std::to_string(best_gen);
            auto gi = ev.string_heap_.size();
            ev.string_heap_.push_back(gs);
            auto fs = std::to_string(best_fitness);
            auto fi = ev.string_heap_.size();
            ev.string_heap_.push_back(fs);

            auto p1 = ev.pairs_.size();
            ev.pairs_.push_back({make_string(gi), make_string(fi)});
            return make_pair(p1);
        });
}

void register_strategy_primitives(PrimRegistrar add, Evaluator& ev) {

    // ── intend — 纯循环管理器 — 纯循环管理器 ────────────────────────────────
    // (intend goal generator-fn verifier-fn [fixer-fn] [max-attempts])
    //
    // 不管理 LLM 调用、不构建 prompt、不做 JSON 解析。
    // 只做循环编排。LLM 交互通过传入的 Aura 函数完成。
    //
    // - generator-fn: (lambda (goal) → code-string)
    // - verifier-fn:  (lambda (code) → "#t" for pass, else error-string)
    // - fixer-fn:     (lambda (code error goal) → new-code-string, optional)
    // - max-attempts: int (optional, default 3)
    add("intend", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Mark task context so closure bodies are allocated in temp_arena_.
        // Issue #68: depth counter (was bool) for nested intend support.
        int saved_depth = ev.in_task_context_;
        ev.in_task_context_ = saved_depth + 1;
        auto restore = [&] { ev.in_task_context_ = saved_depth; };

        if (a.size() < 3) {
            restore();
            return make_void();
        }
        if (!types::is_string(a[0]) || !types::is_closure(a[1]) || !types::is_closure(a[2])) {
            restore();
            return make_void();
        }

        auto goal = ev.string_heap_[types::as_string_idx(a[0])];
        auto gen_cid = types::as_closure_id(a[1]); // generator
        auto ver_cid = types::as_closure_id(a[2]); // verifier

        // Issue #63 Phase 3: optional strategy-name (string) as the
        // 3rd-or-4th positional arg. When present, use the strategy's
        // `max_attempts` (overriding the 4th-or-5th positional int).
        // Format: (intend goal gen ver [fixer] [max-attempts]
        //                 :strategy name)  ← keyword form preferred
        std::string strategy_name = "default";
        int max_attempts = 3;
        bool has_fixer = false;
        std::uint64_t fix_cid = 0;
        // Walk remaining positional args; support both (fixer [max])
        // and (fixer max :strategy name) orderings by looking for the
        // :strategy keyword.
        std::size_t i = 3;
        if (i < a.size() && types::is_closure(a[i])) {
            has_fixer = true;
            fix_cid = types::as_closure_id(a[i]);
            ++i;
        }
        if (i < a.size() && types::is_int(a[i])) {
            max_attempts = static_cast<int>(types::as_int(a[i]));
            ++i;
        }
        for (; i + 1 < a.size(); i += 2) {
            std::string k;
            if (types::is_string(a[i])) {
                k = ev.string_heap_[types::as_string_idx(a[i])];
            } else if (types::is_keyword(a[i])) {
                auto kidx = types::as_keyword_idx(a[i]);
                if (kidx < ev.keyword_table_.size())
                    k = ev.keyword_table_[kidx];
            } else
                continue;
            if (k == ":strategy" && types::is_string(a[i + 1])) {
                strategy_name = ev.string_heap_[types::as_string_idx(a[i + 1])];
                // Look up the strategy's max_attempts (overrides int arg)
                for (auto& s : ev.strategies_) {
                    if (s.name == strategy_name) {
                        max_attempts = s.max_attempts;
                        break;
                    }
                }
            }
        }
        if ((has_fixer && a.size() >= 5 && types::is_int(a[4])) ||
            (!has_fixer && a.size() >= 4 && types::is_int(a[3]))) {
            auto idx = has_fixer ? 4 : 3;
            max_attempts = static_cast<int>(types::as_int(a[idx]));
        }

        auto t0 = std::chrono::steady_clock::now();
        ev.timeline_.clear();
        ev.timeline_.push_back("start:" + goal);

        std::string current_code_str;
        std::string last_error;
        std::vector<std::string> errors;
        std::vector<std::string> error_types;
        std::vector<std::string> generated_codes;
        std::uint64_t llm_call_count = 0;

        auto classify_error = [](const std::string& err) -> std::string {
            if (err.find("unbound variable") != std::string::npos)
                return "unbound-variable";
            if (err.find("type mismatch") != std::string::npos)
                return "type-mismatch";
            if (err.find("division by zero") != std::string::npos)
                return "div-zero";
            if (err.find("syntax") != std::string::npos ||
                err.find("unbalanced") != std::string::npos)
                return "syntax-error";
            if (err.find("timeout") != std::string::npos)
                return "timeout";
            if (err.find("recursion") != std::string::npos ||
                err.find("stack") != std::string::npos)
                return "recursion-limit";
            if (err.find("bad-code") != std::string::npos)
                return "bad-code";
            if (err.find("verification failed") != std::string::npos)
                return "verification-failed";
            return "other";
        };

        // Call a closure, return string result
        auto call_fn = [&](std::uint64_t cid,
                           std::initializer_list<types::EvalValue> args) -> std::string {
            auto opt = ev.apply_closure(cid, args);
            if (!opt)
                return {};
            auto& val = *opt;
            if (types::is_string(val))
                return ev.string_heap_[types::as_string_idx(val)];
            if (types::is_void(val))
                return {};
            if (types::is_int(val))
                return std::to_string(types::as_int(val));
            if (types::is_bool(val))
                return types::as_bool(val) ? "#t" : "#f";
            return {};
        };

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            std::string code_str;
            if (attempt == 1 || current_code_str.empty()) {
                auto gs = ev.string_heap_.size();
                ev.string_heap_.push_back(goal);
                code_str = call_fn(gen_cid, {types::make_string(gs)});
                llm_call_count++;
                if (code_str.empty()) {
                    ev.timeline_.push_back("attempt_" + std::to_string(attempt) +
                                           ":empty from generator");
                    errors.push_back("empty from generator");
                    error_types.push_back("empty");
                    continue;
                }
            } else {
                if (!has_fixer) {
                    ev.timeline_.push_back("attempt_" + std::to_string(attempt) +
                                           ":no fixer, stopping");
                    break;
                }
                auto cs = ev.string_heap_.size();
                ev.string_heap_.push_back(current_code_str);
                auto es = ev.string_heap_.size();
                ev.string_heap_.push_back(last_error);
                auto gs = ev.string_heap_.size();
                ev.string_heap_.push_back(goal);
                code_str = call_fn(fix_cid, {types::make_string(cs), types::make_string(es),
                                             types::make_string(gs)});
                llm_call_count++;
                if (code_str.empty()) {
                    ev.timeline_.push_back("attempt_" + std::to_string(attempt) +
                                           ":empty from fixer");
                    errors.push_back("empty from fixer");
                    error_types.push_back("empty");
                    continue;
                }
            }
            generated_codes.push_back(code_str);

            auto cv = ev.string_heap_.size();
            ev.string_heap_.push_back(code_str);
            auto ver = call_fn(ver_cid, {types::make_string(cv)});

            if (ver.find("#t") == 0) {
                current_code_str = code_str;
                ev.timeline_.push_back("attempt_" + std::to_string(attempt) + ":success");
                auto result = "#(status:\"ok\" goal:\"" + goal +
                              "\" iterations:" + std::to_string(attempt) + ")";
                auto rs = ev.string_heap_.size();
                ev.string_heap_.push_back(result);

                auto t1 = std::chrono::steady_clock::now();
                auto duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                Evaluator::IntendRecord rec;
                rec.record_id = ev.next_record_id_++;
                rec.strategy_name = strategy_name;
                rec.task_desc = goal;
                rec.success = true;
                rec.attempts = attempt;
                rec.errors = errors;
                rec.error_types = error_types;
                rec.generated_codes = generated_codes;
                rec.llm_call_count = llm_call_count;
                rec.llm_tokens = 0;
                rec.duration_ms = static_cast<std::uint64_t>(duration);
                rec.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
                rec.parent_record_id = 0;
                ev.intend_history_.push_back(rec);
                if (ev.intend_history_.size() > Evaluator::MAX_HISTORY_SIZE)
                    ev.intend_history_.erase(ev.intend_history_.begin());
                restore();
                return types::make_string(rs);
            }

            current_code_str = code_str;
            last_error = ver.empty() ? "verification failed" : ver;
            errors.push_back(last_error);
            error_types.push_back(classify_error(last_error));
            ev.timeline_.push_back("attempt_" + std::to_string(attempt) + ":" + last_error);
        }

        auto t1 = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        Evaluator::IntendRecord rec;
        rec.record_id = ev.next_record_id_++;
        rec.strategy_name = strategy_name;
        rec.task_desc = goal;
        rec.success = false;
        rec.attempts = max_attempts;
        rec.errors = errors;
        rec.error_types = error_types;
        rec.generated_codes = generated_codes;
        rec.llm_call_count = llm_call_count;
        rec.llm_tokens = 0;
        rec.duration_ms = static_cast<std::uint64_t>(duration);
        rec.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
        rec.parent_record_id = 0;
        ev.intend_history_.push_back(rec);
        if (ev.intend_history_.size() > Evaluator::MAX_HISTORY_SIZE)
            ev.intend_history_.erase(ev.intend_history_.begin());

        auto result = "#(status:\"failed\" goal:\"" + goal +
                      "\" iterations:" + std::to_string(max_attempts) + " last-error:\"" +
                      last_error + "\")";
        ev.timeline_.push_back("failed:" + last_error);
        auto rs = ev.string_heap_.size();
        ev.string_heap_.push_back(result);
        restore();
        return types::make_string(rs);
    });
    // ── intend-history — 查询意图执行时间线 ────────────────────
    add("intend-history", [&ev](const auto&) -> EvalValue {
        std::string result;
        for (std::size_t i = 0; i < ev.timeline_.size(); ++i) {
            result += std::to_string(i) + ":" + ev.timeline_[i] + "\n";
        }
        if (result.empty())
            result = "(empty)";
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(result);
        return types::make_string(sidx);
    });

    // ── workspace-state — 汇总当前 workspace 的定义 + 近期 mutations ───
    // Issue #63 Phase 2: a single primitive that gives the LLM
    // (or the EDSL) a snapshot of the workspace. The output is a
    // human-readable string: lines starting with "DEFINE: " list
    // top-level definitions; lines starting with "MUTATION: " list
    // the most recent timeline entries (capped at last 10). The
    // first line is a summary header "WORKSPACE: <n> defines" so
    // the LLM has a one-token extractable count.
    add("workspace-state", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_ || !ev.workspace_pool_) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::string("WORKSPACE-ERROR: no workspace AST loaded"));
            return types::make_string(sidx);
        }
        auto& flat = *ev.workspace_flat_;
        auto& pool = *ev.workspace_pool_;
        std::string out;
        // Top-level defines: walk the flat for Define nodes and
        // collect their bound name.
        std::size_t define_count = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM) {
                out += "DEFINE: " + std::string(pool.resolve(v.sym_id)) + "\n";
                ++define_count;
            }
        }
        if (define_count == 0)
            out += "(no defines)\n";
        // Recent mutations: last 10 timeline entries
        out += "MUTATIONS (last 10):\n";
        if (ev.timeline_.empty()) {
            out += "  (none)\n";
        } else {
            std::size_t start = ev.timeline_.size() > 10 ? ev.timeline_.size() - 10 : 0;
            for (std::size_t i = start; i < ev.timeline_.size(); ++i)
                out += "  " + std::to_string(i) + ":" + ev.timeline_[i] + "\n";
        }
        // Prepend a one-line summary header so the LLM has an
        // easy parse target: "WORKSPACE: <n> defines".
        out = "WORKSPACE: " + std::to_string(define_count) + " defines\n" + out;
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return types::make_string(sidx);
    });

    // ── intend-analytics — 聚合 intend 历史数据 ────────────────
    add("intend-analytics", [&ev](std::span<const EvalValue> a) -> EvalValue {
        std::string filter_strategy;
        std::string filter_field;
        std::string filter_value;
        std::size_t arg_idx = 0;

        if (a.size() > arg_idx && types::is_string(a[arg_idx])) {
            filter_strategy = ev.string_heap_[types::as_string_idx(a[arg_idx])];
            arg_idx++;
        }
        if (a.size() > arg_idx + 2 && types::is_string(a[arg_idx]) &&
            ev.string_heap_[types::as_string_idx(a[arg_idx])] == ":filter") {
            if (types::is_string(a[arg_idx + 1]))
                filter_field = ev.string_heap_[types::as_string_idx(a[arg_idx + 1])];
            if (types::is_string(a[arg_idx + 2]))
                filter_value = ev.string_heap_[types::as_string_idx(a[arg_idx + 2])];
        }

        std::uint64_t total = 0, successes = 0, total_attempts = 0;
        std::uint64_t total_llm_calls = 0, total_duration = 0;
        std::map<std::string, std::uint64_t> error_type_counts;
        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> task_stats;

        for (auto& rec : ev.intend_history_) {
            if (!filter_strategy.empty() && rec.strategy_name != filter_strategy)
                continue;
            if (!filter_field.empty()) {
                if (filter_field == "error-type") {
                    bool matches = false;
                    for (auto& et : rec.error_types) {
                        if (et.find(filter_value) != std::string::npos) {
                            matches = true;
                            break;
                        }
                    }
                    if (!matches)
                        continue;
                } else
                    continue;
            }
            total++;
            if (rec.success)
                successes++;
            total_attempts += rec.attempts;
            total_llm_calls += rec.llm_call_count;
            total_duration += rec.duration_ms;
            for (auto& et : rec.error_types)
                error_type_counts[et]++;
            auto key = rec.task_desc.substr(
                0, std::min<std::size_t>(rec.task_desc.size(), std::size_t{60}));
            auto& ts = task_stats[key];
            ts.second++;
            if (rec.success)
                ts.first++;
        }

        std::string result = "#(analytics";
        result += " total-runs:" + std::to_string(total);
        result += " success-rate:";
        if (total > 0)
            result += std::to_string(static_cast<double>(successes) / static_cast<double>(total));
        else
            result += "0";
        result += " avg-attempts:";
        if (total > 0)
            result +=
                std::to_string(static_cast<double>(total_attempts) / static_cast<double>(total));
        else
            result += "0";
        result += " total-llm-calls:" + std::to_string(total_llm_calls);
        result += " avg-duration-ms:";
        if (total > 0)
            result += std::to_string(total_duration / total);
        else
            result += "0";
        result += " top-errors:(";
        for (auto& [et, count] : error_type_counts) {
            result += " " + et + ":" + std::to_string(count);
        }
        result += ")";
        result += " by-task:(";
        for (auto& [task, stats] : task_stats) {
            result += " (" + task + " " + std::to_string(stats.first) + "/" +
                      std::to_string(stats.second) + ")";
        }
        result += "))";

        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(result);
        return types::make_string(sidx);
    });


    // ── define-strategy — 定义策略 ──────────────────────────
    // Issue #63 Phase 3: accept optional keyword args
    //   :max-attempts <int>     (1..20, default 3)
    //   :temperature <float>   (0.0..1.0, default 0.3)
    //   :sys-prompt-template <str>
    add("define-strategy", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto name = ev.string_heap_[types::as_string_idx(a[0])];
        auto body = ev.string_heap_[types::as_string_idx(a[1])];
        // Optional keyword args (pairs from index 2)
        int new_max = 3;
        double new_temp = 0.3;
        std::string new_spt;
        for (std::size_t i = 2; i + 1 < a.size(); i += 2) {
            // Issue #63 Phase 3: keywords come in as a separate EvalValue
            // type, not strings. Resolve both.
            std::string k;
            if (types::is_string(a[i])) {
                k = ev.string_heap_[types::as_string_idx(a[i])];
            } else if (types::is_keyword(a[i])) {
                auto kidx = types::as_keyword_idx(a[i]);
                if (kidx < ev.keyword_table_.size())
                    k = ev.keyword_table_[kidx];
            } else {
                continue;
            }
            if (k == ":max-attempts" && types::is_int(a[i + 1])) {
                int v = static_cast<int>(types::as_int(a[i + 1]));
                if (v >= 1 && v <= 20)
                    new_max = v;
            } else if (k == ":temperature" &&
                       (types::is_int(a[i + 1]) || types::is_float(a[i + 1]))) {
                double v = types::is_float(a[i + 1]) ? types::as_float(a[i + 1])
                                                     : static_cast<double>(types::as_int(a[i + 1]));
                if (v >= 0.0 && v <= 1.0)
                    new_temp = v;
            } else if (k == ":sys-prompt-template" && types::is_string(a[i + 1])) {
                new_spt = ev.string_heap_[types::as_string_idx(a[i + 1])];
            }
        }
        for (auto& s : ev.strategies_) {
            if (s.name == name) {
                s.body = body;
                s.max_attempts = new_max;
                s.temperature = new_temp;
                s.sys_prompt_template = new_spt;
                return make_bool(true);
            }
        }
        ev.strategies_.push_back({name, body, new_max, new_temp, new_spt, 0, ""});
        return make_bool(true);
    });
    // ── register-strategy! — 注册/更新策略 ──────────────
    add("register-strategy!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto name = ev.string_heap_[types::as_string_idx(a[0])];
        auto body = ev.string_heap_[types::as_string_idx(a[1])];
        for (auto& s : ev.strategies_) {
            if (s.name == name) {
                s.body = body;
                return make_bool(true);
            }
        }
        ev.strategies_.push_back({name, body, 3, 0.3, "", 0, ""});
        return make_bool(true);
    });
    // ── strategy-field — 读取策略字段 ──────────────────────
    // Issue #63 Phase 3: support tunable fields beyond 'body'.
    add("strategy-field", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();
        auto name = ev.string_heap_[types::as_string_idx(a[0])];
        auto field = ev.string_heap_[types::as_string_idx(a[1])];
        for (auto& s : ev.strategies_) {
            if (s.name != name)
                continue;
            if (field == "body") {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(s.body);
                return types::make_string(sid);
            }
            if (field == "max-attempts") {
                return types::make_int(s.max_attempts);
            }
            if (field == "temperature") {
                return types::make_float(s.temperature);
            }
            if (field == "sys-prompt-template") {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(s.sys_prompt_template);
                return types::make_string(sid);
            }
            if (field == "evolution") {
                return types::make_int(s.evolution);
            }
            if (field == "parent") {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(s.parent);
                return types::make_string(sid);
            }
        }
        return make_void();
    });
    // ── strategy-set-field! — 修改策略字段（白名单）───────────
    // Whitelist + range checks per e4_evolvable_strategies.md §1.
    add("strategy-set-field!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto field = ev.string_heap_[types::as_string_idx(a[1])];
        auto name = ev.string_heap_[types::as_string_idx(a[0])];
        for (auto& s : ev.strategies_) {
            if (s.name != name)
                continue;
            if (field == "body" && types::is_string(a[2])) {
                s.body = ev.string_heap_[types::as_string_idx(a[2])];
                return make_bool(true);
            }
            if (field == "max-attempts" && types::is_int(a[2])) {
                int v = static_cast<int>(types::as_int(a[2]));
                if (v < 1 || v > 20)
                    return make_bool(false);
                s.max_attempts = v;
                return make_bool(true);
            }
            if (field == "temperature" && (types::is_int(a[2]) || types::is_float(a[2]))) {
                double v = types::is_float(a[2]) ? types::as_float(a[2])
                                                 : static_cast<double>(types::as_int(a[2]));
                if (v < 0.0 || v > 1.0)
                    return make_bool(false);
                s.temperature = v;
                return make_bool(true);
            }
            if (field == "sys-prompt-template" && types::is_string(a[2])) {
                s.sys_prompt_template = ev.string_heap_[types::as_string_idx(a[2])];
                return make_bool(true);
            }
            // name / evolution / parent are read-only
            return make_bool(false);
        }
        return make_bool(false);
    });
    // ── strategy-inspect — 一键检视 ────────────────────────
    // Issue #63 Phase 3: show all tunable fields + read-only metadata.
    add("strategy-inspect", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto name = ev.string_heap_[types::as_string_idx(a[0])];
        for (auto& s : ev.strategies_) {
            if (s.name == name) {
                std::string result = "#(strategy-inspect";
                result += " name:\"" + s.name + "\"";
                result += " body:\"" + s.body + "\"";
                result += " max-attempts:" + std::to_string(s.max_attempts);
                result += " temperature:" + std::to_string(s.temperature);
                result += " evolution:" + std::to_string(s.evolution);
                result += " parent:\"" + s.parent + "\"";
                result += ")";
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(result);
                return types::make_string(sid);
            }
        }
        return make_void();
    });

    // ── evolve-strategy — 基于 intend-analytics 自动调优策略 ────
    // Issue #63 Phase 3. Was registered in type_checker_impl.cpp but
    // never implemented as a runtime primitive. Implements the
    // heuristics from e4_evolvable_strategies.md §3:
    //
    //   - success-rate < 50% AND avg-attempts = max-attempts
    //       → ↑ max-attempts (add 2, capped at 20)
    //   - success-rate > 90% AND avg-attempts < 1.5
    //       → ↓ max-attempts (sub 1, floored at 1)
    //   - top-error is "unbound-variable" (≥ 2 occurrences)
    //       → sys-prompt-template += "Do NOT use undefined variables.\n"
    //   - top-error is "type-mismatch" (≥ 2 occurrences)
    //       → sys-prompt-template += "Use (check x : Type) before ops.\n"
    //   - top-error is "syntax-error" (≥ 2 occurrences)
    //       → sys-prompt-template += "Check parentheses carefully.\n"
    //
    // Returns the new strategy name (e.g. "adaptive-v2"). The original
    // strategy is left untouched (E4 §3 "保留旧版本 + 探索/利用平衡").
    //
    // Args:
    //   (evolve-strategy <strategy-name>)
    //   (evolve-strategy <strategy-name> <analytics-string>)
    add("evolve-strategy", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto name = ev.string_heap_[types::as_string_idx(a[0])];

        // Find the source strategy
        const Evaluator::StrategyDef* src = nullptr;
        for (auto& s : ev.strategies_) {
            if (s.name == name) {
                src = &s;
                break;
            }
        }
        if (!src)
            return make_void();

        // Get analytics: either passed as 2nd arg, or call
        // intend-analytics ourselves on this strategy.
        std::string analytics;
        if (a.size() >= 2 && types::is_string(a[1])) {
            analytics = ev.string_heap_[types::as_string_idx(a[1])];
        } else {
            auto prim = ev.primitives_.lookup("intend-analytics");
            if (prim) {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(name);
                auto res = (*prim)({types::make_string(sid)});
                if (types::is_string(res))
                    analytics = ev.string_heap_[types::as_string_idx(res)];
            }
        }

        // Parse key fields out of the analytics s-expression.
        // Format: #(analytics total-runs:N success-rate:X avg-attempts:Y
        //          total-llm-calls:N avg-duration-ms:N top-errors:( k:n ...)
        //          by-task:( ... ))
        auto find_after = [](const std::string& s, const std::string& key) -> std::string {
            auto p = s.find(key);
            if (p == std::string::npos)
                return {};
            p += key.size();
            // skip ':' and spaces
            while (p < s.size() && (s[p] == ':' || s[p] == ' '))
                ++p;
            auto end = p;
            while (end < s.size() && s[end] != ' ' && s[end] != ')')
                ++end;
            return s.substr(p, end - p);
        };
        double success_rate = 0.0;
        double avg_attempts = 0.0;
        try {
            auto sr = find_after(analytics, "success-rate");
            if (!sr.empty())
                success_rate = std::stod(sr);
            auto aa = find_after(analytics, "avg-attempts");
            if (!aa.empty())
                avg_attempts = std::stod(aa);
        } catch (...) {
            /* [SILENCE-PRIM-#615] malformed → keep defaults */
        }

        // Parse top-errors: walk "(k1:n1 k2:n2 ...)" inside top-errors:(...)
        std::map<std::string, int> top_errors;
        auto te_pos = analytics.find("top-errors:(");
        if (te_pos != std::string::npos) {
            auto te_end = analytics.find(")", te_pos);
            if (te_end != std::string::npos) {
                auto te = analytics.substr(te_pos, te_end - te_pos);
                std::size_t i = 0;
                while (i < te.size()) {
                    // find pattern "k:n"
                    while (i < te.size() && (te[i] == ' ' || te[i] == '('))
                        ++i;
                    auto kstart = i;
                    while (i < te.size() && te[i] != ':')
                        ++i;
                    if (i >= te.size())
                        break;
                    auto k = te.substr(kstart, i - kstart);
                    ++i; // skip ':'
                    auto nstart = i;
                    while (i < te.size() && te[i] != ' ' && te[i] != ')')
                        ++i;
                    try {
                        top_errors[k] = std::stoi(te.substr(nstart, i - nstart));
                    } catch (...) {
                        // [SILENCE-PRIM-#615] Top-errors array parse failure
                        // leaves slot at its zero-init default; this is
                        // best-effort telemetry extraction, not user input.
                    }
                }
            }
        }

        // Build new strategy (preserve old, copy with bumped fields)
        Evaluator::StrategyDef evolved;
        evolved.name = src->name + "-v" + std::to_string(src->evolution + 1);
        evolved.body = src->body;
        evolved.max_attempts = src->max_attempts;
        evolved.temperature = src->temperature;
        evolved.sys_prompt_template = src->sys_prompt_template;
        evolved.evolution = src->evolution + 1;
        evolved.parent = src->name;

        std::string reason;
        if (success_rate < 0.5 && avg_attempts >= static_cast<double>(src->max_attempts)) {
            int bumped = std::min(20, src->max_attempts + 2);
            evolved.max_attempts = bumped;
            reason = "success-rate " + std::to_string(success_rate).substr(0, 4) +
                     " < 0.5 with avg-attempts=" + std::to_string((int)avg_attempts) +
                     " = max-attempts; bumped " + std::to_string(src->max_attempts) + " → " +
                     std::to_string(bumped);
        } else if (success_rate > 0.9 && avg_attempts < 1.5) {
            int lowered = std::max(1, src->max_attempts - 1);
            evolved.max_attempts = lowered;
            reason = "success-rate > 0.9 with avg-attempts < 1.5; lowered " +
                     std::to_string(src->max_attempts) + " → " + std::to_string(lowered);
        }

        // Add sys-prompt-template hints based on top errors
        auto append_hint = [&](const std::string& key, const std::string& hint) {
            auto it = top_errors.find(key);
            if (it != top_errors.end() && it->second >= 2) {
                evolved.sys_prompt_template += hint;
                if (!reason.empty())
                    reason += "; ";
                reason += "appended hint for " + key;
            }
        };
        append_hint("unbound-variable", "\nDo NOT use undefined variables.");
        append_hint("type-mismatch", "\nUse (check x : Type) before operations.");
        append_hint("div-zero", "\nGuard division with (if (= d 0) ...).");
        append_hint("syntax-error", "\nCheck parentheses carefully.");

        if (reason.empty())
            reason = "no heuristics matched; clone unchanged";

        ev.timeline_.push_back("evolve:" + evolved.name + " from " + src->name + " (" + reason +
                               ")");

        // Insert into ev.strategies_ (avoid name collision: bump suffix)
        std::string final_name = evolved.name;
        for (int bump = 2;; ++bump) {
            bool taken = false;
            for (auto& s : ev.strategies_) {
                if (s.name == final_name) {
                    taken = true;
                    break;
                }
            }
            if (!taken)
                break;
            final_name = evolved.name + "-" + std::to_string(bump);
        }
        evolved.name = final_name;
        ev.strategies_.push_back(evolved);

        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(evolved.name);
        return types::make_string(sid);
    });
}

} // namespace aura::compiler::primitives_detail
