// macro_expansion.cpp — Issue #265: hygienic macro clone + expansion
// aura.compiler.macro_expansion module implementation.

module;

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>                  // Issue #2101: getenv / strtol for runtime depth/pass caps
#include "core/capability_model.hh" // Issue #2023: MacroSelfEvo gate
#include "core/sandbox.hh"          // Issue #2023: is_sandbox_active
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>
#include "observability_metrics.h" // Issue #2021: CompilerMetrics snapshot

module aura.compiler.macro_expansion;

import std;
import aura.core.ast;
import aura.compiler.evaluator_pure;

extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
extern "C" void aura_evaluator_bump_macro_expand_checkpoint_save();
extern "C" std::uint64_t aura_fiber_current_id();
extern "C" int aura_macro_provenance_repin_on_steal(void* ev_ptr, std::uint64_t cloned_marker);

namespace aura::compiler::macro_exp {

namespace detail {

    // Quiet by default: expected MacroSelfEvo denials under Restricted
    // production sandbox spam simple REPL `(+ 1 2)`. Counters still
    // advance; set AURA_VERBOSE=1 for operator diagnostics.
    [[nodiscard]] inline bool macro_self_evo_verbose() noexcept {
        static const bool on = [] {
            const char* e = std::getenv("AURA_VERBOSE");
            return e != nullptr && e[0] != '\0' && e[0] != '0';
        }();
        return on;
    }

    // Issue #965: keep special forms + high-frequency primitives that
    // macros must not gensym. Expanded beyond the original 56-name set
    // so new stdlib-facing builtins (length, vector, make-hash, …) are
    // preserved. Full registry-driven sync is Phase 2 (needs Evaluator
    // at expand time); this set is the offline source of truth.
    const std::unordered_set<std::string>& hygiene_builtins() {
        static const std::unordered_set<std::string> builtins = {
            // Special forms / control
            "if",
            "cond",
            "let",
            "let*",
            "letrec",
            "lambda",
            "define",
            "begin",
            "set!",
            "quote",
            "unquote",
            "quasiquote",
            "case",
            "when",
            "unless",
            "do",
            "delay",
            "force",
            "import",
            "export",
            "module",
            // Pair / list
            "car",
            "cdr",
            "cons",
            "list",
            "list-sort",
            "pair?",
            "null?",
            "list?",
            "length",
            "append",
            "reverse",
            "member",
            "member?",
            "assoc",
            "eq?",
            "equal?",
            "eqv?",
            // Arithmetic / compare
            "+",
            "-",
            "*",
            "/",
            "quotient",
            "remainder",
            "modulo",
            "=",
            "<",
            ">",
            "<=",
            ">=",
            "not",
            "and",
            "or",
            "void",
            "max",
            "min",
            "abs",
            // IO
            "display",
            "write",
            "newline",
            "read",
            "write-file",
            "read-file",
            // Type predicates
            "number?",
            "integer?",
            "float?",
            "boolean?",
            "string?",
            "symbol?",
            "char?",
            "vector?",
            "hash?",
            "procedure?",
            "error?",
            // String
            "string-append",
            "string-length",
            "string-ref",
            "substring",
            "number->string",
            "string->number",
            "string=?",
            "symbol->string",
            "string->symbol",
            // Higher-order / collections
            "apply",
            "map",
            "filter",
            "foldl",
            "foldr",
            "for-each",
            "vector",
            "make-vector",
            "vector-ref",
            "vector-set!",
            "vector-length",
            "make-hash",
            "hash-ref",
            "hash-set!",
            "hash-count",
            // Errors / assert / gensym
            "error",
            "assert",
            "gensym",
            "raise",
            // Eval / meta commonly used in macros
            "eval",
            "current-time",
            "current-time-ms", // Issue #2655
            "monotonic-ms",    // Issue #2655
        };
        return builtins;
    }

} // namespace detail

// Issue #365: MAX_HYGIENE_DEPTH — upper bound on recursive
// clone_macro_body nesting. Exported from macro_expansion.ixx.
// Prevents stack overflow on pathological inputs
// (self-referential macro bodies, deeply nested quasiquote
// chains). Tuned to be much larger than any realistic macro
// body so well-formed programs never hit it; only
// pathological or adversarial inputs trigger the
// graceful-degradation path.

// Issue #2806: recursion depth is an explicit parameter on
// clone_macro_body_at_depth (not TLS / not process-global). TLS
// previously broke under concurrent top-level clones on different
// OS threads only if it were file-static; fiber multiplexing on one
// OS thread can still corrupt a TLS depth counter across yields.
// Depth-as-parameter is correct for both threads and fibers.
// Residual TLS mirror kept only for non-clone diagnostics that still
// observe "last known depth on this OS thread" (not authority).
thread_local int s_hygiene_depth = 0;

// Issue #2023: MacroSelfEvo policy depth for this expand (set by
// macro_expand_all / top-level clone). Always ≤ MAX_HYGIENE_DEPTH.
// Default = hard limit so unconstrained paths keep historical behaviour.
thread_local int s_effective_max_depth = MAX_HYGIENE_DEPTH;
// Issue #2023: when false, rest-param hygiene gensym is skipped.
thread_local bool s_allow_rest_hygiene = true;

// Issue #2243: per-tenant / per-call mirrors of the 3 MacroSelfEvo policy
// knobs (force_hygienic, max_gensym_map_size, max_violations_per_fiber).
// Read inside clone_macro_body fallback paths + name_map insert guards;
// written from TopLevelMacroCapGuard ctor, restored in dtor so recursion
// stays consistent.
thread_local bool s_force_hygienic = false;
thread_local std::uint32_t s_max_gensym_map_size = 0;
thread_local std::uint32_t s_max_violations_per_fiber = 0;
// Issue #2804: process-wide test override for gensym-map ceiling. When
// non-zero, rename_binding_pre / rename_binding use this instead of the
// TLS s_max_gensym_map_size (TopLevelMacroCapGuard overwrites TLS from
// capability each expand). Production always leaves override at 0.
static std::atomic<std::uint32_t> g_test_max_gensym_map_size_override{0};

// Effective gensym-map-size cap (TLS policy or test override).
static std::uint32_t effective_max_gensym_map_size() noexcept {
    const auto o = g_test_max_gensym_map_size_override.load(std::memory_order_relaxed);
    if (o > 0)
        return o;
    return s_max_gensym_map_size;
}

// Issue #2101: process-wide runtime caps (atomics for concurrent set+expand).
// Depth default = hard ceiling (no extra clamp). Pass default = 0 (no clamp).
// Env (read once on first access): AURA_MACRO_HYGIENE_DEPTH_CAP, AURA_MACRO_HYGIENE_PASS_CAP.
static std::atomic<int> g_runtime_hygiene_depth_cap{MAX_HYGIENE_DEPTH};
static std::atomic<int> g_runtime_hygiene_pass_cap{0};
static std::atomic<bool> g_runtime_caps_env_loaded{false};

static void ensure_runtime_caps_env_loaded() noexcept {
    bool expected = false;
    if (!g_runtime_caps_env_loaded.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel))
        return;
    if (const char* env = std::getenv("AURA_MACRO_HYGIENE_DEPTH_CAP")) {
        char* end = nullptr;
        long v = std::strtol(env, &end, 10);
        if (end != env && v >= 1 && v <= MAX_HYGIENE_DEPTH)
            g_runtime_hygiene_depth_cap.store(static_cast<int>(v), std::memory_order_relaxed);
    }
    if (const char* env = std::getenv("AURA_MACRO_HYGIENE_PASS_CAP")) {
        char* end = nullptr;
        long v = std::strtol(env, &end, 10);
        if (end != env && v >= 0 && v <= 1'000'000)
            g_runtime_hygiene_pass_cap.store(static_cast<int>(v), std::memory_order_relaxed);
    }
}

int hard_hygiene_depth_limit() noexcept {
    return MAX_HYGIENE_DEPTH;
}

int runtime_hygiene_depth_cap() noexcept {
    ensure_runtime_caps_env_loaded();
    return g_runtime_hygiene_depth_cap.load(std::memory_order_acquire);
}

int runtime_hygiene_pass_cap() noexcept {
    ensure_runtime_caps_env_loaded();
    return g_runtime_hygiene_pass_cap.load(std::memory_order_acquire);
}

bool set_hygiene_depth_cap(int n) noexcept {
    ensure_runtime_caps_env_loaded();
    // AC1: reject above hard ceiling and n < 1 (no silent raise/zero).
    if (n < 1 || n > MAX_HYGIENE_DEPTH)
        return false;
    g_runtime_hygiene_depth_cap.store(n, std::memory_order_release);
    return true;
}

bool set_hygiene_pass_cap(int n) noexcept {
    ensure_runtime_caps_env_loaded();
    if (n < 0)
        return false;
    g_runtime_hygiene_pass_cap.store(n, std::memory_order_release);
    return true;
}

void reset_hygiene_runtime_caps_for_test() noexcept {
    g_runtime_hygiene_depth_cap.store(MAX_HYGIENE_DEPTH, std::memory_order_release);
    g_runtime_hygiene_pass_cap.store(0, std::memory_order_release);
    // Keep env_loaded=true so tests control caps without env re-apply.
    g_runtime_caps_env_loaded.store(true, std::memory_order_release);
}

// Combine hard ceiling + process runtime + optional capability depth.
// capability_depth<=0 means "no capability clamp" (Off / unconstrained).
static int combine_depth_limit(int capability_depth) noexcept {
    ensure_runtime_caps_env_loaded();
    int lim = MAX_HYGIENE_DEPTH;
    const int rt = g_runtime_hygiene_depth_cap.load(std::memory_order_acquire);
    if (rt > 0 && rt < lim)
        lim = rt;
    if (capability_depth > 0 && capability_depth < lim)
        lim = capability_depth;
    if (lim < 1)
        lim = 1;
    return lim;
}

int effective_hygiene_depth_limit() noexcept {
    ensure_runtime_caps_env_loaded();
    using aura::core::capability::check_macro_self_evo;
    using aura::core::capability::g_capability_registry;
    const auto tenant = g_capability_registry().default_tenant.load();
    const bool sandbox_active = aura::core::sandbox::is_sandbox_active();
    const auto chk = check_macro_self_evo(tenant, sandbox_active, /*wildcard_ok=*/false);
    int cap_depth = 0;
    if (chk.allowed && chk.effective.max_depth > 0)
        cap_depth = static_cast<int>(chk.effective.max_depth);
    // Also honour TLS expand-session bound when nested under expand.
    int tls = s_effective_max_depth;
    if (tls > 0 && tls < MAX_HYGIENE_DEPTH) {
        if (cap_depth <= 0 || tls < cap_depth)
            cap_depth = tls;
    }
    return combine_depth_limit(cap_depth);
}

int effective_hygiene_pass_cap() noexcept {
    ensure_runtime_caps_env_loaded();
    using aura::core::capability::check_macro_self_evo;
    using aura::core::capability::g_capability_registry;
    const auto tenant = g_capability_registry().default_tenant.load();
    const bool sandbox_active = aura::core::sandbox::is_sandbox_active();
    const auto chk = check_macro_self_evo(tenant, sandbox_active, /*wildcard_ok=*/false);
    int lim = 0; // 0 = no extra clamp
    const int rt = g_runtime_hygiene_pass_cap.load(std::memory_order_acquire);
    if (rt > 0)
        lim = rt;
    if (chk.allowed && chk.effective.max_expansion_passes > 0) {
        const int cap = static_cast<int>(chk.effective.max_expansion_passes);
        if (lim <= 0 || cap < lim)
            lim = cap;
    }
    return lim;
}

// Issue #2023: MacroSelfEvo gate counters (also mirrored in capability metrics).
std::atomic<std::uint64_t> g_macro_self_evo_denied_total{0};
std::atomic<std::uint64_t> g_macro_self_evo_allowed_total{0};
std::atomic<std::uint64_t> g_macro_self_evo_pass_clamp_total{0};
std::atomic<std::uint64_t> g_macro_self_evo_depth_clamp_total{0};
// Issue #2243: surfaces for the 3 new MacroSelfEvo enforcement modes
// (force_hygienic deny, gensym-map-size exceeded). Both bumped from the
// enforcement sites in clone_macro_body.
std::atomic<std::uint64_t> g_macro_self_evo_force_hygienic_denied_total{0};
std::atomic<std::uint64_t> g_macro_self_evo_gensym_map_size_exceeded_total{0};
// Issue #2804: clone-walk rename_binding ceiling denials (distinct from
// pre-scan rename_binding_pre bumps of gensym_map_size_exceeded_total).
std::atomic<std::uint64_t> g_clone_walk_gensym_ceiling_exceeded_total{0};
// Issue #2805: dotted-rest force-repair fallback refused to map a
// hygiene_builtins name into name_map (would silently rename builtins).
std::atomic<std::uint64_t> g_dotted_rest_builtin_rename_prevented_total{0};

// Forward decl — body runs under MacroSelfEvo TLS depth policy set by expand entry.
static aura::ast::NodeId macro_expand_all_body(aura::ast::FlatAST& flat,
                                               aura::ast::StringPool& pool, aura::ast::NodeId root,
                                               int max_passes);

