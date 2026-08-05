// evaluator_primitives_messaging.cpp — P0 step 16: messaging / fiber / channel primitives
// aura.compiler.evaluator module partition; registered via evaluator_ctor.cpp.

module;

#include "runtime_shared.h"
#include "messaging_bridge.h"
#include "security_capabilities.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include "observability_metrics.h"
#include "hash_meta.h"

module aura.compiler.evaluator;

import std;
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

namespace {

    struct FiberResult {
        std::shared_ptr<std::optional<EvalValue>> value;
        bool ready = false;
    };
    std::unordered_map<int64_t, FiberResult> s_fiber_results;
    std::mutex s_fiber_results_mtx;
    std::condition_variable s_fiber_results_cv;

} // namespace

void register_messaging_primitives(PrimRegistrar add, Evaluator& ev) {

    // ═══════════════════════════════════════════════════════════════

    // P14: Inter-Agent Messaging (P0)

    // ═══════════════════════════════════════════════════════════════

    // ── Messaging primitives ───────────────────────────────────

    //

    // send: global bridge (pushing to a target is session-independent)

    // recv: uses compiler_service_ + g_mailbox_read (per-service mailbox access)

    // my-id: uses compiler_service_ + g_session_id (per-service identity)

    // (send target-id message) → #t on success
    add("send", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]))
            return make_bool(false);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send)
            return make_bool(false);
        auto target = ev.string_heap_[as_string_idx(a[0])];
        // If message is already a string, send directly (backward compat, efficient).
        // Otherwise, JSON-encode it.
        std::string msg;
        if (is_string(a[1])) {
            msg = ev.string_heap_[as_string_idx(a[1])];
        } else {
            // Use json-encode primitive to serialize
            auto json_fn = ev.primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[1]});
                if (is_string(result))
                    msg = ev.string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty())
            return make_bool(false);
        return make_bool(bridge.send(target, msg));
    });

    // (broadcast message) — Send a message to ALL registered sessions (P2)
    // Uses g_session_list to enumerate sessions and bridge.send for each.
    // Returns number of messages sent (0 if no sessions or no service).
    add("broadcast", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_int(0);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send)
            return make_int(0);
        std::string msg;
        if (is_string(a[0])) {
            msg = ev.string_heap_[as_string_idx(a[0])];
        } else {
            auto json_fn = ev.primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[0]});
                if (is_string(result))
                    msg = ev.string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty())
            return make_int(0);

        // Enumerate sessions and send to each
        int sent = 0;
        if (aura::messaging::g_session_list && *aura::messaging::g_session_list) {
            auto names = (*aura::messaging::g_session_list)();
            for (auto& name : names) {
                if (bridge.send(name, msg))
                    ++sent;
            }
        }
        return make_int(sent);
    });

    // (recv [timeout-ms]) → message value
    //   Returns: message (string or parsed JSON value) on success,
    //            #<void> on timeout (distinguishable from #f = no service),
    //            #f if no messaging service available.
    //   Uses evaluator's stored ev.compiler_service_ (not global) for safety.
    //   Global g_current_compiler_service may dangle after service destruction.
    add("recv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto svc = ev.compiler_service_;
        if (!svc || !aura::messaging::g_mailbox_read)
            return make_bool(false);
        int timeout_ms = -1;
        if (a.size() >= 1 && is_int(a[0]))
            timeout_ms = static_cast<int>(as_int(a[0]));
        auto result = aura::messaging::g_mailbox_read(svc, timeout_ms);
        if (!result) {
            return make_bool(false);
        }
        auto& raw = *result;
        if (!raw.empty() && (raw[0] == '{' || raw[0] == '[' || raw[0] == '"')) {
            auto parse_fn = ev.primitives_.lookup("json-parse");
            if (parse_fn) {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(raw);
                auto parsed = (*parse_fn)({make_string(sid)});
                if (!is_void(parsed))
                    return parsed;
            }
        }
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(*result));
        return make_string(idx);
    });

    // (my-id) → current session ID string
    add("my-id", [&ev](const auto&) -> EvalValue {
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_session_id)
            return make_string(0);
        auto id = aura::messaging::g_session_id(svc);
        if (id.empty())
            id = "(unknown)";
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(id);
        return make_string(idx);
    });

    // (reply msg) → #t on success (sends to last message's sender)
    // Supports non-string values via JSON encoding.
    add("reply", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_bool(false);
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_mailbox_last_sender)
            return make_bool(false);
        auto target = aura::messaging::g_mailbox_last_sender(svc);
        if (target.empty())
            return make_bool(false);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send)
            return make_bool(false);
        std::string msg;
        if (is_string(a[0])) {
            msg = ev.string_heap_[as_string_idx(a[0])];
        } else {
            auto json_fn = ev.primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[0]});
                if (is_string(result))
                    msg = ev.string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty())
            return make_bool(false);
        return make_bool(bridge.send(target, msg));
    });

    // (session-active? id) → #t if session exists
    add("session-active?", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]) || !aura::messaging::g_session_exists)
            return make_bool(false);
        auto id = ev.string_heap_[as_string_idx(a[0])];
        return make_bool(aura::messaging::g_session_exists(id));
    });

    // (mailbox-count) → number of pending messages in our mailbox
    ObservabilityPrims::register_stats_impl("mailbox-count", [&ev](const auto&) -> EvalValue {
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_mailbox_count)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(aura::messaging::g_mailbox_count(svc)));
    });

    // Issue #1585: process-global MultiFiberMailbox stats surface.
    // Full attach/broadcast/recv API is C++ (multi_fiber_mailbox.h);
    // agent frameworks bind via this dashboard + serve hooks.
    ObservabilityPrims::register_stats_impl(
        "query:mf-mailbox-stats", [&ev](const auto&) -> EvalValue {
            using namespace aura::serve::mf_mailbox;
            // Issue #1881: full health fields (priority / waits / linear).
            // Issue #2010: fanout-backpressure-rejects.
            // Issue #2188: recv-rejected-in-mutation-boundary.
            std::uint64_t pushes = 0, pops = 0, broadcasts = 0, bp = 0, attaches = 0, ph = 0,
                          waits = 0, tmo = 0, lchk = 0, lviol = 0, fbp = 0, rej_bound = 0,
                          def_mh = 0;
            MultiFiberMailbox::snapshot_global_full(pushes, pops, broadcasts, bp, attaches, ph,
                                                    waits, tmo, lchk, lviol, &fbp, &rej_bound,
                                                    &def_mh);
            // Capacity 64→128: #2188/#2312/#2347/#2378 drain-SLA keys.
            auto* ht = FlatHashTable::create(128);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("pushes", static_cast<std::int64_t>(pushes));
            insert_kv("pops", static_cast<std::int64_t>(pops));
            insert_kv("broadcasts", static_cast<std::int64_t>(broadcasts));
            insert_kv("backpressure-rejects", static_cast<std::int64_t>(bp));
            insert_kv("fanout-backpressure-rejects", static_cast<std::int64_t>(fbp));
            insert_kv("attaches", static_cast<std::int64_t>(attaches));
            insert_kv("priority-high", static_cast<std::int64_t>(ph));
            insert_kv("recv-waits", static_cast<std::int64_t>(waits));
            insert_kv("recv-timeouts", static_cast<std::int64_t>(tmo));
            insert_kv("linear-checks", static_cast<std::int64_t>(lchk));
            insert_kv("linear-violations", static_cast<std::int64_t>(lviol));
            insert_kv("phase", static_cast<std::int64_t>(kMultiFiberMailboxPhase));
            insert_kv("schema", 1585);
            insert_kv("schema-1881", 1881);
            insert_kv("schema-2010", 2010);
            // Issue #2188: forbid blocking recv under live MutationBoundary.
            insert_kv("recv-rejected-in-mutation-boundary", static_cast<std::int64_t>(rej_bound));
            insert_kv("recv-rejected-in-mutation-boundary-total",
                      static_cast<std::int64_t>(rej_bound));
            insert_kv("schema-2188", 2188);
            insert_kv("issue-2188", 2188);
            insert_kv("recv-boundary-gate-wired", 1);
            insert_kv("health-wired", 1);
            // Issue #2312: push-side delivery gate — push / broadcast_fanout
            // defer (BP) when target fiber(s) hold MutationBoundary. Distinct
            // from recv-side recv-rejected-in-mutation-boundary (#2188); this
            // is the producer-side gate that prevents lock-order inversion vs
            // workspace_mtx_ + GcDeferReason::MutationHold under multi-agent
            // fanout. Reuses #2184 MutationSafetySnapshot + is_at_mutation_
            // boundary_safe truth table.
            insert_kv("mailbox-deferred-mutation-hold-total", static_cast<std::int64_t>(def_mh));
            insert_kv("mailbox_deferred_mutation_hold_total", static_cast<std::int64_t>(def_mh));
            insert_kv("mailbox-mutation-hold-gate-wired", 1);
            insert_kv("schema-2312", 2312);
            insert_kv("issue-2312", 2312);
            // Issue #2378: defer drain SLA — depth / HWM / flush latency /
            // starvation (last-call + cumulative). Happy path depth=0 free.
            insert_kv("mailbox-deferred-depth",
                      static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_depth.load(
                          std::memory_order_relaxed)));
            insert_kv("mailbox_deferred_depth",
                      static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_depth.load(
                          std::memory_order_relaxed)));
            insert_kv(
                "mailbox-deferred-depth-high-water",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_depth_high_water.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "mailbox_deferred_depth_high_water",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_depth_high_water.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-deferred-flush-latency-us-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_deferred_flush_latency_us_total.load(
                              std::memory_order_relaxed)));
            insert_kv(
                "mailbox-deferred-flush-samples",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_flush_samples.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-deferred-flush-latency-us-max",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_deferred_flush_latency_us_max.load(
                              std::memory_order_relaxed)));
            insert_kv(
                "mailbox-defer-starvation-total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "mailbox_defer_starvation_total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-deferred-drain-opportunity-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_deferred_drain_opportunity_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-defer-drain-sla-wired", 1);
            insert_kv("schema-2378", 2378);
            insert_kv("issue-2378", 2378);
            // Issue #2511: outermost Guard exit forced deferred drain under budget.
            insert_kv(
                "mailbox-hold-exit-drain-total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_hold_exit_drain_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "mailbox-hold-exit-drain-us-total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_hold_exit_drain_us_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "mailbox-hold-exit-drain-us-max",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_hold_exit_drain_us_max.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-hold-exit-starvation-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-hold-exit-force-resolved-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_hold_exit_force_resolved_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-hold-exit-drain-budget-us",
                      static_cast<std::int64_t>(mailbox_hold_drain_budget_us()));
            insert_kv("mailbox-hold-exit-drain-wired", 1);
            insert_kv("schema-2511", 2511);
            insert_kv("issue-2511", 2511);
            // Issue #2551: production hard starvation + Agent throttle flag.
            insert_kv("mailbox-hold-starvation-hard-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(
                              std::memory_order_relaxed)));
            insert_kv("agent-throttle-for-mailbox-starvation",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-hold-starvation-hard-wired", 1);
            insert_kv("schema-2551", 2551);
            insert_kv("issue-2551", 2551);
            // Issue #2587: mutate admission gate counter (hard reject
            // vs metric-only soft path; AC1 / AC2). Zero cost when
            // agent-throttle flag == 0 — single relaxed load at every
            // gate site (try_acquire + try_acquire_for_region +
            // public mutate prims + TransactionGuard host callback
            // transitively).
            insert_kv("mutate-rejected-mailbox-starvation-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mutate_rejected_mailbox_starvation_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mutate-rejected-mailbox-starvation-wired", 1);
            insert_kv("schema-2587", 2587);
            insert_kv("issue-2587", 2587);
            // Issue #2347: Strict hard audit + optional Guard-window force-rollback.
            // Soft path leaves hard-total / force-rollback-total at 0; Policy A
            // soft reject remains on recv-rejected-in-mutation-boundary.
            // Capacity 48 holds pre-#2347 keys + these additive rows without drop.
            const auto hard_tot =
                g_mf_mailbox_stats.recv_rejected_in_mutation_boundary_hard_total.load(
                    std::memory_order_relaxed);
            const auto force_rb = g_mf_mailbox_stats.recv_boundary_force_rollback_total.load(
                std::memory_order_relaxed);
            insert_kv("recv-rejected-in-mutation-boundary-hard-total",
                      static_cast<std::int64_t>(hard_tot));
            insert_kv("recv_rejected_in_mutation_boundary_hard_total",
                      static_cast<std::int64_t>(hard_tot));
            insert_kv("recv-boundary-force-rollback-total", static_cast<std::int64_t>(force_rb));
            insert_kv("recv_boundary_force_rollback_total", static_cast<std::int64_t>(force_rb));
            insert_kv("mutate-mailbox-strict-wired", 1);
            insert_kv("recv-boundary-hard-wired", 1);
            insert_kv("schema-2347", 2347);
            insert_kv("issue-2347", 2347);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1586 / #1881: process-global parallel_orch / parallel_intend stats.
    // Full spawn/join/aggregate API is C++ (parallel_orch.h); agent EDSL
    // surface for (parallel-intend ...) is #1587.
    ObservabilityPrims::register_stats_impl(
        "query:parallel-orch-stats", [&ev](const auto&) -> EvalValue {
            using namespace aura::serve::parallel_orch;
            std::uint64_t batches = 0, spawned = 0, joined = 0, ok = 0, err = 0, ff = 0, to = 0,
                          mb = 0, qr = 0, inv = 0, bok = 0, bpart = 0, jwait = 0, elapsed = 0,
                          retries = 0, circuit = 0;
            snapshot_global_ext(batches, spawned, joined, ok, err, ff, to, mb, qr, inv, bok, bpart,
                                jwait, elapsed, &retries, &circuit);
            // Capacity 48: pre-#2153 keys + join-drain residual/us/schema (#2153).
            auto* ht = FlatHashTable::create(48);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("batches", static_cast<std::int64_t>(batches));
            insert_kv("spawned", static_cast<std::int64_t>(spawned));
            insert_kv("joined", static_cast<std::int64_t>(joined));
            insert_kv("ok", static_cast<std::int64_t>(ok));
            insert_kv("err", static_cast<std::int64_t>(err));
            insert_kv("fail-fast-aborts", static_cast<std::int64_t>(ff));
            insert_kv("timeouts", static_cast<std::int64_t>(to));
            insert_kv("mailbox-posts", static_cast<std::int64_t>(mb));
            // Issue #1881 health fields
            insert_kv("quota-rejects", static_cast<std::int64_t>(qr));
            insert_kv("invalid-batches", static_cast<std::int64_t>(inv));
            insert_kv("batch-ok", static_cast<std::int64_t>(bok));
            insert_kv("batch-partial", static_cast<std::int64_t>(bpart));
            insert_kv("join-wait-us-total", static_cast<std::int64_t>(jwait));
            insert_kv("avg-join-us", batches > 0 ? static_cast<std::int64_t>(jwait / batches) : 0);
            insert_kv("parallel-elapsed-us", static_cast<std::int64_t>(elapsed));
            // Issue #2007 FailurePolicy counters
            insert_kv("retries-performed", static_cast<std::int64_t>(retries));
            insert_kv("circuit-opened-total", static_cast<std::int64_t>(circuit));
            insert_kv("phase", static_cast<std::int64_t>(kParallelOrchPhase));
            insert_kv("schema", 1586);
            insert_kv("schema-1881", 1881);
            insert_kv("schema-2007", 2007);
            insert_kv("health-wired", 1);
            // Issue #2153: Timeout path secondary drain residual / wait.
            insert_kv(
                "join-drain-residual-total",
                static_cast<std::int64_t>(g_parallel_orch_stats.join_drain_residual_total.load(
                    std::memory_order_relaxed)));
            insert_kv("join-drain-us-total",
                      static_cast<std::int64_t>(g_parallel_orch_stats.join_drain_us_total.load(
                          std::memory_order_relaxed)));
            insert_kv("schema-2153", 2153);
            insert_kv("issue-2153", 2153);
            insert_kv("join-drain-wired", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // (fiber:spawn fn) — Spawn a fiber (async).
    // fn is a 0-arg closure. Result retrievable via (fiber:join fid).
    //
    // Issue #2656 contract (CLI denseness / Hephaestus H9):
    //   Success → positive integer fiber id
    //   Failure → #f (or capability error object under sandbox)
    //   Never returns 0 or -1 on success.
    //
    // Backends:
    //   1. Scheduler — when g_fiber_spawn is set (serve-async /
    //      --serve-async-bench). Positive scheduler Fiber::id().
    //   2. Thread fallback — CLI stdin / denseness without serve-async.
    //      Detached std::thread + positive high-bit ids (0x4000_0000 |
    //      seq). Pre-#2656 used negative ids starting at -1; denseness
    //      probes treated -1 as failure even though join worked.
    //
    // Concurrent denseness: thread fallback is a valid multi-worker
    // backend for axis-C rebind under load. Real stackful fibers need
    // serve-async / epoll. Sequential-yield fanout remains a supported
    // denseness surrogate when spawn is denied by capability/sandbox.
    add("fiber:spawn", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_closure(a[0]))
            return make_bool(false);
        // Issue #1294 Phase 1: capability gate for fiber primitives.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapFiber) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: fiber required",
                                        ev.primitive_error_counter_ptr());
        }
        auto cid = as_closure_id(a[0]);
        auto result_ptr = std::make_shared<std::optional<EvalValue>>();
        // Issue #1291 (P0): shared atomic holds the real fiber id.
        // Pre-#1291 captured `fid` by value at lambda creation (fid==0),
        // so workers wrote s_fiber_results[0] while joiners waited on
        // the real fid — hang forever. Shared state is published before
        // the worker runs complete_fiber.
        auto fid_holder = std::make_shared<std::atomic<int64_t>>(0);
        auto complete_fiber = [&ev, cid, result_ptr, fid_holder]() {
            *result_ptr = ev.apply_closure(cid, {});
            // Issue #1291: wait until spawn path publishes the real fid
            // AND registers s_fiber_results[fid] (g_fiber_spawn may run
            // the body before returning / before register).
            for (int spin = 0; spin < 200000; ++spin) {
                const int64_t fid = fid_holder->load(std::memory_order_acquire);
                if (fid != 0) {
                    std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
                    auto it = s_fiber_results.find(fid);
                    if (it != s_fiber_results.end()) {
                        it->second.ready = true;
                        s_fiber_results_cv.notify_all();
                        return;
                    }
                }
                std::this_thread::yield();
            }
            // Best-effort notify even if registration raced past timeout.
            s_fiber_results_cv.notify_all();
        };
        int64_t fid = 0;
        // In serve-async mode: use g_fiber_spawn to create a real fiber
        if (aura::messaging::g_fiber_spawn) {
            fid = aura::messaging::g_fiber_spawn(complete_fiber);
            fid_holder->store(fid, std::memory_order_release);
        }
        // Fallback (CLI denseness / stdin): std::thread with positive ids.
        if (fid <= 0) {
            // Issue #2656: positive thread-fallback ids. High bit set so
            // they never collide with low scheduler Fiber::id() values
            // and never equal -1 (denseness failure sentinel).
            static std::atomic<std::int64_t> thread_fiber_seq{0};
            const auto seq = thread_fiber_seq.fetch_add(1, std::memory_order_relaxed) + 1;
            fid = static_cast<std::int64_t>(0x4000'0000LL) | seq;
            fid_holder->store(fid, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
                s_fiber_results[fid] = FiberResult{result_ptr, false};
            }
            std::thread(complete_fiber).detach();
            return make_int(fid);
        }
        if (fid > 0) {
            // Publish fid before registering so a fast worker still
            // sees the real id (complete_fiber loads with acquire).
            fid_holder->store(fid, std::memory_order_release);
            std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
            s_fiber_results[fid] = FiberResult{std::move(result_ptr), false};
            return make_int(fid);
        }
        return make_bool(false);
    });

    // Issue #2656: which backend (fiber:spawn) will use right now.
    // "scheduler" | "thread" — denseness hosts can assert concurrent path.
    add("fiber:spawn-backend", [](const auto&) -> EvalValue {
        // Interned at call time via make_string of heap push is heavier;
        // return a small int enum for probes + keep string via display path
        // optional. Use 1=scheduler, 2=thread (always one of these when
        // capability allows; capability deny is only on spawn).
        if (aura::messaging::g_fiber_spawn)
            return make_int(1); // scheduler
        return make_int(2);     // thread fallback (CLI denseness)
    });

    // (fiber:yield) — Yield current fiber to scheduler (serve mode only)
    // Uses g_fiber_yield callback if available.
    add("fiber:yield", [&ev](const auto&) -> EvalValue {
        if (aura::messaging::g_fiber_yield) {
            aura::messaging::g_fiber_yield();
        }
        return make_void();
    });

    // (session:create name) — Create a new isolated session (serve mode only)
    // Returns #t on success, #f in stdin mode or if name already exists
    add("session:create", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto& name = ev.string_heap_[as_string_idx(a[0])];
        if (!aura::messaging::g_session_create || !(*aura::messaging::g_session_create))
            return make_bool(false);
        return make_bool((*aura::messaging::g_session_create)(name));
    });

    // (_agent:spawn name) — Create a new named agent session (internal primitive)
    // Called by the Aura-level agent:spawn wrapper.
    // In serve mode: creates a full cross-session agent via g_session_create.
    // In stdin mode: creates a lightweight in-process agent with a mailbox.
    // Returns the agent name on success, or error on failure.
    add("_agent:spawn", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // local merr removed; now centralized make_merr (phase complete)
        if (a.empty() || !is_string(a[0]))
            return ev.make_merr("bad-arg", "usage: (agent:spawn name)");
        auto& name = ev.string_heap_[as_string_idx(a[0])];
        if (name.empty())
            return ev.make_merr("bad-arg", "agent name must not be empty");

        // Try serve-mode first (full session isolation)
        if (aura::messaging::g_session_create && *aura::messaging::g_session_create) {
            if ((*aura::messaging::g_session_create)(name)) {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(name);
                return make_string(sidx);
            }
            return ev.make_merr("create-failed",
                                std::string("could not create session \"") + name + "\"");
        }

        // Stdin/pipe mode: lightweight in-process agent via Aura-level *agents* registry
        // The Aura-level agent:spawn wraps this C++ primitive and falls back to
        // the *agents* registry when g_session_create is unavailable.
        return ev.make_merr("no-serve", "agent:spawn requires serve mode or a local handler");
    });

    // (fiber:join fiber-id) — Wait for a fiber to complete and return its result.
    //
    // Issue #119: replaces the previous 200k-iteration spin-wait
    // (unreliable for slow fibers / heavily loaded schedulers)
    // with a proper blocking mechanism. Two paths:
    //
    // 1. **stdin mode** (no fiber scheduler): the OS thread
    //    blocks on s_fiber_results_cv via wait_for (200s ceiling
    //    via wait_for's timeout). Woken by the worker that
    //    completes the target fiber. Zero CPU burn during the
    //    wait. (This path was already in #109 5b; preserved.)
    //
    // 2. **serve-async mode** (fiber scheduler active): the
    //    joiner fiber registers itself on the target's
    //    joiner_map_ via the new g_fiber_lookup +
    //    Scheduler::add_joiner API. When the target completes,
    //    Scheduler::on_fiber_done writes a 1 to the joiner's
    //    eventfd, which is registered with the IO thread's
    //    epoll — the IO thread resumes the joiner via
    //    wait_map_ lookup. The joiner yields (yield reason:
    //    BlockingIO — unstealable), the IO thread picks up
    //    the eventfd notification, and the joiner is resumed
    //    in the next dispatch cycle. No spin, no busy poll.
    //
    // Both paths share the same s_fiber_results entry / ready
    // flag, so the result-fetch-and-erase is unchanged.
    add("fiber:join", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto fid = static_cast<int64_t>(as_int(a[0]));

        auto is_ready = [fid] {
            auto it = s_fiber_results.find(fid);
            return it != s_fiber_results.end() && it->second.ready;
        };

        // Issue #164 sub-fix #4: capture the workspace's
        // ev.defuse_version_ at the start of the join, so we can
        // re-check it at wakeup to detect mutations that
        // happened DURING the join. Set to 0 at the start means
        // the post-wait check is "first join ever or just reset"
        // — guarded by `!= 0` so we don't false-positive on the
        // initial state.
        ev.defuse_version_at_wait_ = ev.defuse_version_.load(std::memory_order_relaxed);

        if (aura::messaging::g_fiber_yield && aura::messaging::g_fiber_lookup) {
            // Issue #119: serve-async proper-blocking path.
            //
            // 1. Check if the target is already done. If so, skip
            //    the joiner registration and go straight to
            //    result fetch.
            // 2. Register this fiber (the joiner) on the target's
            //    joiner_map via Scheduler::add_joiner.
            // 3. Yield with BlockingIO (unstealable) so the
            //    scheduler parks the joiner. The target's
            //    on_fiber_done wakes the joiner via eventfd;
            //    the IO thread's epoll resumes the joiner.
            // 4. After wakeup, unregister from joiner_map and
            //    fetch the result.
            auto* target = static_cast<aura::serve::Fiber*>(aura::messaging::g_fiber_lookup(fid));
            if (!target) {
                // Target fiber not found. Fall through to the
                // stdin-style result-fetch path: if a result
                // exists, return it; otherwise return void.
                std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
                auto it = s_fiber_results.find(fid);
                if (it == s_fiber_results.end())
                    return make_void();
                if (!it->second.ready)
                    return make_void();
            } else if (!target->is_done()) {
                // Target is alive. Register the joiner and
                // yield. We need to schedule the unregister for
                // after the wakeup — store the target pointer
                // in a thread_local so the post-yield cleanup
                // can find it.
                thread_local std::uint64_t joiner_target_id = 0;
                // (Note: thread_local is a per-fiber/thread trick;
                // a cleaner approach is to pass the target_id as
                // a stack value via the yield + resume protocol,
                // but that's invasive. The thread_local works
                // because the joiner fiber is single-threaded.)
                if (aura::serve::g_scheduler &&
                    aura::serve::g_scheduler->add_joiner(static_cast<std::uint64_t>(fid),
                                                         aura::serve::g_current_fiber)) {
                    joiner_target_id = static_cast<std::uint64_t>(fid);
                    // Yield with BlockingIO so the scheduler
                    // doesn't steal this fiber while we wait.
                    aura::serve::Fiber::yield(aura::serve::YieldReason::BlockingIO);
                    // After wakeup: the target's on_fiber_done
                    // already wrote to our eventfd. The
                    // IO thread's epoll resumed us. The
                    // result is now in s_fiber_results.
                    aura::serve::g_scheduler->remove_joiner(joiner_target_id,
                                                            aura::serve::g_current_fiber);
                }
                // (If add_joiner failed — e.g. target was
                // destroyed between the lookup and the add —
                // we fall through to the result fetch below.
                // The fetch will find no entry and return void,
                // which is the correct error behavior.)
            }
            // else: target is already done. Skip the join — go
            // straight to the result fetch below.
        } else if (aura::messaging::g_fiber_yield) {
            // Issue #164: serve-async but no fiber_lookup. The
            // previous implementation did a 200k-iteration spin
            // with g_fiber_yield between iterations. This is a
            // bug:
            //   - Wastes CPU even with yield (the test reproduces
            //     5-15% CPU burn under 50+ fibers with mutations)
            //   - Creates transient ev.defuse_version_ ↔ workspace
            //     inconsistency: while spinning, mutate:*
            //     primitives may have incremented ev.defuse_version_
            //     (twice — once before and once after the
            //     mutation) and the joiner's view of the workspace
            //     is now stale.
            //   - Degrades to old behavior silently when
            //     g_fiber_lookup is missing (e.g. test contexts
            //     where the fiber scheduler hook isn't installed).
            //
            // The fix: degrade cleanly to the stdin-style
            // blocking wait on s_fiber_results_cv. The cv
            // semantics are already tested (Issue #109 5b) and
            // give us zero-CPU blocking. This is the "clean
            // degradation" option from the issue AC.
            //
            // Tradeoff: slower wakeup than the proper-blocking
            // path (cv needs a notify_all + epoll cycle vs the
            // eventfd + epoll cycle), but ZERO CPU burn during
            // the wait, and no version inconsistency.
            std::println(std::cerr, "[fiber:join] WARN: serve-async mode without g_fiber_lookup; "
                                    "degrading to cv-based blocking. This is a misconfiguration "
                                    "in production; check the fiber scheduler hook installation.");
            std::unique_lock<std::mutex> lock(s_fiber_results_mtx);
            s_fiber_results_cv.wait_for(lock, std::chrono::seconds(200), is_ready);
        } else {
            // Stdin mode: real blocking wait on the cv. 200s
            // ceiling via wait_for (returns false on timeout,
            // true on notify).
            std::unique_lock<std::mutex> lock(s_fiber_results_mtx);
            s_fiber_results_cv.wait_for(lock, std::chrono::seconds(200), is_ready);
        }

        // Result is ready (or timed out). Fetch and return.
        std::shared_ptr<std::optional<EvalValue>> result_ptr;
        {
            std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
            auto it = s_fiber_results.find(fid);
            if (it == s_fiber_results.end())
                return make_void();
            if (!it->second.ready || !it->second.value || !it->second.value->has_value()) {
                s_fiber_results.erase(it);
                return make_void();
            }
            result_ptr = std::move(it->second.value);
            s_fiber_results.erase(it);
        }

        // Issue #164 sub-fix #4: re-validate ev.defuse_version_ after
        // wakeup. While the joiner was blocked (via the proper-
        // blocking path OR the cv-degradation path), mutate:*
        // primitives in OTHER fibers may have incremented
        // ev.defuse_version_ (twice per mutation: once before and
        // once after, with a yield_mutation_boundary between).
        //
        // A mismatch between the version we observed at join start
        // and the version we observe at join wakeup means the
        // workspace mutated during the join — which is the
        // transient inconsistency the issue calls out.
        //
        // The fix: capture the version at the start of the wait,
        // re-check at the end. On mismatch, emit a loud warning
        // (stderr) so the operator sees the inconsistency. We
        // do NOT abort — the result is still valid; the warning
        // is for observability. Future work could surface this
        // as a structured diagnostic.
        //
        // The captured version is updated only when the wait
        // actually happened (i.e., the joiner didn't fast-path
        // the already-done target). For fast-path joins, there's
        // no wait, so no re-validation needed.
        auto dv_now = ev.defuse_version_.load(std::memory_order_acquire);
        if (ev.defuse_version_at_wait_ != dv_now && ev.defuse_version_at_wait_ != 0) {
            std::println(std::cerr,
                         "[fiber:join] WARN: workspace mutated during join "
                         "(defuse_version {} -> {}). The joiner resumed after "
                         "concurrent mutate:* primitives. Result is still valid; "
                         "re-validate callers if they depend on workspace state.",
                         ev.defuse_version_at_wait_, dv_now);
        }
        ev.defuse_version_at_wait_ = 0; // reset for next join

        return std::move(**result_ptr);
    });

    // ── orch:metrics — scheduler metrics (Issue #32) ─────────────
    // (orch:metrics) → Returns a JSON string with scheduler counters.
    // Fields: fibers_spawned, fibers_completed, io_events,
    // steal_attempts, steal_successes, per-worker breakdown.
    // Returns empty string when not in serve-async mode.
    ObservabilityPrims::register_stats_impl("orch:metrics", [&ev](const auto&) -> EvalValue {
        // Issue #1147: wire orch_telemetry counters on the real orch path.
        ev.bump_orch_telemetry();
        if (!aura::messaging::g_get_scheduler_metrics) {
            // Not in serve-async mode — return empty list
            return types::make_void();
        }
        auto json = aura::messaging::g_get_scheduler_metrics();
        ev.bump_orch_telemetry_hit();
        if (!json.empty())
            ev.bump_orch_telemetry_savings();
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(json);
        return types::make_string(idx);
    });

    // ── scheduler:pin — pin current fiber to a specific worker (P2) ─
    // (scheduler:pin worker-id) → Pins the current fiber to the given
    // worker thread for cache-aware scheduling. The fiber will always
    // run on that worker (won't be stolen). Returns #t on success, #f
    // when not in serve-async mode or invalid worker ID.
    // worker-id: 0..N-1 where N = number of workers.
    add("scheduler:pin", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto wid = static_cast<int>(as_int(a[0]));
        if (wid < 0)
            return make_bool(false);
        if (aura::messaging::g_fiber_set_affinity) {
            aura::messaging::g_fiber_set_affinity(wid);
            return make_bool(true);
        }
        return make_bool(false);
    });

    // ── orch:reset-metrics — reset scheduler counters (Issue #32) ─
    // (orch:reset-metrics) → Resets all metrics to zero.
    // Returns #t when done, #f when not in serve-async mode.
    add("orch:reset-metrics", [&ev](const auto&) -> EvalValue {
        if (aura::messaging::g_reset_scheduler_metrics) {
            aura::messaging::g_reset_scheduler_metrics();
            return types::make_bool(true);
        }
        return types::make_bool(false);
    });

    // (thread_pool:enqueue fn) — Offload a blocking function to the thread pool.
    // fn is a closure taking no arguments.
    // Returns the result on success (may block caller until done).
    // In serve/fiber mode: yields fiber, pool thread wakes it.
    // In stdin mode: uses std::async, blocks synchronously.
    add("thread_pool:enqueue", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_closure(a[0]))
            return make_bool(false);
        auto cid = as_closure_id(a[0]);
        // Serve/fiber mode: use g_thread_pool_enqueue callback
        if (aura::messaging::g_thread_pool_enqueue) {
            auto result_ptr = std::make_shared<std::optional<EvalValue>>();
            aura::messaging::g_thread_pool_enqueue(
                [&ev, cid, result_ptr]() { *result_ptr = ev.apply_closure(cid, {}); }, -1);
            if (aura::messaging::g_fiber_block) {
                aura::messaging::g_fiber_block();
            }
            if (result_ptr && result_ptr->has_value())
                return std::move(**result_ptr);
            return make_void();
        }
        // Stdin mode: use std::async
        auto future =
            std::async(std::launch::async, [&ev, cid]() { return ev.apply_closure(cid, {}); });
        auto result = future.get();
        if (result)
            return *result;
        return make_void();
    });

    // (channel:create [buffer-size]) — Create a new channel
    // Returns channel-id (fixnum) or error.
    // buffer-size defaults to 0 (rendezvous/synchronous).
    add("channel:create", [&ev](std::span<const EvalValue> a) -> EvalValue {
        std::size_t buf = 0;
        if (!a.empty() && is_int(a[0])) {
            auto v = as_int(a[0]);
            if (v < 0)
                return make_bool(false);
            buf = static_cast<std::size_t>(v);
        }
        auto ch = std::make_shared<Evaluator::Channel>();
        ch->buffer_size = buf;
        std::lock_guard lk(ev.channels_mtx_);
        auto id = ev.channels_.size();
        ev.channels_.push_back(ch);
        return make_int(static_cast<std::int64_t>(id));
    });

    // (channel:send channel-id msg) — Send a message to a channel
    // Returns #t on success, #f if channel does not exist / closed.
    // Blocks if buffer full (buffered) or until a recv is posted (rendezvous).
    // Issue #2483: buffer_size==0 must NOT short-circuit the wait predicate
    // (that made send non-blocking and let the queue grow unboundedly).
    add("channel:send", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        auto msg = ev.string_heap_[as_string_idx(a[1])]; // copy before wait
        // Issue #2483: release channels_mtx_ before blocking — holding it
        // across cv.wait deadlocks concurrent channel:recv on the same
        // Evaluator (recv needs the global map lock to look up the channel).
        std::shared_ptr<Evaluator::Channel> ch_ptr;
        {
            std::lock_guard lk(ev.channels_mtx_);
            if (ch_id >= ev.channels_.size() || !ev.channels_[ch_id])
                return make_bool(false);
            ch_ptr = ev.channels_[ch_id];
        }
        auto& ch = *ch_ptr;
        std::unique_lock ul(ch.mtx);
        ch.cv.wait(ul, [&]() {
            if (ch.closed)
                return true;
            // Buffered: wait for free slot (size never exceeds buffer_size).
            if (ch.buffer_size > 0)
                return ch.queue.size() < ch.buffer_size;
            // Rendezvous (buffer_size==0): wait until a recv is posted and
            // the handoff slot is free (at most one in-flight message).
            return ch.waiting_receivers > 0 && ch.queue.empty();
        });
        if (ch.closed)
            return make_bool(false);
        ch.queue.push_back(std::move(msg));
        ul.unlock();
        ch.cv.notify_all();
        return make_bool(true);
    });

    // (channel:recv channel-id) — Receive a message from a channel
    // Returns the message string, or empty string if channel closed.
    // Blocks until a message is available.
    add("channel:recv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_string(0);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::shared_ptr<Evaluator::Channel> ch_ptr;
        {
            std::lock_guard lk(ev.channels_mtx_);
            if (ch_id >= ev.channels_.size() || !ev.channels_[ch_id])
                return make_string(0);
            ch_ptr = ev.channels_[ch_id];
        }
        auto& ch = *ch_ptr;
        std::unique_lock ul(ch.mtx);
        // Issue #2483: announce waiting receiver so rendezvous send can proceed.
        ++ch.waiting_receivers;
        ch.cv.notify_all();
        ch.cv.wait(ul, [&]() { return ch.closed || !ch.queue.empty(); });
        --ch.waiting_receivers;
        if (ch.queue.empty()) {
            ul.unlock();
            ch.cv.notify_all();
            return make_string(0);
        }
        auto msg = std::move(ch.queue.front());
        ch.queue.pop_front();
        ul.unlock();
        ch.cv.notify_all();
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(msg));
        return make_string(static_cast<std::uint64_t>(idx));
    });

    // (channel:try-recv channel-id) — Non-blocking receive
    // Returns message string if available, or empty string if no message.
    add("channel:try-recv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_string(0);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::shared_ptr<Evaluator::Channel> ch_ptr;
        {
            std::lock_guard lk(ev.channels_mtx_);
            if (ch_id >= ev.channels_.size() || !ev.channels_[ch_id])
                return make_string(0);
            ch_ptr = ev.channels_[ch_id];
        }
        auto& ch = *ch_ptr;
        std::unique_lock ul(ch.mtx);
        if (ch.queue.empty())
            return make_string(0);
        auto msg = std::move(ch.queue.front());
        ch.queue.pop_front();
        ul.unlock();
        ch.cv.notify_all(); // wake blocked senders (full buffer / next rendezvous)
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(msg));
        return make_string(static_cast<std::uint64_t>(idx));
    });

    // (channel:close channel-id) — Close a channel
    // Wakes all waiters; subsequent recv returns empty string.
    add("channel:close", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::shared_ptr<Evaluator::Channel> ch_ptr;
        {
            std::lock_guard lk(ev.channels_mtx_);
            if (ch_id >= ev.channels_.size() || !ev.channels_[ch_id])
                return make_bool(false);
            ch_ptr = ev.channels_[ch_id];
        }
        auto& ch = *ch_ptr;
        {
            std::lock_guard ul(ch.mtx);
            ch.closed = true;
        }
        ch.cv.notify_all();
        return make_bool(true);
    });

    // (_agent:list) — List all active agent sessions (internal primitive)
    // Called by the Aura-level agent:list wrapper.
    add("_agent:list", [&ev](const auto&) -> EvalValue {
        EvalValue result = make_void();
        if (!aura::messaging::g_session_list || !(*aura::messaging::g_session_list))
            return result;
        auto names = (*aura::messaging::g_session_list)();
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });
}

} // namespace aura::compiler::primitives_detail