// Issue #1245 Phase 1: concurrent hygiene / dirty observability.
std::atomic<std::uint64_t> g_macro_clone_concurrent_fiber_total{0};
std::atomic<std::uint64_t> g_macro_clone_hygiene_dirty_total{0};
// Issue #2806: top-level clone_macro_body entered while another top-level
// clone is already in-flight (g_macro_clone_in_flight was already > 0).
// Counts concurrent top-level depth=0 entries (threads/fibers).
std::atomic<std::uint64_t> g_clone_macro_body_concurrent_top_level_total{0};
// Issue #2807: pre_scan treated unquote-splicing as template scope (would
// gensym caller-scope bindings). Bumped when the boundary is recognized
// and recursion stops (parity with unquote).
std::atomic<std::uint64_t> g_unquote_splicing_hygiene_mismatch_total{0};
// Issue #2021: how many top-level clone_macro_body calls are live
// across threads, and the high-water mark (peak concurrent).
std::atomic<std::uint64_t> g_macro_clone_in_flight{0};
std::atomic<std::uint64_t> g_macro_clone_concurrent_peak{0};

// Issue #1247–#1248 Phase 1: macro-origin provenance + hygiene tracer.
std::atomic<std::uint64_t> g_macro_origin_provenance_errors{0};
std::atomic<std::uint64_t> g_hygiene_tracer_expansions{0};
std::atomic<std::uint64_t> g_hygiene_tracer_depth_max{0};
// Issue #2097: per-fiber hygiene metrics for Agent query under concurrent
// self-evo / fiber-steal. Mutex-guarded; populated only on expand entry/exit
// + violation path bumps (same hot path that already bumps the global atomics).
// Zero-cost-when-not-requested: hash lookups amortized, queries that don't
// pass a fiber_id see nothing. #2018 rest-param gensym-map-size is left at
// 0 here (placeholder for forward compat; future rest-param tracker can
// bump it on pre-scan entry).
namespace {
    inline std::mutex g_fiber_hygiene_mu{};
}
inline std::unordered_map<std::uint32_t, FiberHygieneStats> g_fiber_hygiene_map{};
inline std::atomic<std::uint64_t> g_fiber_hygiene_query_total{0};
inline std::atomic<std::uint64_t> g_fiber_hygiene_violation_per_fiber_total{0};

// Issue #2241: per-fiber hygiene violation budget. When non-zero, an
// Agent / supervisor can deny further expand on fibers that have
// accumulated more than `budget` violations — Agent-throttlable
// surface (refine #2097 FiberHygieneStats). Default 0 = unlimited
// (relaxed-by-default, matches #2228 / #2235 / #2238 pattern). Set
// via AURA_MACRO_SELF_EVO_FIBER_VIOLATION_BUDGET env or
// aura_macro_self_evo_set_fiber_violation_budget(n). Zero-cost early
// out: a single atomic load + branch in clone_macro_body entry.
inline std::atomic<std::uint64_t> g_macro_self_evo_fiber_violation_budget{0};
inline std::atomic<std::uint64_t> g_macro_self_evo_fiber_violation_deny_total{0};

inline void bump_fiber_hygiene_on_enter(std::uint32_t fiber_id, int depth) noexcept {
    std::lock_guard<std::mutex> lock(g_fiber_hygiene_mu);
    g_fiber_hygiene_map[fiber_id].depth = depth;
}
inline void bump_fiber_hygiene_on_violation(std::uint32_t fiber_id) noexcept {
    std::lock_guard<std::mutex> lock(g_fiber_hygiene_mu);
    g_fiber_hygiene_map[fiber_id].violations += 1;
    g_fiber_hygiene_violation_per_fiber_total.fetch_add(1, std::memory_order_relaxed);
}
inline void bump_fiber_hygiene_on_exit(std::uint32_t fiber_id,
                                       std::size_t name_map_size_snapshot = 0u) noexcept {
    // Issue #2097: on exit we don't remove the entry — keep last snapshot
    // for agent diagnostic. depth zeroed so re-entry bumps cleanly.
    // Issue #2241: caller passes name_map->size() (or 0 if no name_map)
    // so gensym_map_size reflects the live hygiene rename occupancy of
    // this expand, not a placeholder zero. Bumped under the same lock
    // as depth/violations so the snapshot is consistent.
    std::lock_guard<std::mutex> lock(g_fiber_hygiene_mu);
    g_fiber_hygiene_map[fiber_id].depth = 0;
    g_fiber_hygiene_map[fiber_id].gensym_map_size = name_map_size_snapshot;
}
FiberHygieneStats get_fiber_hygiene_metrics(std::uint32_t fiber_id) noexcept {
    g_fiber_hygiene_query_total.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fiber_hygiene_mu);
    auto it = g_fiber_hygiene_map.find(fiber_id);
    return it == g_fiber_hygiene_map.end() ? FiberHygieneStats{} : it->second;
}
// Issue #1652: clone_macro_body expand observability counters (paired with
// #1611 MacroIntroduced hygiene gate). Bumped at the success path +
// early-return hygiene-violation paths inside clone_macro_body. Exposed via
// the C-linkage accessor + composed into existing (query:pattern-hygiene-stats)
// primitive surface (no new primitive per #1632 “原语最小化” directive).
std::atomic<std::uint64_t> g_macro_expansion_total{0};
std::atomic<std::uint64_t> g_macro_introduced_nodes_created_total{0};
std::atomic<std::uint64_t> g_hygiene_violation_in_macro_expand_total{0};
// Issue #2018 / #2169: rest-param gensyms applied in clone_macro_body
// pre-scan / rename path (`__rest_<name>_<serial>`). Folded into
// macro-hygiene-stats / reflect:hygiene-stats.
std::atomic<std::uint64_t> g_macro_rest_param_hygiene_total{0};
// Issue #2169: incomplete rest hygiene (rename skipped while policy on).
std::atomic<std::uint64_t> g_macro_rest_param_hygiene_incomplete_total{0};
// Process-wide serial for concurrent fiber uniqueness of rest gensyms.
std::atomic<std::uint64_t> g_macro_rest_gensym_serial{0};
// Issue #2019: post-expand MacroIntroduced generation restamp calls.
std::atomic<std::uint64_t> g_macro_restamp_after_flat_total{0};
// Issue #2096: per-cloned-subtree restamp counter (subtree-local coherence
// at expand exit + critical mutate entry). Bumped when NodeId-rooted
// restamp_macro_introduced_subtree actually repinned ≥1 node.
std::atomic<std::uint64_t> g_macro_expand_mutate_restamp_total{0};
// Issue #2098: per-cloned-subtree schema-cache + dirty/provenance
// stamp counter for the MacroIntroduced clone path. Bumped once per
// MacroIntroduced node that received apply_macro_dirty_bits(cur, kMacroExpansion)
// + set_provenance() stamp in the iterative walk inside clone_macro_body.
// Covers rest-param + nested qq + schema_cache copy paths, giving
// Agents / observability dashboards visibility into the stamping rate
// (previously silent). Surfaces via (query:macro-schema-cache-dirty-stamp-stats).
std::atomic<std::uint64_t> g_macro_schema_cache_dirty_stamped_total{0};
// Issue #2239: per-rest-param + nested-qq breakdown for the schema_cache
// stamping path. Bumped once per rest-param binding that the qq-aware
// pre_scan discovered while walking through a (quasiquote ...) template
// body (Call to 'quasiquote' = recursive scan; Call to 'unquote' = stop).
// Tracks the "rest-param nested in qq" path separately from the existing
// rest_param_hygiene_total counter so Agents can see the nested-qq
// coverage as its own observability dimension. Surfaces via
// (query:macro-schema-cache-dirty-stamp-stats) under
// `rest-param-nested-qq-hits-total`.
std::atomic<std::uint64_t> g_macro_rest_param_nested_qq_hits_total{0};
// Issue #2239: per-rest-list node stamp counter for the freshly
// allocated `(list remaining...)` Call in expand_inner_macros +
// macro_expand_all_body rest-param paths. Bumped once per node in the
// new rest-list spine (the freshly allocated Call + every arg) that
// stamp_rest_param_hygiene applied kMacroExpansion dirty bit +
// set_provenance + copied schema_cache from the source rest binding.
// Mirrors the g_macro_schema_cache_dirty_stamped_total pattern
// (file-level atomic + C-linkage reader). Surfaces via
// (query:macro-schema-cache-dirty-stamp-stats) under
// `schema-cache-rest-stamped-total`.
std::atomic<std::uint64_t> g_macro_schema_cache_rest_stamped_total{0};
// Issue #2808: stamp_rest_param_hygiene also sets SyntaxMarker::MacroIntroduced
// so is_macro_introduced() gates (replace-subtree / rebind) see rest lists.
// set_total = nodes newly marked MacroIntroduced; skipped_total = already
// MacroIntroduced (or out-of-range skip before stamp).
std::atomic<std::uint64_t> g_stamp_rest_param_marker_set_total{0};
std::atomic<std::uint64_t> g_stamp_rest_param_marker_skipped_total{0};
// Issue #2176: selective unstamp for MacroIntroduced subtrees (Agent
// experimental rollback path). Bumped per successful unstamp via the
// C-linkage helper aura_unstamp_macro_introduced_with_counter below.
// Mirrors the g_macro_restamp_after_flat_total pattern (file-level
// atomic bumped by the helper, NOT by the FlatAST method itself — the
// method only updates the per-instance FlatAST member, which the
// snapshot function syncs into CompilerMetrics).
std::atomic<std::uint64_t> g_unstamp_macro_introduced_total{0};
// Issue #2237: agent-driven rollback counter (always bumps on every
// mutate:rollback-macro-introduced call regardless of strict-mode).
// Distinct from g_unstamp_macro_introduced_total which counts the
// nodes actually unstampped; this counts the rollback ops invoked.
// AC2 / AC3: query:macro-hygiene-stats exposes both via dedicated keys.
std::atomic<std::uint64_t> g_rollback_macro_introduced_total{0};
// Issue #2237: strict-audited rollback counter. Bumps only when
// `g_macro_expand_sandbox_strict` is set AND the rollback actually
// unstampped >=1 node. AC4: paired with SecurityEventKind::
// MacroHygieneRollbackOnStrict emit (via append_security_event into
// g_security_event_ring() and SecurityEventWAL if enabled).
std::atomic<std::uint64_t> g_rollback_strict_audited_total{0};
// Issue #2235: process-wide strict-mode flag for cross-FlatAST clone
// hygiene gate. When 0 (default = relaxed mode), the cross-flat
// post-restamp validate is counter-only (no abort, no second-pass
// restamp) — production-safe, just bumps the
// g_hygiene_violation_in_macro_expand_total counter on drift. When 1
// (sandbox-strict / MacroSelfEvo force-hygienic), violations > 0
// trigger a forced second-pass restamp on the target + audit-worthy
// stderr warning. Settable via aura_macro_set_expand_sandbox_strict(1)
// or runtime hook — opt-in for tests + sandbox-strict runtimes, not
// the production default. Mirrors the existing g_* file-atomic pattern
// (file-local atomic + C-linkage reader / setter).
std::atomic<std::uint64_t> g_macro_expand_sandbox_strict{0};

// Issue #1652: C-linkage accessors so the (query:pattern-hygiene-stats)
// primitive can read these file-level atomics from another TU without the
// Evaluator module import (paired pattern with #1648 reflect.hh +
// #1651 macro_expansion.cpp).
// C-linkage readers: atomics live in this namespace (not ::global),
// so use unqualified names — ::g_* fails under modules (#1652/#1757).
extern "C" {
inline std::uint64_t aura_macro_expansion_total_v_read() noexcept {
    return g_macro_expansion_total.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_macro_introduced_nodes_created_total_v_read() noexcept {
    return g_macro_introduced_nodes_created_total.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_hygiene_violation_in_macro_expand_total_v_read() noexcept {
    return g_hygiene_violation_in_macro_expand_total.load(std::memory_order_relaxed);
}
// Non-inline: other TUs (query:macro-hygiene-stats) read this counter.
std::uint64_t aura_macro_rest_param_hygiene_total_v_read() noexcept {
    return g_macro_rest_param_hygiene_total.load(std::memory_order_relaxed);
}
std::uint64_t aura_macro_rest_param_hygiene_incomplete_total_v_read() noexcept {
    return g_macro_rest_param_hygiene_incomplete_total.load(std::memory_order_relaxed);
}
std::uint64_t aura_macro_rest_gensym_serial_v_read() noexcept {
    return g_macro_rest_gensym_serial.load(std::memory_order_relaxed);
}
std::uint64_t aura_macro_restamp_after_flat_total_v_read() noexcept {
    return g_macro_restamp_after_flat_total.load(std::memory_order_relaxed);
}
// Issue #2096: per-cloned-subtree MacroIntroduced restamp counter
// (subtree-local coherence at expand exit + critical mutate entry).
std::uint64_t aura_macro_expand_mutate_restamp_total_v_read() noexcept {
    return g_macro_expand_mutate_restamp_total.load(std::memory_order_relaxed);
}

// Issue #2176: C-linkage helper for the mutate:rollback-macro-introduced
// primitive. Calls FlatAST::unstamp_macro_introduced (the per-instance
// method, which updates the FlatAST member for snapshot) AND bumps the
// file-level g_unstamp_macro_introduced_total (which the C-linkage
// accessor reads for cross-TU query stats). Same pattern as
// restamp_after_expand() for the restamp counter — method updates
// FlatAST member, helper bumps file-level atomic.
extern "C" std::uint64_t aura_unstamp_macro_introduced_with_counter(void* flat_ptr,
                                                                    std::uint32_t root,
                                                                    int keep_provenance) noexcept {
    auto* flat = static_cast<aura::ast::FlatAST*>(flat_ptr);
    if (!flat)
        return 0;
    const auto n =
        flat->unstamp_macro_introduced(static_cast<aura::ast::NodeId>(root), keep_provenance != 0);
    if (n > 0)
        g_unstamp_macro_introduced_total.fetch_add(n, std::memory_order_relaxed);
    return n;
}

// Issue #2176: selective unstamp for MacroIntroduced subtrees (Agent
// experimental rollback path). Bumped per successful unstamp.
std::uint64_t aura_unstamp_macro_introduced_total_v_read() noexcept {
    return g_unstamp_macro_introduced_total.load(std::memory_order_relaxed);
}

// Issue #2237: agent-driven rollback counter reader (AC2 / AC3).
// Always-bump counter — separate from per-node unstampped total so
// ops vs nodes are observable independently. Used by
// query:macro-hygiene-stats under the `rollback-macro-introduced-total`
// key. No-op rollback calls (root=NULL_NODE, out-of-bounds, no
// MacroIntroduced descendants) still bump the counter because the
// op was invoked; the per-node count stays at 0.
extern "C" std::uint64_t aura_rollback_macro_introduced_total_v_read() noexcept {
    return g_rollback_macro_introduced_total.load(std::memory_order_relaxed);
}

// Issue #2237: strict-audited rollback counter reader (AC4). Only
// bumps when `g_macro_expand_sandbox_strict` is set AND at least one
// node was unstampped. Used by query:macro-hygiene-stats under
// `rollback-strict-audited-total`. Should normally be 0 unless the
// sandbox is in strict mode (which is opt-in via
// aura_macro_set_expand_sandbox_strict(1)).
extern "C" std::uint64_t aura_rollback_strict_audited_total_v_read() noexcept {
    return g_rollback_strict_audited_total.load(std::memory_order_relaxed);
}

// Issue #2237: bumpers for the new counters (called from
// mutate:rollback-macro-introduced in evaluator_primitives_mutate.cpp).
// `aura_rollback_macro_introduced_total_bump` always bumps (per-op
// counter); `aura_rollback_strict_audited_total_bump` only bumps
// from the strict-mode branch (after SecurityEvent emit succeeded).
extern "C" void aura_rollback_macro_introduced_total_bump() noexcept {
    g_rollback_macro_introduced_total.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_rollback_strict_audited_total_bump() noexcept {
    g_rollback_strict_audited_total.fetch_add(1, std::memory_order_relaxed);
}
// Issue #2235: C-linkage reader / setter for the cross-FlatAST
// hygiene-gate strict-mode flag (g_macro_expand_sandbox_strict).
// Must be extern "C" so mutate/query TUs can call them without
// module import of macro_expansion internals.
extern "C" std::uint64_t aura_macro_expand_sandbox_strict_v_read() noexcept {
    return g_macro_expand_sandbox_strict.load(std::memory_order_relaxed);
}
extern "C" void aura_macro_set_expand_sandbox_strict(int strict_mode) noexcept {
    g_macro_expand_sandbox_strict.store(strict_mode != 0 ? 1 : 0, std::memory_order_relaxed);
}
// Issue #2098: per-cloned-subtree schema-cache + dirty/provenance
// stamp counter C-linkage reader (clone_macro_body walk visibility).
std::uint64_t aura_macro_schema_cache_dirty_stamped_total_v_read() noexcept {
    return g_macro_schema_cache_dirty_stamped_total.load(std::memory_order_relaxed);
}
// Issue #2239: nested-qq rest-param hits reader (the per-rest-binding
// hit counter discovered by the qq-aware pre_scan while walking
// (quasiquote ...) template bodies). Pairs with
// g_macro_rest_param_nested_qq_hits_total atomic.
std::uint64_t aura_macro_rest_param_nested_qq_hits_total_v_read() noexcept {
    return g_macro_rest_param_nested_qq_hits_total.load(std::memory_order_relaxed);
}
// Issue #2239: per-rest-list node stamp reader. Bumped by
// stamp_rest_param_hygiene (the focused rest-list helper) for every
// node in the freshly allocated `(list remaining...)` spine that
// received kMacroExpansion dirty bit + set_provenance + schema_cache
// copy. Pairs with g_macro_schema_cache_rest_stamped_total atomic.
std::uint64_t aura_macro_schema_cache_rest_stamped_total_v_read() noexcept {
    return g_macro_schema_cache_rest_stamped_total.load(std::memory_order_relaxed);
}
// Issue #2808: rest-list MacroIntroduced marker stamp metrics.
extern "C" std::uint64_t aura_stamp_rest_param_marker_set_total_v_read(void) noexcept {
    return g_stamp_rest_param_marker_set_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_stamp_rest_param_marker_skipped_total_v_read(void) noexcept {
    return g_stamp_rest_param_marker_skipped_total.load(std::memory_order_relaxed);
}
extern "C" void aura_test_reset_stamp_rest_param_marker_totals_for_test(void) noexcept {
    g_stamp_rest_param_marker_set_total.store(0, std::memory_order_relaxed);
    g_stamp_rest_param_marker_skipped_total.store(0, std::memory_order_relaxed);
}
std::uint64_t aura_macro_clone_concurrent_peak_v_read() noexcept {
    return g_macro_clone_concurrent_peak.load(std::memory_order_relaxed);
}
std::uint64_t aura_macro_clone_in_flight_v_read() noexcept {
    return g_macro_clone_in_flight.load(std::memory_order_relaxed);
}
std::uint64_t aura_hygiene_tracer_depth_max_v_read() noexcept {
    return g_hygiene_tracer_depth_max.load(std::memory_order_relaxed);
}
// Issue #2097: per-fiber hygiene query counter (Agent-throttlable).
std::uint64_t aura_fiber_hygiene_query_total_v_read() noexcept {
    return g_fiber_hygiene_query_total.load(std::memory_order_relaxed);
}
std::uint64_t aura_fiber_hygiene_violation_per_fiber_total_v_read() noexcept {
    return g_fiber_hygiene_violation_per_fiber_total.load(std::memory_order_relaxed);
}

// Issue #2241: per-fiber violation budget gate (refine #2097).
// Default 0 = unlimited (production stays permissive until Agent /
// supervisor explicitly enables throttling). Setter / getter are
// simple file-scope atomic wrappers. The check helper reads the
// per-fiber map under the existing `g_fiber_hygiene_mu` lock and
// compares against the budget — bumps `g_macro_self_evo_fiber_violation_deny_total`
// on deny. Test-only reset for hermetic test isolation.
extern "C" void aura_macro_self_evo_set_fiber_violation_budget(std::uint64_t budget) noexcept {
    g_macro_self_evo_fiber_violation_budget.store(budget, std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_macro_self_evo_get_fiber_violation_budget(void) noexcept {
    return g_macro_self_evo_fiber_violation_budget.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_macro_self_evo_fiber_violation_deny_total_v_read(void) noexcept {
    return g_macro_self_evo_fiber_violation_deny_total.load(std::memory_order_relaxed);
}
extern "C" void aura_test_reset_macro_self_evo_fiber_violation_deny_total_for_test(void) noexcept {
    g_macro_self_evo_fiber_violation_deny_total.store(0, std::memory_order_relaxed);
}
// Issue #2243: per-policy self-evo enforcement v_read (refine #2241).
// Counter bump sites live in clone_macro_body (force_hygienic deny
// fallback) and rename_binding_pre (gensym-map-size exceeded gate).
extern "C" std::uint64_t aura_macro_self_evo_force_hygienic_denied_total_v_read(void) noexcept {
    return g_macro_self_evo_force_hygienic_denied_total.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_macro_self_evo_gensym_map_size_exceeded_total_v_read(void) noexcept {
    return g_macro_self_evo_gensym_map_size_exceeded_total.load(std::memory_order_relaxed);
}
// Issue #2804: clone-walk gensym ceiling denials + test hook for map-size cap.
extern "C" std::uint64_t aura_clone_walk_gensym_ceiling_exceeded_total_v_read(void) noexcept {
    return g_clone_walk_gensym_ceiling_exceeded_total.load(std::memory_order_relaxed);
}
extern "C" void aura_test_set_max_gensym_map_size_for_test(std::uint32_t n) noexcept {
    // Process-wide override (not TLS): TopLevelMacroCapGuard rewrites TLS.
    g_test_max_gensym_map_size_override.store(n, std::memory_order_relaxed);
}
extern "C" void aura_test_reset_clone_walk_gensym_ceiling_exceeded_total_for_test(void) noexcept {
    g_clone_walk_gensym_ceiling_exceeded_total.store(0, std::memory_order_relaxed);
}
// Issue #2805: dotted-rest builtin rename prevented metric.
extern "C" std::uint64_t aura_dotted_rest_builtin_rename_prevented_total_v_read(void) noexcept {
    return g_dotted_rest_builtin_rename_prevented_total.load(std::memory_order_relaxed);
}
extern "C" void aura_test_reset_dotted_rest_builtin_rename_prevented_total_for_test(void) noexcept {
    g_dotted_rest_builtin_rename_prevented_total.store(0, std::memory_order_relaxed);
}
// Issue #2806: concurrent top-level clone observability.
extern "C" std::uint64_t aura_clone_macro_body_concurrent_top_level_total_v_read(void) noexcept {
    return g_clone_macro_body_concurrent_top_level_total.load(std::memory_order_relaxed);
}
extern "C" void
aura_test_reset_clone_macro_body_concurrent_top_level_total_for_test(void) noexcept {
    g_clone_macro_body_concurrent_top_level_total.store(0, std::memory_order_relaxed);
}
// Issue #2807: unquote-splicing boundary recognition metric.
extern "C" std::uint64_t aura_unquote_splicing_hygiene_mismatch_total_v_read(void) noexcept {
    return g_unquote_splicing_hygiene_mismatch_total.load(std::memory_order_relaxed);
}
extern "C" void aura_test_reset_unquote_splicing_hygiene_mismatch_total_for_test(void) noexcept {
    g_unquote_splicing_hygiene_mismatch_total.store(0, std::memory_order_relaxed);
}

// Issue #2241: check whether a fiber is allowed to expand given its
// current accumulated violation count and the configured per-fiber
// budget. Returns 1 if deny (budget > 0 AND violations > budget),
// 0 if permit. Zero-cost fast path: budget == 0 or fiber_id == 0 →
// permit without lock acquire. Slow path: shared lock the map,
// compare, optionally bump the deny counter (lock-free atomic).
extern "C" int aura_macro_self_evo_check_fiber_hygiene_budget(std::uint32_t fiber_id) noexcept {
    const std::uint64_t budget =
        g_macro_self_evo_fiber_violation_budget.load(std::memory_order_relaxed);
    // Zero-cost early out: budget 0 = unlimited (relaxed-by-default).
    // fiber_id 0 = no recorded fiber (defensive; bumpers skip fid=0).
    if (budget == 0 || fiber_id == 0)
        return 0;
    std::lock_guard<std::mutex> lock(g_fiber_hygiene_mu);
    auto it = g_fiber_hygiene_map.find(fiber_id);
    if (it == g_fiber_hygiene_map.end())
        return 0; // no recorded expand events → permit
    if (it->second.violations > budget) {
        g_macro_self_evo_fiber_violation_deny_total.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return 0;
}

// Issue #2241: filtered map scan for `query:macro-fiber-hygiene` Agent
// filter surface. Returns the count of fibers in the per-fiber map
// whose accumulated `violations` >= `min_violations` AND `depth` >=
// `min_depth` (zero threshold = no filter on that dimension). Used
// by the primitive to populate the `filtered-entries` key so Agents
// can ask "how many fibers in my workspace are noisy?" without
// iterating the map themselves. C-linkage wrapper required because
// `g_fiber_hygiene_mu` lives in an anonymous namespace and is not
// visible across TUs (the primitive in evaluator_primitives_obs_eval.cpp
// can read atomics via inline `extern` symbols but cannot lock this
// mutex directly). Single lock acquire covers the full scan; the
// resulting count is a snapshot under that lock (subsequent mutations
// may already be in flight — best-effort observability, same contract
// as the existing query counters).
extern "C" std::uint64_t
aura_macro_self_evo_count_fibers_meeting_filter(std::uint64_t min_violations,
                                                int min_depth) noexcept {
    std::lock_guard<std::mutex> lock(g_fiber_hygiene_mu);
    std::uint64_t count = 0;
    for (const auto& [fid, stats] : g_fiber_hygiene_map) {
        (void)fid;
        if (stats.violations >= min_violations && stats.depth >= min_depth)
            ++count;
    }
    return count;
}
std::uint64_t aura_macro_clone_concurrent_fiber_total_v_read() noexcept {
    return g_macro_clone_concurrent_fiber_total.load(std::memory_order_relaxed);
}

// Issue #2021: mirror live macro-hygiene atomics into CompilerMetrics
// so query / Guard / Agent dashboards see peak depth + concurrent peak.
// `metrics_ptr` is CompilerMetrics* (void* avoids module import here).
void aura_macro_hygiene_snapshot_metrics(void* metrics_ptr) noexcept {
    if (!metrics_ptr)
        return;
    auto* m = static_cast<CompilerMetrics*>(metrics_ptr);
    m->hygiene_tracer_depth_max.store(g_hygiene_tracer_depth_max.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
    m->hygiene_tracer_expansions.store(g_hygiene_tracer_expansions.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
    m->macro_origin_provenance_errors.store(
        g_macro_origin_provenance_errors.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    // #1245 flag was a constant 1; now mirror the live fiber-stamp total.
    m->macro_clone_concurrent_hygiene.store(
        g_macro_clone_concurrent_fiber_total.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    m->macro_clone_concurrent_peak.store(
        g_macro_clone_concurrent_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m->macro_clone_in_flight.store(g_macro_clone_in_flight.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
    m->macro_rest_param_hygiene_total.store(
        g_macro_rest_param_hygiene_total.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    m->macro_restamp_after_flat_total.store(
        g_macro_restamp_after_flat_total.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    // Issue #2235: mirror cross-FlatAST hygiene violation counter so
    // (query:macro-provenance-stats) and (engine:metrics) views see
    // the production always-validate rate. The file-level atomic
    // remains the canonical source; this is just the snapshot copy.
    m->macro_hygiene_violation_in_macro_expand_total.store(
        g_hygiene_violation_in_macro_expand_total.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    // Issue #2096: mirror per-cloned-subtree restamp too (used by
    // (query:macro-mutate-restamp-stats) and (engine:metrics) views).
    m->macro_expand_mutate_restamp_total.store(
        g_macro_expand_mutate_restamp_total.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    // Issue #2098: mirror per-cloned-subtree schema-cache + dirty/provenance
    // stamp counter too (used by (query:macro-schema-cache-dirty-stamp-stats)
    // and (engine:metrics) views for rest-param + nested qq visibility).
    m->macro_schema_cache_dirty_stamped_total.store(
        g_macro_schema_cache_dirty_stamped_total.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    // Issue #2097: mirror per-fiber hygiene query counters into CompilerMetrics
    // for (engine:metrics) views + (query:macro-fiber-hygiene) primitive.
    m->fiber_hygiene_query_total.store(g_fiber_hygiene_query_total.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
    m->fiber_hygiene_violation_per_fiber_total.store(
        g_fiber_hygiene_violation_per_fiber_total.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
}
} // extern "C"

// Forward decl — definition later in this TU (after restamp helpers).
static void ensure_cross_flat_expand_consistency(aura::ast::FlatAST& target,
                                                 aura::ast::StringPool& target_pool,
                                                 aura::ast::FlatAST& source,
                                                 aura::ast::StringPool& source_pool,
                                                 aura::ast::NodeId new_root = aura::ast::NULL_NODE);

// Issue #2235: C-linkage test helper that invokes
// `ensure_cross_flat_expand_consistency` directly. The function is
// `static` (file-local in this TU) so it's not exposed via module
// import. Returns the post-call violation count (== 0 in healthy
// cross-flat clone; the first-pass restamp auto-clears the bit on
// every MacroIntroduced node), or UINT64_MAX on a bad arg. Used by
// tests/compiler/test_macro_cross_flat_hygiene.cpp AC1-AC4.
extern "C" std::uint64_t
aura_test_cross_flat_expand_consistency(void* target_flat, void* target_pool, void* source_flat,
                                        void* source_pool, std::uint32_t new_root) noexcept {
    auto* target_f = static_cast<aura::ast::FlatAST*>(target_flat);
    auto* target_p = static_cast<aura::ast::StringPool*>(target_pool);
    auto* source_f = static_cast<aura::ast::FlatAST*>(source_flat);
    auto* source_p = static_cast<aura::ast::StringPool*>(source_pool);
    if (!target_f || !target_p || !source_f || !source_p)
        return std::numeric_limits<std::uint64_t>::max();
    ensure_cross_flat_expand_consistency(*target_f, *target_p, *source_f, *source_p,
                                         static_cast<aura::ast::NodeId>(new_root));
    return static_cast<std::uint64_t>(target_f->validate_macro_hygiene_invariants());
}

// Issue #2235: C-linkage test helper that bumps the cross-FlatAST
// hygiene violation counter (so the wire-up + query-surface +
// CompilerMetrics mirror can be tested without forcing an actual
// production-side drift scenario — the file-level atomic IS the
// canonical source, the bump here just exercises the snapshot + query
// path that reads it). Bumps g_hygiene_violation_in_macro_expand_total
// by `n`. Returns the post-bump value.
extern "C" std::uint64_t
aura_test_bump_hygiene_violation_in_macro_expand(std::uint64_t n) noexcept {
    return g_hygiene_violation_in_macro_expand_total.fetch_add(n, std::memory_order_relaxed) + n;
}

extern "C" void aura_test_set_macro_expand_sandbox_strict(int v) noexcept {
    aura_macro_set_expand_sandbox_strict(v);
}
extern "C" std::uint64_t aura_test_macro_expand_sandbox_strict_v_read() noexcept {
    return aura_macro_expand_sandbox_strict_v_read();
}

// Issue #2019: restamp MacroIntroduced gens after a successful expand
// pass so FlatAST consumers (mutate / query / JIT) never see stale gen.
// Issue #2096: when `new_root` is provided, also run the NodeId-rooted
// restamp_macro_introduced_subtree on the cloned/expanded subtree so
// the immediately-introduced MacroIntroduced closure gets a per-subtree
// generation+parent+dirty repair without the AST-wide scan cost.
// `new_root` defaults NULL_NODE for backward compat with callers that
// don't track the just-cloned root.
static void restamp_after_expand(aura::ast::FlatAST& flat,
                                 aura::ast::NodeId new_root = aura::ast::NULL_NODE) {
    const auto n = flat.restamp_macro_introduced_generations();
    if (n > 0)
        g_macro_restamp_after_flat_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #2096: per-cloned-subtree restamp (subtree-local coherence).
    if (new_root != aura::ast::NULL_NODE) {
        const auto m = flat.restamp_macro_introduced_subtree(new_root);
        if (m > 0)
            g_macro_expand_mutate_restamp_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #2171: when a clone_macro_body invocation crosses FlatAST +
// StringPool boundaries (target.flat != source.flat OR target.pool !=
// source.pool), force a restamp on the target + bump the
// g_macro_restamp_after_flat_total counter so subsequent mutate /
// query / JIT consumers see consistent marker / provenance /
// kMacroExpansion bits even when the source pool's generation counter
// differs from the target's. Single-flat clones are a no-op (no perf
// regression on the hot in-flat path used by `macro_expand_all` and
// the closure-materialization path).
//
// Issue #2235: production-grade cross-FlatAST clone_macro_body
// hygiene gate (replaces the `#ifndef NDEBUG` + abort path added by
// #2171). In production builds (was silent corruption risk for Agents
// that materialize macros into a different workspace FlatAST), always
// runs validate_macro_hygiene_invariants() post-restamp. On > 0
// violations:
//   - bump g_hygiene_violation_in_macro_expand_total by `violations`
//     (so Agents / observability dashboards / query:macro-provenance-stats
//     see the production always-validate rate).
//   - log a stderr warning (CI / runtime visible, no abort — the
//     restamp + counter is the recovery contract).
//   - if `g_macro_expand_sandbox_strict` is set (sandbox-strict mode):
//     force a second-pass restamp on the target (full AST restamp +
//     subtree restamp) so any residual MacroIntroduced drift is
//     guaranteed re-stamped. Relaxed mode leaves the first-pass
//     restamp + counter bump as the only signal — production does
//     NOT abort (no crash; the restamp + counter + sandbox-strict
//     forced-restamp is the complete recovery contract).
// The two restamp passes (first-call + strict-mode second-call) both
// bump g_macro_restamp_after_flat_total, giving Agents a clean
// per-cross-flat-clone signal via query:macro-provenance-stats
// `cross-flat-restamp-after-total`.
//
// Single-flat clones remain an early-return (AC4 zero-perf-regression
// contract preserved — the hot in-flat path used by macro_expand_all
// is unaffected).
static void ensure_cross_flat_expand_consistency(aura::ast::FlatAST& target,
                                                 aura::ast::StringPool& target_pool,
                                                 aura::ast::FlatAST& source,
                                                 aura::ast::StringPool& source_pool,
                                                 aura::ast::NodeId new_root) {
    const bool cross_flat = (&target != &source) || (&target_pool != &source_pool);
    if (!cross_flat)
        return;                             // AC4: single-flat path stays zero-overhead.
    restamp_after_expand(target, new_root); // #2171 first-pass restamp.
    // Issue #2235: production always-on validate (was #ifndef NDEBUG).
    // Normal-case: restamp auto-clears kMacroExpansion bit on every
    // MacroIntroduced node, so post-restamp violations == 0. Bumps
    // the violation counter only when drift IS present (corruption /
    // partial-restamp case — a real bug signal).
    const auto violations = target.validate_macro_hygiene_invariants();
    if (violations == 0)
        return;
    g_hygiene_violation_in_macro_expand_total.fetch_add(violations, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[#2235 cross-flat hygiene] post-restamp violations=%zu "
                 "target.size()=%zu new_root=%u sandbox_strict=%u\n",
                 violations, target.size(), static_cast<unsigned>(new_root),
                 static_cast<unsigned>(g_macro_expand_sandbox_strict.load()));
    // AC2: strict mode fails closed via forced second-pass restamp +
    // audit-worthy stderr warning. Relaxed mode is counter-only
    // (no abort, no second-pass).
    if (g_macro_expand_sandbox_strict.load() != 0) {
        const auto n = target.restamp_macro_introduced_generations();
        if (n > 0)
            g_macro_restamp_after_flat_total.fetch_add(1, std::memory_order_relaxed);
        if (new_root != aura::ast::NULL_NODE) {
            const auto m = target.restamp_macro_introduced_subtree(new_root);
            if (m > 0)
                g_macro_expand_mutate_restamp_total.fetch_add(1, std::memory_order_relaxed);
        }
        std::fprintf(stderr,
                     "[#2235 strict-mode forced restamp] target.size()=%zu "
                     "new_root=%u (Audit: cross-flat hygiene violation forced restamp)\n",
                     target.size(), static_cast<unsigned>(new_root));
    }
}

// Issue #2806: internal recursive entry with explicit depth (not TLS).
// Public clone_macro_body(...) is a depth=0 wrapper.
static aura::ast::NodeId clone_macro_body_at_depth(
    aura::ast::FlatAST& target, aura::ast::StringPool& target_pool, aura::ast::FlatAST& source,
    aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId, aura::core::TransparentStringHash,
                             std::equal_to<>>* subst,
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                       std::equal_to<>>* name_map,
    aura::ast::SyntaxMarker cloned_marker, int hygiene_depth);

aura::ast::NodeId clone_macro_body(
    aura::ast::FlatAST& target, aura::ast::StringPool& target_pool, aura::ast::FlatAST& source,
    aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId, aura::core::TransparentStringHash,
                             std::equal_to<>>* subst,
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                       std::equal_to<>>* name_map,
    aura::ast::SyntaxMarker cloned_marker) {
    // Issue #2806: public API is always top-level depth=0.
    return clone_macro_body_at_depth(target, target_pool, source, source_pool, body_id, subst,
                                     name_map, cloned_marker, /*hygiene_depth=*/0);
}

static aura::ast::NodeId clone_macro_body_at_depth(
    aura::ast::FlatAST& target, aura::ast::StringPool& target_pool, aura::ast::FlatAST& source,
    aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId, aura::core::TransparentStringHash,
                             std::equal_to<>>* subst,
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                       std::equal_to<>>* name_map,
    aura::ast::SyntaxMarker cloned_marker, int hygiene_depth) {
    using namespace aura::ast;
    // Issue #2806: residual TLS mirror for diagnostics only (not authority).
    s_hygiene_depth = hygiene_depth;
    // Issue #2171: capture cross-flat status at top-level entry so the
    // success-path exit can call ensure_cross_flat_expand_consistency()
    // exactly once per top-level clone (recursive calls walk the same
    // target/source so all recursions share the cross_flat status; we
    // only need to restamp + counter-bump once for the whole subtree).
    // Issue #2806: use explicit hygiene_depth (not TLS) so concurrent
    // top-level clones never mis-detect cross_flat_top.
    const bool cross_flat_top =
        (hygiene_depth == 0) && ((&target != &source) || (&target_pool != &source_pool));
    // Issue #1652: per-call success-path observability bump (fired once per
    // clone_macro_body invocation that survives the early-return hygiene
    // checks). The per-node count (clone_macro_introduced_nodes_created) is
    // deferred to #1688 along with the clone_macro_body recursive-walk
    // refactor that threads the cumulative count through the AST walk.
    g_macro_expansion_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #2021: track concurrent top-level clone occupancy (not recursive
    // re-entries on the same thread). Peak is visible via query/metrics.
    struct ConcurrentCloneGuard {
        bool armed = false;
        std::uint32_t captured_fiber_id = 0;
        // Issue #2241: capture name_map* by value — dtor cannot touch
        // enclosing function parameters (C++ local-class rule).
        const std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                 std::equal_to<>>* name_map_ptr = nullptr;
        ConcurrentCloneGuard(
            const std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                     std::equal_to<>>* nm,
            int depth) noexcept
            : name_map_ptr(nm) {
            // Issue #2806: arm only for explicit top-level depth.
            if (depth != 0)
                return;
            armed = true;
            const auto prev = g_macro_clone_in_flight.fetch_add(1, std::memory_order_relaxed);
            // Issue #2806: another top-level clone already in flight.
            if (prev > 0)
                g_clone_macro_body_concurrent_top_level_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            const auto n = prev + 1;
            auto peak = g_macro_clone_concurrent_peak.load(std::memory_order_relaxed);
            while (n > peak && !g_macro_clone_concurrent_peak.compare_exchange_weak(
                                   peak, n, std::memory_order_relaxed)) {
            }
            // Issue #2097: capture fiber-id for per-fiber hygiene snapshot.
            // Top-level entry only (nested recursion on the same thread
            // already inherited depth from the outermost call). The per-fiber
            // map is keyed by fiber-id, so two concurrent top-level expand
            // calls on different fibers get independent snapshots (AC1).
            captured_fiber_id = aura_fiber_current_id();
            if (captured_fiber_id != 0)
                bump_fiber_hygiene_on_enter(captured_fiber_id, depth);
        }
        ~ConcurrentCloneGuard() noexcept {
            if (armed) {
                g_macro_clone_in_flight.fetch_sub(1, std::memory_order_relaxed);
                // Issue #2097: zero per-fiber depth on exit (violations
                // persist for agent diagnostic; depth re-bumps on entry).
                // Issue #2241: snapshot the live name_map occupancy so
                // gensym_map_size reflects the rename footprint of this expand.
                if (captured_fiber_id != 0) {
                    const std::size_t nm_size = name_map_ptr ? name_map_ptr->size() : 0u;
                    bump_fiber_hygiene_on_exit(captured_fiber_id, nm_size);
                }
            }
        }
        ConcurrentCloneGuard(const ConcurrentCloneGuard&) = delete;
        ConcurrentCloneGuard& operator=(const ConcurrentCloneGuard&) = delete;
    } concurrent_guard{name_map, hygiene_depth};
    // Issue #2023: top-level clone entry also consults MacroSelfEvo so
    // direct clone_macro_body (without macro_expand_all) cannot bypass
    // the sandbox. Nested recursion skips the check.
    struct TopLevelMacroCapGuard {
        bool armed = false;
        int prev_depth = MAX_HYGIENE_DEPTH;
        bool prev_rest = true;
        explicit TopLevelMacroCapGuard(int depth) noexcept {
            if (depth != 0)
                return;
            using aura::core::capability::check_macro_self_evo;
            using aura::core::capability::g_capability_registry;
            const auto tenant = g_capability_registry().default_tenant.load();
            const bool sandbox_active = aura::core::sandbox::is_sandbox_active();
            const auto chk = check_macro_self_evo(tenant, sandbox_active, /*wildcard_ok=*/false);
            if (!chk.allowed) {
                g_macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
                // Signal denial to outer scope via depth sentinel.
                s_effective_max_depth = -1;
                return;
            }
            // Issue #2241: per-fiber hygiene violation budget gate
            // (refine #2097 FiberHygieneStats). When the budget is
            // non-zero AND the current fiber's recorded violations
            // exceed it, deny further expand on this fiber. The
            // helper bumps g_macro_self_evo_fiber_violation_deny_total
            // on deny and reads the per-fiber map under g_fiber_hygiene_mu.
            // Default budget=0 → unlimited (zero-cost early-out path
            // inside the helper, no map lookup). Threads into the
            // existing MacroSelfEvo sentinel machinery so the outer
            // `if (top_cap_guard.denied())` check returns NULL_NODE
            // without any new state.
            const std::uint32_t cur_fid = aura_fiber_current_id();
            if (aura_macro_self_evo_check_fiber_hygiene_budget(cur_fid) != 0) {
                if (detail::macro_self_evo_verbose()) {
                    std::fprintf(stderr,
                                 "[#2241 MacroSelfEvo] clone_macro_body denied: "
                                 "fiber %u exceeded hygiene violation budget "
                                 "(no clone work performed)\n",
                                 cur_fid);
                }
                s_effective_max_depth = -1;
                return;
            }
            armed = true;
            prev_depth = s_effective_max_depth;
            prev_rest = s_allow_rest_hygiene;
            // Issue #2101: capability may only tighten further under the
            // process-wide runtime depth cap (never raise past hard/runtime).
            {
                int cap_d = 0;
                if (chk.effective.max_depth > 0)
                    cap_d = static_cast<int>(chk.effective.max_depth);
                const int d = combine_depth_limit(cap_d);
                // Only tighten if expand_all has not already set a tighter bound.
                if (s_effective_max_depth == MAX_HYGIENE_DEPTH || d < s_effective_max_depth)
                    s_effective_max_depth = d;
            }
            s_allow_rest_hygiene = chk.effective.allow_rest_hygiene;
            // Issue #2243: thread 3 new MacroSelfEvo knobs through for the
            // duration of this expand + push the per-fiber violation budget
            // into the C-linkage helper used by the per-fiber hygiene gate.
            s_force_hygienic = chk.effective.force_hygienic;
            s_max_gensym_map_size = chk.effective.max_gensym_map_size;
            s_max_violations_per_fiber = chk.effective.max_violations_per_fiber;
            aura_macro_self_evo_set_fiber_violation_budget(s_max_violations_per_fiber);
        }
        ~TopLevelMacroCapGuard() noexcept {
            if (!armed)
                return;
            s_effective_max_depth = prev_depth;
            s_allow_rest_hygiene = prev_rest;
            // Issue #2243: restore the 3 new knobs to default-off so nested
            // expand / macro_expand_all doesn't leak outer-tenant limits.
            s_force_hygienic = false;
            s_max_gensym_map_size = 0;
            s_max_violations_per_fiber = 0;
            aura_macro_self_evo_set_fiber_violation_budget(0);
        }
        TopLevelMacroCapGuard(const TopLevelMacroCapGuard&) = delete;
        TopLevelMacroCapGuard& operator=(const TopLevelMacroCapGuard&) = delete;
        [[nodiscard]] bool denied() const noexcept { return s_effective_max_depth < 0; }
    } top_cap_guard{hygiene_depth};
    if (top_cap_guard.denied()) {
        if (detail::macro_self_evo_verbose()) {
            std::fprintf(stderr,
                         "[#2023 MacroSelfEvo] clone_macro_body denied: capability not granted "
                         "(no clone work performed)\n");
        }
        s_effective_max_depth = MAX_HYGIENE_DEPTH; // restore after deny sentinel
        return NULL_NODE;
    }
    // Issue #365 / #2806: depth guard uses explicit hygiene_depth
    // (public API starts at 0; recursion passes depth+1). When depth
    // exceeds MAX_HYGIENE_DEPTH, degrade gracefully with NULL_NODE.
    // Warning once per top-level call (hygiene_depth==0 resets flag).
    static thread_local bool s_warned_this_call = false;
    if (hygiene_depth == 0) {
        s_warned_this_call = false;
    }
    // Issue #2023 / #2101: honour min(hard, runtime cap, TLS/capability).
    const int depth_limit =
        combine_depth_limit((s_effective_max_depth > 0 && s_effective_max_depth < MAX_HYGIENE_DEPTH)
                                ? s_effective_max_depth
                                : 0);
    if (hygiene_depth >= depth_limit) {
        if (!s_warned_this_call) {
            s_warned_this_call = true;
            // Issue #1247: include macro-origin provenance in the diagnostic
            // so Agents can locate which MacroIntroduced path blew the depth.
            g_macro_origin_provenance_errors.fetch_add(1, std::memory_order_relaxed);
            // Issue #1652: paired bump — depth exceeded is a hygiene violation
            // against the macro expand contract. Bump the new g_* counter.
            g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2097: per-fiber hygiene violation snapshot (zero-cost
            // when not requested; live fiber-id here gives per-fiber attribution).
            bump_fiber_hygiene_on_violation(aura_fiber_current_id());
            if (depth_limit < MAX_HYGIENE_DEPTH)
                g_macro_self_evo_depth_clamp_total.fetch_add(1, std::memory_order_relaxed);
            const char* origin = (cloned_marker == aura::ast::SyntaxMarker::MacroIntroduced)
                                     ? "MacroIntroduced"
                                     : "User";
            std::fprintf(stderr,
                         "[#365/#1247/#2023/#2101/#2806 warning] clone_macro_body exceeded "
                         "depth_limit=%d (hard MAX_HYGIENE_DEPTH=%d runtime_cap=%d); "
                         "marker=%s depth=%d "
                         "[MacroIntroduced provenance path]; falling back to "
                         "unhygienic substitution (original name).\n",
                         depth_limit, MAX_HYGIENE_DEPTH, runtime_hygiene_depth_cap(), origin,
                         hygiene_depth);
        }
        // Issue #2243: if force_hygienic is set, deny instead of silently
        // falling back to the unhygienic (original-name) substitution path on
        // depth-limit. Bumps force_hygienic_denied_total + macro_origin
        // provenance error counter; outer call returns NULL_NODE.
        if (s_force_hygienic) {
            g_macro_self_evo_force_hygienic_denied_total.fetch_add(1, std::memory_order_relaxed);
            g_macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
            g_macro_origin_provenance_errors.fetch_add(1, std::memory_order_relaxed);
            bump_fiber_hygiene_on_violation(aura_fiber_current_id());
            return NULL_NODE;
        }
        return NULL_NODE;
    }
    // Issue #1248: hygiene provenance tracer — track max depth + expansions.
    {
        auto cur = static_cast<std::uint64_t>(hygiene_depth);
        auto prev = g_hygiene_tracer_depth_max.load(std::memory_order_relaxed);
        while (cur > prev && !g_hygiene_tracer_depth_max.compare_exchange_weak(
                                 prev, cur, std::memory_order_relaxed)) {
        }
    }
    if (body_id == NULL_NODE || body_id >= source.size()) {
        // Issue #1652: paired bump — invalid body_id is a hygiene violation
        // (caller passed an out-of-range NodeId for the macro body).
        g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #2097: per-fiber hygiene violation snapshot.
        bump_fiber_hygiene_on_violation(aura_fiber_current_id());
        // Issue #2243: force_hygienic elevates invalid-body to a deny
        // instead of silent NULL_NODE; outermost caller returns NULL with
        // sentinel.
        if (s_force_hygienic) {
            g_macro_self_evo_force_hygienic_denied_total.fetch_add(1, std::memory_order_relaxed);
            g_macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
            g_macro_origin_provenance_errors.fetch_add(1, std::memory_order_relaxed);
        }
        return NULL_NODE;
    }
    auto v = source.get(body_id);

    // Variable substitution: if this variable is a macro param, return the arg clone.
    //
    // Issue #120: the arg's NodeId is in the *calling* FlatAST (= target),
    // not in `source` (the macro definition's FlatAST). The recursive
    // call to clone_macro_body with body_id=it->second would try to
    // read it->second from `source`, which is wrong (NodeId indices
    // are per-FlatAST). The fix: detect this case and return the
    // arg's NodeId as-is, then recursively clone its children from
    // `target` (not `source`).
    //
    // Issue #2169: template-introduced *rest* gensyms in name_map shadow
    // macro-param subst so nested `(lambda (... . rest) rest)` does not
    // capture the outer rest list wrap. Ordinary template bindings keep
    // pre-#2169 subst-first semantics (free macro-param uses).
    if (subst && v.tag == NodeTag::Variable && v.sym_id != INVALID_SYM) {
        auto name = std::string(source_pool.resolve(v.sym_id));
        bool shadowed_by_rest_gensym = false;
        if (name_map) {
            auto mit = name_map->find(name);
            if (mit != name_map->end() && mit->second.rfind("__rest_", 0) == 0)
                shadowed_by_rest_gensym = true;
        }
        if (!shadowed_by_rest_gensym) {
            auto it = subst->find(name);
            if (it != subst->end()) {
                // Issue #334 follow-up: REVERTED Quote-wrap from commit
                // 6b90641. The Quote-wrap made Variables in macro
                // bodies evaluate to the literal arg value (helped
                // define-struct), but it broke `set!` semantics in
                // normal macros: the set! target became a literal
                // symbol (the arg name) instead of the caller's
                // variable, causing test_issue_137/190 to fail. The
                // original AST subst (returning the arg NodeId
                // directly) is restored for now. The proper fix for
                // #230 #1 (define-struct) is the env-binding path
                // (tracked in issue 334), not Quote-wrap.
                return it->second;
            }
        }
    }

    // Re-intern SymIds: resolve in source_pool, intern in target_pool
    auto transplant = [&](SymId sid) -> SymId {
        return (sid == INVALID_SYM) ? sid
                                    : target_pool.intern(std::string(source_pool.resolve(sid)));
    };

    // Resolve a name through name_map (hygiene: renamed binding)
    auto resolve_name = [&](SymId sid) -> std::string {
        if (sid == INVALID_SYM)
            return "";
        auto name = std::string(source_pool.resolve(sid));
        if (name_map) {
            auto it = name_map->find(name);
            if (it != name_map->end())
                return it->second;
        }
        return name;
    };

    // Rename a binding position for hygiene: gensym if macro-introduced
    std::uint64_t hyg_ctr = 0; // Issue #265: per-call counter
    auto rename_binding_pre = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        // Ordinary template bindings: keep free macro-param names substitutable
        // (subst wins). Builtins keep their name.
        if ((subst && subst->count(name)) || detail::hygiene_builtins().count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        // Issue #2243: gensym-map-size ceiling enforcement. If the granted
        // policy sets a non-zero max_gensym_map_size, deny the expansion here
        // (bump counter + sentinel) instead of letting name_map balloon under
        // adversarial macros. Empty name_map (pre-scan not run yet) is
        // allowed.
        // Issue #2804: use effective_max_gensym_map_size (TLS or test override).
        const auto gensym_cap = effective_max_gensym_map_size();
        if (gensym_cap > 0 && name_map && name_map->size() >= gensym_cap) {
            g_macro_self_evo_gensym_map_size_exceeded_total.fetch_add(1, std::memory_order_relaxed);
            g_macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
            g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);
            bump_fiber_hygiene_on_violation(aura_fiber_current_id());
            return aura::ast::NULL_NODE;
        }
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Issue #2018 / #2169: rest-param binding hygiene. Dotted Lambda /
    // MacroDef last params ALWAYS get a process-unique
    // `__rest_<name>_<serial>` gensym (unless MacroSelfEvo disables via
    // s_allow_rest_hygiene). name_map is filled in pre-scan so body uses
    // resolve via resolve_name / rename_binding. Free uses of the *macro
    // formal* rest still hit Variable subst when not shadowed by a
    // template rest binding of the same source name.
    auto rename_rest_binding_pre = [&](SymId sid) -> SymId {
        // No name_map → caller opted into untracked clone (not a violation).
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        // Issue #2023: MacroSelfEvo policy may disable rest hygiene (opt-out).
        if (!s_allow_rest_hygiene)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        if (detail::hygiene_builtins().count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end()) {
            // Already mapped — ensure rest-shaped gensym; repair if not.
            if (it->second.rfind("__rest_", 0) != 0) {
                const auto serial =
                    g_macro_rest_gensym_serial.fetch_add(1, std::memory_order_relaxed);
                auto fresh = std::string("__rest_") + name + "_" + std::to_string(serial);
                it->second = fresh;
                g_macro_rest_param_hygiene_total.fetch_add(1, std::memory_order_relaxed);
                g_macro_rest_param_hygiene_incomplete_total.fetch_add(1, std::memory_order_relaxed);
                return target_pool.intern(fresh);
            }
            return target_pool.intern(it->second);
        }
        // Process-wide serial — concurrent fiber expand cannot collide.
        const auto serial = g_macro_rest_gensym_serial.fetch_add(1, std::memory_order_relaxed);
        auto fresh = std::string("__rest_") + name + "_" + std::to_string(serial);
        (*name_map)[name] = fresh;
        g_macro_rest_param_hygiene_total.fetch_add(1, std::memory_order_relaxed);
        return target_pool.intern(fresh);
    };

    // Issue #120: pre-scan the body to populate name_map BEFORE cloning.
    // The body may reference gensym'd bindings (e.g., `(let ((tmp a)) (set! b tmp))`
    // — the inner `tmp` Variable reference must see the gensym'd name
    // when it's cloned). Without the pre-scan, the recursive clone
    // would process the inner `tmp` (as a Variable reference) before the
    // let binding is processed (which is what gensym's `tmp`).
    //
    // Issue #2018: Lambda / MacroDef with dotted rest — last param is a
    // rest binding; gensym via rename_rest_binding_pre (`__rest_` prefix).
    if (name_map) {
        // Issue #2239 / #2807: qq-aware pre_scan. Tracks `(quasiquote ...)`,
        // `(unquote ...)`, and `(unquote-splicing ...)` boundaries so
        // rest-param bindings nested inside qq templates get gensym'd
        // (template scope), while unquote / unquote-splicing inner
        // expressions are NOT gensym'd (caller scope). Also bumps
        // g_macro_rest_param_nested_qq_hits_total whenever a dotted
        // Lambda/MacroDef is discovered inside qq (qq_depth > 0).
        std::function<void(NodeId, int)> pre_scan = [&](NodeId nid, int qq_depth) {
            if (nid == NULL_NODE || nid >= source.size())
                return;
            auto nv = source.get(nid);
            // Quasiquote / unquote / unquote-splicing boundary detection.
            // Encoded as Call nodes with Variable heads (no dedicated
            // NodeTag). Quasiquote → recurse into first arg with deeper
            // qq_depth; unquote / unquote-splicing → stop recursion
            // (caller scope, no gensym). Issue #2807: `,@x` must match
            // unquote, not fall through to generic child walk.
            if (nv.tag == NodeTag::Call && !nv.children.empty()) {
                auto callee_n = source.get(nv.child(0));
                if (callee_n.tag == NodeTag::Variable) {
                    auto cname = std::string(source_pool.resolve(callee_n.sym_id));
                    if (cname == "quasiquote" && nv.children.size() >= 2) {
                        // Entering qq body: bump depth + recurse ONLY
                        // into the qq template (first arg).
                        pre_scan(nv.child(1), qq_depth + 1);
                        return;
                    }
                    if (cname == "unquote") {
                        // Boundary: do NOT recurse into unquote inner.
                        // Bindings inside unquote live in the caller's
                        // scope and must NOT be gensym'd by the macro.
                        return;
                    }
                    // Issue #2807: unquote-splicing (`,@x`) is also caller
                    // scope — same stop as unquote. Without this, pre_scan
                    // walks the splice body at template qq_depth and
                    // gensyms bindings that should evaluate in the caller.
                    if (cname == "unquote-splicing") {
                        g_unquote_splicing_hygiene_mismatch_total.fetch_add(
                            1, std::memory_order_relaxed);
                        return;
                    }
                }
            }
            // If this node is a binding position, gensym its name
            // (into the name_map) but don't generate any target node.
            if (nv.tag == NodeTag::Let || nv.tag == NodeTag::LetRec || nv.tag == NodeTag::Define) {
                rename_binding_pre(nv.sym_id);
            } else if (nv.tag == NodeTag::Lambda) {
                const bool dotted = nv.int_value != 0;
                const auto nparams = nv.params.size();
                for (std::size_t i = 0; i < nparams; ++i) {
                    if (dotted && i + 1 == nparams)
                        rename_rest_binding_pre(nv.params[i]);
                    else
                        rename_binding_pre(nv.params[i]);
                }
                // Issue #2239: track rest-param bindings discovered
                // inside nested qq templates as their own observability
                // dimension (separate from the flat rest-param counter).
                if (dotted && qq_depth > 0)
                    g_macro_rest_param_nested_qq_hits_total.fetch_add(1, std::memory_order_relaxed);
            } else if (nv.tag == NodeTag::MacroDef) {
                // Nested macro defs inside a template: rest bit is bit 0 of
                // int_value (same encoding as add_macrodef).
                const bool dotted = (nv.int_value & 1) != 0;
                const auto nparams = nv.params.size();
                for (std::size_t i = 0; i < nparams; ++i) {
                    if (dotted && i + 1 == nparams)
                        rename_rest_binding_pre(nv.params[i]);
                    else
                        rename_binding_pre(nv.params[i]);
                }
                if (dotted && qq_depth > 0)
                    g_macro_rest_param_nested_qq_hits_total.fetch_add(1, std::memory_order_relaxed);
            }
            std::vector<aura::ast::NodeId> scan_children(nv.children.begin(), nv.children.end());
            for (auto c : scan_children)
                pre_scan(c, qq_depth);
        };
        pre_scan(body_id, /*qq_depth=*/0);
    }

    auto rename_binding = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        // Prefer pre-scan / rest renames first so template-introduced
        // rest bindings (name_map) win over a same-named macro param.
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        // Macro params, builtins keep their name (free uses → Variable subst)
        if ((subst && subst->count(name)) || detail::hygiene_builtins().count(name))
            return transplant(sid);
        // Issue #2804: gensym-map-size ceiling — parity with rename_binding_pre.
        // Clone walk can still allocate names pre-scan missed or refused;
        // without this check max_gensym_map_size is only half-enforced.
        const auto gensym_cap = effective_max_gensym_map_size();
        if (gensym_cap > 0 && name_map->size() >= gensym_cap) {
            g_macro_self_evo_gensym_map_size_exceeded_total.fetch_add(1, std::memory_order_relaxed);
            g_clone_walk_gensym_ceiling_exceeded_total.fetch_add(1, std::memory_order_relaxed);
            g_macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
            g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);
            bump_fiber_hygiene_on_violation(aura_fiber_current_id());
            return aura::ast::NULL_NODE;
        }
        // Gensym! Create fresh name and track in name_map
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Clone children recursively (pass cloned_marker through).
    // Issue #483: snapshot child NodeIds before recursion — recursive
    // clone / set_child on either flat can replace a parent's
    // PersistentChildVector and invalidate v.children spans.
    std::vector<aura::ast::NodeId> child_ids;
    std::vector<aura::ast::NodeId> source_children;
    {
        auto fresh = source.get(body_id);
        source_children.assign(fresh.children.begin(), fresh.children.end());
    }
    for (auto cid : source_children) {
        // Issue #365 / #2806: pass depth+1 into the recursive frame
        // (explicit parameter; siblings share the same depth).
        child_ids.push_back(clone_macro_body_at_depth(target, target_pool, source, source_pool, cid,
                                                      subst, name_map, cloned_marker,
                                                      hygiene_depth + 1));
    }

    // Clone params (for Lambda nodes) — with hygienic renaming.
    // Issue #2018: rest (dotted last) already mapped via pre-scan
    // rename_rest_binding_pre; rename_binding prefers name_map.
    std::vector<aura::ast::SymId> param_syms;
    for (auto pid : v.params)
        param_syms.push_back(rename_binding(pid));

    aura::ast::NodeId new_id = NULL_NODE;
    switch (v.tag) {
        case NodeTag::LiteralInt:
            new_id = target.add_literal(v.int_value);
            break;
        case NodeTag::LiteralString:
            new_id = target.add_literalstring(transplant(v.sym_id));
            break;
        case NodeTag::Variable: {
            // Hygienic: check name_map for renamed bindings
            if (name_map) {
                auto name = resolve_name(v.sym_id);
                new_id = target.add_variable(target_pool.intern(name));
            } else {
                new_id = target.add_variable(transplant(v.sym_id));
            }
            break;
        }
        case NodeTag::Call: {
            std::vector<aura::ast::NodeId> args(child_ids.begin() + 1, child_ids.end());
            if (!child_ids.empty())
                new_id = target.add_call(child_ids[0], args);
            break;
        }
        case NodeTag::IfExpr:
            if (child_ids.size() >= 3)
                new_id = target.add_if(child_ids[0], child_ids[1], child_ids[2]);
            break;
        case NodeTag::Lambda:
            // Issue #2018 / #2169: preserve dotted rest flag (int_value != 0).
            // Fallback: if dotted rest param did not get a __rest_ gensym
            // while hygiene is on, force-repair + incomplete counter.
            // Issue #2805: never map hygiene_builtins names (let, list, …)
            // into name_map — pre-scan already skips them; overwriting would
            // rename free Variable uses of those builtins in the body.
            if (!child_ids.empty()) {
                const bool dotted = v.int_value != 0;
                if (dotted && !param_syms.empty() && name_map && s_allow_rest_hygiene) {
                    auto rest_name = std::string(target_pool.resolve(param_syms.back()));
                    if (rest_name.rfind("__rest_", 0) != 0) {
                        const std::string src_nm =
                            !v.params.empty() ? std::string(source_pool.resolve(v.params.back()))
                                              : rest_name;
                        // Issue #2805: builtins stay unmapped (parity with
                        // rename_rest_binding_pre / rename_binding_pre).
                        if (detail::hygiene_builtins().count(src_nm) ||
                            detail::hygiene_builtins().count(rest_name)) {
                            g_dotted_rest_builtin_rename_prevented_total.fetch_add(
                                1, std::memory_order_relaxed);
                            // Keep param_syms.back() as the transplanted
                            // original name — do not force __rest_fb_ gensym.
                        } else {
                            const auto serial =
                                g_macro_rest_gensym_serial.fetch_add(1, std::memory_order_relaxed);
                            auto fresh = std::string("__rest_fb_") + rest_name + "_" +
                                         std::to_string(serial);
                            param_syms.back() = target_pool.intern(fresh);
                            if (!v.params.empty())
                                (*name_map)[src_nm] = fresh;
                            g_macro_rest_param_hygiene_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                            g_macro_rest_param_hygiene_incomplete_total.fetch_add(
                                1, std::memory_order_relaxed);
                            g_hygiene_violation_in_macro_expand_total.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                }
                new_id = target.add_lambda(param_syms, child_ids[0], /*dotted=*/dotted);
            }
            break;
        case NodeTag::Let:
        case NodeTag::LetRec:
            if (child_ids.size() >= 2)
                new_id =
                    (v.tag == NodeTag::Let)
                        ? target.add_let(rename_binding(v.sym_id), child_ids[0], child_ids[1])
                        : target.add_letrec(rename_binding(v.sym_id), child_ids[0], child_ids[1]);
            break;
        case NodeTag::Begin:
            if (!child_ids.empty())
                new_id = target.add_begin(child_ids);
            break;
        case NodeTag::Set:
            if (!child_ids.empty()) {
                // Issue #120: if the set! target is a macro param, look
                // up the arg and use ITS sym_id (resolved from target).
                // Otherwise the set! would target the macro param
                // (e.g., "a") which isn't bound in the calling env.
                SymId set_name_sid = transplant(v.sym_id);
                if (subst) {
                    auto set_name = std::string(source_pool.resolve(v.sym_id));
                    auto sit = subst->find(set_name);
                    if (sit != subst->end()) {
                        auto arg_v = target.get(sit->second);
                        if (arg_v.tag == NodeTag::Variable) {
                            set_name_sid = arg_v.sym_id;
                        }
                    }
                }
                new_id = target.add_set(set_name_sid, child_ids[0]);
            }
            break;
        case NodeTag::Quote:
            if (!child_ids.empty())
                new_id = target.add_quote(child_ids[0]);
            break;
        case NodeTag::Define:
            if (!child_ids.empty())
                new_id = target.add_define(rename_binding(v.sym_id), child_ids[0]);
            break;
        default:
            break;
    }

    if (new_id != NULL_NODE) {
        // Issue #190: use the caller's specified marker
        // (MacroIntroduced for macro expansion, User for closure
        // materialization). The recursive calls already set the
        // marker on each child node, so this is just the outer
        // wrapper node.

        // Issue #1908: force repin on MacroIntroduced clone (per #1908 AC).
        // The bridge hook routes through the active Evaluator (ev_ptr fallback
        // for module-aware call sites) and bumps both per-CompilerMetrics counters
        // + the file-level atomic fallback (covers module-unaware call sites
        // like clone_macro_body). This is the "强制 repin 在 MacroIntroduced 路径"
        // half of the #1908 improvement pseudocode.
        if (cloned_marker == aura::ast::SyntaxMarker::MacroIntroduced) {
            (void)aura_macro_provenance_repin_on_steal(nullptr,
                                                       static_cast<std::uint64_t>(cloned_marker));
        }
        target.set_marker(new_id, cloned_marker);
        target.set_loc(new_id, v.line, v.col);
        // Issue #390: populate the per-node schema
        // cache. Pre-#390 the type checker had to
        // re-infer the type of every macro-cloned
        // node from scratch (the cloned body had
        // no pre-computed type). Post-#390 we copy
        // the source node's schema_cache (or
        // type_id_ as a fallback) into the cloned
        // node's schema_cache column, so the type
        // checker can use it as a cache hit signal
        // and avoid the re-inference. The
        // (compile:schema-cache-stats) Aura
        // primitive reports the hit rate.
        if (source.schema_cache(body_id) != 0) {
            target.set_schema_cache(new_id, source.schema_cache(body_id));
        } else if (source.type_id(body_id) != 0) {
            target.set_schema_cache(new_id, source.type_id(body_id));
        }
        // Issue #290: also OR kMacroExpansion into the
        // macro_dirty_ bitmask on every node in the cloned
        // subtree (root + descendants). Single hook point for
        // ALL clone_macro_body callers (eval_flat top-level,
        // expand_inner_macros for nested, evaluator_eval_flat
        // closure materialization). We condition on
        // cloned_marker == MacroIntroduced so the
        // closure-materialization call site (which passes
        // User) doesn't accidentally trip the dirty bit.
        // Iterative walk via std::vector stack — no
        // recursion, safe for pathological depth.
        //
        // Issue #1891: also stamp expansion provenance at clone
        // time (not only fiber restamp #1612) so AST→IR lowering
        // sees non-zero provenance for MacroIntroduced nodes and
        // blame / JIT hygiene can correlate back to the expansion
        // origin without waiting for steal/GC repair.
        if (cloned_marker == aura::ast::SyntaxMarker::MacroIntroduced) {
            if (aura_evaluator_mutation_boundary_depth() > 0)
                aura_evaluator_bump_macro_expand_checkpoint_save();
            // Issue #1245 Phase 1: fiber-aware hygiene provenance counter for
            // concurrent clone_macro_body (steal/GC/hot-swap contexts).
            // Full dirty-to-fiber peel follows; metric makes the path visible.
            const auto fiber_id = aura_fiber_current_id();
            if (fiber_id != 0)
                g_macro_clone_concurrent_fiber_total.fetch_add(1, std::memory_order_relaxed);
            g_macro_clone_hygiene_dirty_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #1248: hygiene tracer expansion count (MacroIntroduced stamps).
            g_hygiene_tracer_expansions.fetch_add(1, std::memory_order_relaxed);
            (void)fiber_id;
            // Prefer source body provenance; else weak-link to source body id
            // (same weak pattern as fiber restamp #1612 / #1891).
            const std::uint32_t src_prov = source.provenance(body_id);
            const std::uint32_t origin =
                src_prov != 0 ? src_prov : static_cast<std::uint32_t>(body_id == 0 ? 1 : body_id);
            std::vector<aura::ast::NodeId> stack;
            stack.push_back(new_id);
            while (!stack.empty()) {
                auto cur = stack.back();
                stack.pop_back();
                if (cur == aura::ast::NULL_NODE)
                    continue;
                target.apply_macro_dirty_bits(
                    cur, static_cast<std::uint8_t>(
                             aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion));
                // Issue #2098: bump per-cloned-subtree schema-cache +
                // dirty/provenance stamp counter — bumped per MacroIntroduced
                // node that received kMacroExpansion dirty bit in this walk,
                // giving observability into the stamping rate across rest-param
                // + nested qq + schema_cache copy paths (the existing iterative
                // stamp walk was previously silent in metrics).
                g_macro_schema_cache_dirty_stamped_total.fetch_add(1, std::memory_order_relaxed);
                if (target.provenance(cur) == 0)
                    target.set_provenance(cur, origin);
                auto cv = target.get(cur);
                std::vector<aura::ast::NodeId> walk_children(cv.children.begin(),
                                                             cv.children.end());
                for (auto child : walk_children) {
                    if (child != aura::ast::NULL_NODE)
                        stack.push_back(child);
                }
            }
        }
    }
    // Issue #2171: cross-flat top-level clones must restamp + counter-bump
    // before returning so the freshly introduced MacroIntroduced subtree
    // leaves consistent marker / provenance / kMacroExpansion bits in
    // the target even when source pool's generation counter differs.
    // Single-flat and recursive frames are no-op (perf-stable for the
    // hot in-flat path used by macro_expand_all).
    if (cross_flat_top && new_id != NULL_NODE) {
        ensure_cross_flat_expand_consistency(target, target_pool, source, source_pool, new_id);
    }
    return new_id;
}

namespace detail {

    aura::ast::NodeId unwrap_cons_chain_to_call(
        aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root,
        const std::unordered_map<std::string, MacroExpansionDef, aura::core::TransparentStringHash,
                                 std::equal_to<>>& macros) {
        using namespace aura::ast;
        if (root == NULL_NODE)
            return NULL_NODE;
        auto v = flat->get(root);
        if (v.tag != NodeTag::Call || v.children.size() != 3)
            return NULL_NODE;
        auto callee_v = flat->get(v.child(0));
        if (callee_v.tag != NodeTag::Variable)
            return NULL_NODE;
        auto callee_name = std::string(pool->resolve(callee_v.sym_id));
        if (callee_name != "cons")
            return NULL_NODE;
        // First arg must be (quote <known-macro-sym>)
        auto arg0_v = flat->get(v.child(1));
        if (arg0_v.tag != NodeTag::Quote || arg0_v.children.empty())
            return NULL_NODE;
        auto quoted_v = flat->get(arg0_v.child(0));
        if (quoted_v.tag != NodeTag::Variable)
            return NULL_NODE;
        auto quoted_name = std::string(pool->resolve(quoted_v.sym_id));
        if (macros.find(quoted_name) == macros.end())
            return NULL_NODE;
        // Walk the cdr chain (v.child(2)) to collect arg NodeIds.
        // Each step: cdr is (cons <arg> <rest>) or (quote ()).
        std::vector<NodeId> args;
        NodeId cdr_id = v.child(2);
        while (cdr_id != NULL_NODE) {
            auto cdr_v = flat->get(cdr_id);
            if (cdr_v.tag == NodeTag::Quote) {
                // (quote ()) — end of list
                break;
            }
            if (cdr_v.tag != NodeTag::Call || cdr_v.children.size() != 3) {
                // Not a cons cell — bail
                return NULL_NODE;
            }
            auto c_callee = flat->get(cdr_v.child(0));
            if (c_callee.tag != NodeTag::Variable ||
                std::string(pool->resolve(c_callee.sym_id)) != "cons") {
                return NULL_NODE;
            }
            // Push the arg (cdr_v.child(1))
            args.push_back(cdr_v.child(1));
            cdr_id = cdr_v.child(2);
        }
        // Build Call(<quoted_name>, args...)
        auto macro_var = flat->add_variable(pool->intern(quoted_name));
        flat->set_marker(macro_var, SyntaxMarker::MacroIntroduced);
        return flat->add_call(macro_var, args);
    }

} // namespace detail

// Issue #2239: focused rest-list hygiene stamp. Applied to the
// freshly allocated `(list remaining...)` Call in expand_inner_macros
// + macro_expand_all_body. Multi-pass expansion can re-introduce
// free uses of rest names without restamping the new list_call —
// without this stamp the new nodes are missing kMacroExpansion +
// provenance + schema_cache, so type checking re-infers and downstream
// hygiene gates (MutationBoundaryGuard etc.) don't see the new
// rest-list. Iterative walk via std::vector stack (same shape as the
// existing clone_macro_body stamp loop at #2098).
static inline void stamp_rest_param_hygiene(aura::ast::FlatAST& target,
                                            const aura::ast::FlatAST& source,
                                            aura::ast::NodeId src_body_id,
                                            aura::ast::NodeId list_root) {
    using namespace aura::ast;
    if (list_root == NULL_NODE || list_root >= target.size())
        return;
    const std::uint32_t src_prov = source.provenance(src_body_id);
    const std::uint32_t origin =
        src_prov != 0 ? src_prov : static_cast<std::uint32_t>(src_body_id == 0 ? 1 : src_body_id);
    const std::uint64_t src_schema = source.schema_cache(src_body_id);
    std::vector<NodeId> stack;
    stack.push_back(list_root);
    while (!stack.empty()) {
        auto cur = stack.back();
        stack.pop_back();
        if (cur == NULL_NODE || cur >= target.size())
            continue;
        target.apply_macro_dirty_bits(
            cur, static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion));
        if (target.provenance(cur) == 0)
            target.set_provenance(cur, origin);
        if (src_schema != 0)
            target.set_schema_cache(cur, src_schema);
        // Issue #2808 / #142: rest-list spine is macro-introduced. Without
        // set_marker, is_macro_introduced() stays false and mutate gates
        // (replace-subtree / rebind) cannot reject rest-stamped nodes.
        if (target.is_macro_introduced(cur)) {
            g_stamp_rest_param_marker_skipped_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            target.set_marker(cur, SyntaxMarker::MacroIntroduced);
            g_stamp_rest_param_marker_set_total.fetch_add(1, std::memory_order_relaxed);
        }
        g_macro_schema_cache_rest_stamped_total.fetch_add(1, std::memory_order_relaxed);
        auto cv = target.get(cur);
        std::vector<NodeId> walk_children(cv.children.begin(), cv.children.end());
        for (auto child : walk_children) {
            if (child != NULL_NODE && child < target.size())
                stack.push_back(child);
        }
    }
}

// Issue #2808: test entry for static stamp_rest_param_hygiene (opaque FlatAST*).
extern "C" void aura_test_call_stamp_rest_param_hygiene(void* target_flat, void* source_flat,
                                                        std::uint32_t src_body_id,
                                                        std::uint32_t list_root) noexcept {
    if (!target_flat || !source_flat)
        return;
    stamp_rest_param_hygiene(*static_cast<aura::ast::FlatAST*>(target_flat),
                             *static_cast<const aura::ast::FlatAST*>(source_flat),
                             static_cast<aura::ast::NodeId>(src_body_id),
                             static_cast<aura::ast::NodeId>(list_root));
}

aura::ast::NodeId expand_inner_macros(
    aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root, int depth,
    int max_depth,
    const std::unordered_map<std::string, MacroExpansionDef, aura::core::TransparentStringHash,
                             std::equal_to<>>& macros) {
    using namespace aura::ast;
    if (root == NULL_NODE || depth >= max_depth)
        return root;
    // Issue #158: unwrap qq-built cons chains whose head is a
    // known macro. Without this, `(bar ,x)` inside a macro body
    // stays as `(cons (quote bar) ...)` after expand_qq, and the
    // main macro check below (which expects a Call head matching
    // a known macro) misses it.
    if (auto unwrapped = detail::unwrap_cons_chain_to_call(flat, pool, root, macros);
        unwrapped != NULL_NODE) {
        // Substitute the unwrapped Call for the original cons chain
        // at the parent's child slot, then recurse.
        auto parent_id = flat->parent_of(root);
        if (parent_id != NULL_NODE) {
            auto parent_v = flat->get(parent_id);
            std::vector<aura::ast::NodeId> parent_children(parent_v.children.begin(),
                                                           parent_v.children.end());
            for (std::uint32_t ci = 0; ci < parent_children.size(); ++ci) {
                if (parent_children[ci] == root) {
                    flat->set_child(parent_id, ci, unwrapped);
                    flat->restamp_all_node_generations();
                    break;
                }
            }
        }
        // Recurse into the unwrapped Call (which is now a real
        // macro call site).
        return expand_inner_macros(flat, pool, unwrapped, depth, max_depth, macros);
    }
    {
        auto v = flat->get(root);
        if (v.tag == NodeTag::Call && !v.children.empty()) {
            std::vector<aura::ast::NodeId> call_args(v.children.begin(), v.children.end());
            auto callee_v = flat->get(call_args[0]);
            if (callee_v.tag == NodeTag::Variable) {
                auto cname = std::string(pool->resolve(callee_v.sym_id));
                auto it = macros.find(cname);
                if (it != macros.end()) {
                    // Build substitution: macro param → arg NodeId.
                    // Issue #146 follow-up: route through the pure helper
                    // so the substitution logic lives in evaluator_pure.ixx
                    // (single source of truth) and the legacy inline loop
                    // goes away. call_args is snapshotted above (Issue #483)
                    // so set_child during clone/expand cannot UAF v.children.
                    const auto& md = it->second;
                    auto subst = aura::compiler::pure::compute_macro_subst_pure(
                        md.params, call_args, md.dotted);
                    // Issue #2018: rest params on inner macros — build
                    // (list remaining...) into subst (same as eval_flat
                    // hygienic path).
                    if (md.dotted && !md.params.empty()) {
                        const std::size_t regular_count = md.params.size() - 1;
                        std::vector<aura::ast::NodeId> remaining;
                        for (std::size_t ai = regular_count + 1; ai < call_args.size(); ++ai)
                            remaining.push_back(call_args[ai]);
                        auto list_var = flat->add_variable(pool->intern("list"));
                        auto list_call = flat->add_call(list_var, remaining);
                        subst[md.params.back()] = list_call;
                        // Issue #2239: focused rest-list hygiene stamp —
                        // apply kMacroExpansion + provenance +
                        // schema_cache copy from the source rest
                        // binding (macro body in the source flat) to
                        // every node in the freshly allocated
                        // `(list remaining...)` spine. Without this,
                        // multi-pass expand_inner_macros can
                        // re-introduce free uses of rest names
                        // without restamping the new list_call.
                        stamp_rest_param_hygiene(*flat, *md.flat, md.body_id, list_call);
                    }
                    // Clone the macro body into the current flat and
                    // re-intern sym_ids. Use the runtime registry's
                    // `flat` / `pool` pointers as the source.
                    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                       std::equal_to<>>
                        rename_map;
                    auto* src_pool = md.pool ? md.pool : pool;
                    auto cloned = clone_macro_body(*flat, *pool, *md.flat, *src_pool, md.body_id,
                                                   &subst, &rename_map);
                    if (cloned == NULL_NODE)
                        return root;
                    // Recursively expand inner macros in the cloned body
                    cloned = expand_inner_macros(flat, pool, cloned, depth + 1, max_depth, macros);
                    // Rewrite the parent's child to use the cloned body
                    auto parent_id = flat->parent_of(root);
                    if (parent_id != NULL_NODE) {
                        auto parent_v = flat->get(parent_id);
                        std::vector<aura::ast::NodeId> parent_children(parent_v.children.begin(),
                                                                       parent_v.children.end());
                        for (std::uint32_t ci = 0; ci < parent_children.size(); ++ci) {
                            if (parent_children[ci] == root) {
                                flat->set_child(parent_id, ci, cloned);
                                // Issue #2019: set_child bumps generation_;
                                // restamp MacroIntroduced so cloned body
                                // matches surrounding AST gen.
                                // Issue #2096: also run per-cloned-subtree
                                // restamp on the freshly introduced subtree.
                                restamp_after_expand(*flat, cloned);
                                break;
                            }
                        }
                    } else {
                        // Root-level expand: still restamp MacroIntroduced.
                        // Issue #2096: also restamp the cloned subtree root.
                        restamp_after_expand(*flat, cloned);
                    }
                    return cloned;
                }
            }
        }
    }
    // Not a macro call — recurse into children.
    // Issue #483: snapshot child ids; recursive set_child on this
    // node (or descendants) replaces PersistentChildVector storage.
    std::vector<aura::ast::NodeId> child_ids;
    {
        auto rv = flat->get(root);
        child_ids.assign(rv.children.begin(), rv.children.end());
    }
    for (auto child : child_ids)
        (void)expand_inner_macros(flat, pool, child, depth + 1, max_depth, macros);
    return root;
}
aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                   aura::ast::NodeId root, int max_passes) {
    using namespace aura::ast;
    // Issue #2023: MacroSelfEvo capability gate — consult before any clone work.
    // Sandbox Off → permissive (no clamp). Strict / Restricted+active without
    // grant → reject with clear error and return root unchanged.
    {
        using aura::core::capability::check_macro_self_evo;
        using aura::core::capability::g_capability_effect_metrics;
        using aura::core::capability::g_capability_registry;
        const auto tenant = g_capability_registry().default_tenant.load();
        const bool sandbox_active = aura::core::sandbox::is_sandbox_active();
        const auto chk = check_macro_self_evo(tenant, sandbox_active, /*wildcard_ok=*/false);
        if (!chk.allowed) {
            g_macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
            // Expected under Restricted without MacroSelfEvo grant — quiet
            // unless AURA_VERBOSE=1 (metrics still count denials).
            if (detail::macro_self_evo_verbose()) {
                std::fprintf(stderr,
                             "[#2023 MacroSelfEvo] macro_expand_all denied: %s "
                             "(no clone work performed)\n",
                             chk.deny_reason ? chk.deny_reason : "capability denied");
            }
            return root;
        }
        g_macro_self_evo_allowed_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #2101: process-wide runtime pass cap first (capability only tightens).
        {
            const int rt_pass = runtime_hygiene_pass_cap();
            if (rt_pass > 0 && max_passes > rt_pass) {
                max_passes = rt_pass;
                g_macro_self_evo_pass_clamp_total.fetch_add(1, std::memory_order_relaxed);
                g_capability_effect_metrics().macro_self_evo_pass_clamp_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        // Clamp passes further when MacroSelfEvo policy sets a tighter bound.
        if (chk.effective.max_expansion_passes > 0 &&
            max_passes > static_cast<int>(chk.effective.max_expansion_passes)) {
            max_passes = static_cast<int>(chk.effective.max_expansion_passes);
            g_macro_self_evo_pass_clamp_total.fetch_add(1, std::memory_order_relaxed);
            g_capability_effect_metrics().macro_self_evo_pass_clamp_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        // RAII-ish restore of TLS policy for nested clone_macro_body.
        struct DepthPolicyGuard {
            int prev_depth;
            bool prev_rest;
            DepthPolicyGuard(int d, bool rest) noexcept
                : prev_depth(s_effective_max_depth)
                , prev_rest(s_allow_rest_hygiene) {
                s_effective_max_depth = d;
                s_allow_rest_hygiene = rest;
            }
            ~DepthPolicyGuard() noexcept {
                s_effective_max_depth = prev_depth;
                s_allow_rest_hygiene = prev_rest;
            }
            DepthPolicyGuard(const DepthPolicyGuard&) = delete;
            DepthPolicyGuard& operator=(const DepthPolicyGuard&) = delete;
        };
        // Issue #2101: min(hard, runtime, capability) — capability cannot loosen.
        int cap_d = 0;
        if (chk.effective.max_depth > 0)
            cap_d = static_cast<int>(chk.effective.max_depth);
        const int eff_depth = combine_depth_limit(cap_d);
        if (eff_depth < MAX_HYGIENE_DEPTH) {
            g_macro_self_evo_depth_clamp_total.fetch_add(1, std::memory_order_relaxed);
            g_capability_effect_metrics().macro_self_evo_depth_clamp_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        // Concurrent-fiber policy: deny when other top-level clones are live
        // and policy forbids concurrent expand.
        if (!chk.effective.allow_concurrent_fiber &&
            g_macro_clone_in_flight.load(std::memory_order_relaxed) > 0) {
            g_macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
            g_capability_effect_metrics().macro_self_evo_denied_total.fetch_add(
                1, std::memory_order_relaxed);
            if (detail::macro_self_evo_verbose()) {
                std::fprintf(stderr, "[#2023 MacroSelfEvo] macro_expand_all denied: "
                                     "concurrent fiber expand not allowed by policy\n");
            }
            return root;
        }
        DepthPolicyGuard depth_guard(eff_depth, chk.effective.allow_rest_hygiene);
        // Fall through into expand body with max_passes possibly clamped
        // and TLS depth set. Guard must outlive the loop — so we restructure:
        // the rest of the function body continues below with guard in scope.
        return macro_expand_all_body(flat, pool, root, max_passes);
    }
}

// Issue #2023: body of macro_expand_all after capability gate (DepthPolicyGuard
// is held by the caller via TLS already set).
static aura::ast::NodeId macro_expand_all_body(aura::ast::FlatAST& flat,
                                               aura::ast::StringPool& pool, aura::ast::NodeId root,
                                               int max_passes) {
    using namespace aura::ast;
    // Issue #2019: track whether any pass expanded so we restamp
    // MacroIntroduced gens once before return (FlatAST consistency).
    bool any_expand = false;
    for (int pass = 0; pass < max_passes; ++pass) {
        // Phase 1: collect macro definitions
        struct MD {
            aura::ast::FlatAST* src_flat;
            aura::ast::StringPool* src_pool;
            std::vector<std::string> params;
            NodeId body_id;
            bool dotted;
            bool hygienic;  // Issue #120
            bool preserved; // Issue #230 #2
        };
        std::unordered_map<std::string, MD, aura::core::TransparentStringHash, std::equal_to<>>
            local_macros;
        bool has_macro_def = false;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::MacroDef) {
                has_macro_def = true;
                // Macro name is in sym_id; params follow
                auto macro_name = std::string(pool.resolve(v.sym_id));
                std::vector<std::string> params;
                for (auto pid : v.params)
                    params.push_back(std::string(pool.resolve(pid)));
                auto body_id = v.children.empty() ? NULL_NODE : v.child(0);
                // Issue #120: dotted is bit 0, hygienic is bit 1
                bool is_dotted = (v.int_value & 1) != 0;
                bool is_hygienic = (v.int_value & 2) != 0;
                bool is_preserved = (v.int_value & 4) != 0;
                local_macros[macro_name] = MD{&flat,     &pool,       std::move(params), body_id,
                                              is_dotted, is_hygienic, is_preserved};
            }
        }

        if (!has_macro_def) {
            // No more macro defs — final restamp if we expanded earlier.
            if (any_expand)
                restamp_after_expand(flat);
            return root;
        }

        // Phase 2: find and expand macro calls
        bool expanded_any = false;
        NodeId new_root = root;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                std::vector<aura::ast::NodeId> call_args(v.children.begin(), v.children.end());
                auto callee_v = flat.get(call_args[0]);
                if (callee_v.tag == NodeTag::Variable) {
                    auto cname = std::string(pool.resolve(callee_v.sym_id));
                    auto it = local_macros.find(cname);
                    if (it != local_macros.end()) {
                        // Build substitution: macro param → arg expression.
                        // Issue #146 follow-up: route through the pure
                        // helper. Rest-param handling stays here because
                        // it requires FlatAST mutation (allocating a
                        // pair-list) — that's stateful, not pure.
                        auto& md = it->second;
                        auto subst = aura::compiler::pure::compute_macro_subst_pure(
                            md.params, call_args, md.dotted);
                        // Issue #2018: rest param → (list remaining...) in subst
                        // so clone_macro_body substitutes free rest uses.
                        if (md.dotted && !md.params.empty()) {
                            const std::size_t regular_count = md.params.size() - 1;
                            std::vector<aura::ast::NodeId> remaining;
                            for (std::size_t ai = regular_count + 1; ai < call_args.size(); ++ai)
                                remaining.push_back(call_args[ai]);
                            auto list_var = flat.add_variable(pool.intern("list"));
                            auto list_call = flat.add_call(list_var, remaining);
                            subst[md.params.back()] = list_call;
                            // Issue #2239: focused rest-list hygiene
                            // stamp — apply kMacroExpansion +
                            // provenance + schema_cache copy from the
                            // source rest binding (macro body in the
                            // source flat) to every node in the freshly
                            // allocated `(list remaining...)` spine.
                            // Without this, multi-pass macro_expand_all
                            // can re-introduce free uses of rest names
                            // without restamping the new list_call.
                            stamp_rest_param_hygiene(flat, *md.src_flat, md.body_id, list_call);
                        }
                        // Clone macro body with substitution
                        std::unordered_map<std::string, std::string,
                                           aura::core::TransparentStringHash, std::equal_to<>>
                            rename_map;
                        auto expanded = clone_macro_body(
                            flat, pool, *md.src_flat, *md.src_pool, md.body_id, &subst, &rename_map,
                            /*cloned_marker=*/aura::ast::SyntaxMarker::MacroIntroduced);
                        if (expanded != NULL_NODE) {
                            if (id == root)
                                new_root = expanded;
                            expanded_any = true;
                        }
                    }
                }
            }
        }

        if (!expanded_any) {
            if (any_expand)
                restamp_after_expand(flat);
            return root;
        }
        any_expand = true;
        root = new_root;
    }
    // Issue #121: hit the pass limit with macros still in the
    // tree. Emit a warning so the user knows the result is
    // partial. This is the user-facing equivalent of the
    // solver TIMEOUT pattern from Issue #118.
    if (root != NULL_NODE) {
        std::println(std::cerr,
                     "warning: macro_expand_all hit pass limit ({}); "
                     "the result may have unexpanded macro calls",
                     max_passes);
    }
    // Issue #2019: restamp after multi-pass expand (pass-limit exit).
    if (any_expand)
        restamp_after_expand(flat);
    return root;
}
} // namespace aura::compiler::macro_exp
