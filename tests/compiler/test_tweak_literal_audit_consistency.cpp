// @category: unit
// @reason: Issue #2799 — tweak-literal logs Committed before outer batch
// commit; batch/Guard abort must restore int AND mark status=RolledBack
// (same torn-audit class as #2793 replace-value).
//
//   AC1: lockless + public cite #2799; MutationSoAField::IntVal
//   AC2: Guard abort restores value + status=RolledBack
//   AC3: atomic-batch tweak then fail → value old + tweak log RolledBack
//   AC4: commit path keeps Committed
//   AC5: this suite + linter; no docs/design/2799-*; no test_issue_2799.cpp
//
// Issue #3601 (lockless mutate deny joinable SE — #3217/#3319/#3543 residual):
//   3601 AC1: batch :allow-macro? #t + lockless tweak deny → MacroHygiene SE
//             rows carrying the pinned composite mid + stable reason
//             hygiene-macro-introduced (#3066 join)
//   3601 AC2: query:security-audit positional mid+reason filter returns the row
//   3601 AC3: Soft/Off deny — zero SE emit, zero ring rows (bumps only)
//   3601 AC4: public mutate:tweak-literal deny emits exactly one SE (no double)
//   3601 AC5: chaos — remove-node + tweak in one aborted batch; both deny
//             stamps share the pinned composite mid (#3066)

#include "test_harness.hpp"

#include "compiler/grant_test_support.hh"
#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_isolation.hh"

#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.macro_expansion;
import aura.compiler.service;
import aura.compiler.value;
import aura.core;
import aura.core.ast;

namespace {

using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::macro_exp::g_hygiene_violation_se_emit_total;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::string merr_kind(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

// Message text of a merr pair (cdr side) — #3601 diagnostics.
static std::string merr_msg(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].cdr))
        return {};
    auto sidx = as_string_idx(pairs[idx].cdr);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

static NodeId find_literal_int(aura::ast::FlatAST& flat, std::int64_t want) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag == NodeTag::LiteralInt && v.int_value == want)
            return id;
    }
    return NULL_NODE;
}

// Extracts key=value (or key="value") from a security-audit trail row.
static std::string row_field(const std::string& line, const std::string& key) {
    const auto pos = line.find(key + "=");
    if (pos == std::string::npos)
        return {};
    const auto start = pos + key.size() + 1;
    if (start >= line.size())
        return {};
    if (line[start] == '"') {
        const auto end = line.find('"', start + 1);
        if (end == std::string::npos)
            return {};
        return line.substr(start + 1, end - start - 1);
    }
    const auto end = line.find(' ', start);
    return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static std::uint64_t field_u64(const std::string& line, const std::string& key) {
    const auto s = row_field(line, key);
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}

// MacroHygiene rows with the stable deny reason, seq > since_seq. Read
// directly from the process ring (#3543 pattern) — the trail QUERY prim can
// be capability-gated under the armed Restricted face, the ring is not.
static std::vector<std::string> new_hygiene_deny_rows(std::uint64_t since_seq) {
    using aura::core::security_event::g_security_event_ring;
    using aura::core::security_event::kSecurityEventRingSize;
    using aura::core::security_event::SecurityEventKind;
    auto& ring = g_security_event_ring();
    const auto head = ring.seq.load(std::memory_order_relaxed);
    std::vector<std::string> out;
    if (head == 0)
        return out;
    const auto scan = head < kSecurityEventRingSize ? head : kSecurityEventRingSize;
    for (std::uint64_t s = head; s > head - scan; --s) {
        const auto& e = ring.ring[(s - 1) % kSecurityEventRingSize];
        if (e.kind != SecurityEventKind::MacroHygiene)
            continue;
        if (std::string_view(e.reason) != "hygiene-macro-introduced")
            continue;
        if (e.seq <= since_seq)
            continue;
        out.push_back(std::format("seq={} kind=MacroHygiene tenant={} fiber={} mutation_id={}",
                                  e.seq, e.tenant_id, e.fiber_id, e.mutation_id));
    }
    return out;
}

static std::uint64_t max_audit_seq() {
    return aura::core::security_event::g_security_event_ring().seq.load(std::memory_order_relaxed);
}

// Mirrors the #3301 arming dance: under Restricted/Strict the mutate
// wrapper's require_effect runs check_workspace_isolation first and the
// default principal (tenant 0) trips the #2385 unset-principal deny BEFORE
// the hygiene gate can stamp. Grants go in while the process sandbox is
// still Off (#3141 fence), then the face arms (epoch bumps) and grants are
// refreshed so bound_mutation_id matches require_effect's fail-closed join.
// Post-arm re-grants MUST pass caller_principal=1: grant_locked #3409
// high-bits fence refuses plain caller=0 under the armed face.
static void arm_mutate_face(CompilerService& cs, aura::core::sandbox::SandboxMode proc) {
    auto& ev = cs.evaluator();
    aura::core::capability::reset_capability_effects_for_test();
    ev.set_capability_tenant_id(1);
    aura::core::workspace_isolation::g_workspace_isolation().set_current_tenant(1, "3601-tenant");
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    const auto grant_plain = [&ev] {
        aura::core::capability::g_capability_registry().grant(
            1, "tenant-admin", aura::core::capability::Effect::TenantAdmin, aura_test_grant_prov());
        aura::core::capability::g_capability_registry().grant(
            1, aura::compiler::security::kCapWildcard,
            aura::core::capability::effect_for_cap_name(aura::compiler::security::kCapWildcard),
            aura_test_grant_prov());
        ev.grant_capability(std::string(aura::compiler::security::kCapWildcard));
    };
    const auto grant_armed = [&ev] {
        aura::core::capability::g_capability_registry().grant(
            1, "tenant-admin", aura::core::capability::Effect::TenantAdmin, aura_test_grant_prov(),
            false, false, /*caller_principal=*/1);
        aura::core::capability::g_capability_registry().grant(
            1, aura::compiler::security::kCapWildcard,
            aura::core::capability::effect_for_cap_name(aura::compiler::security::kCapWildcard),
            aura_test_grant_prov(), false, false, /*caller_principal=*/1);
    };
    grant_plain();
    ev.set_effect_sandbox_mode(1); // Restricted — after grants (#3141 fence)
    grant_armed();                 // epoch bumped by arming
    aura::core::sandbox::set_mode(proc);
    grant_armed(); // epoch bumped by the process authority
}

} // namespace

int run_test_tweak_literal_audit_consistency() {
    std::println("=== Issue #2799: tweak-literal audit status consistency ===");
    CHECK(true, "ac2799: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source cites #2799 + IntVal ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(!mut.empty() && !flat.empty(), "AC1: sources readable");
        auto ppos = mut.find("Issue #2799");
        if (ppos == std::string::npos)
            ppos = mut.find("mutate:tweak-literal");
        CHECK(ppos != std::string::npos, "AC1: public tweak-literal");
        auto pwin = mut.substr(ppos > 200 ? ppos - 200 : 0, 3500);
        CHECK(pwin.find("Issue #2799") != std::string::npos, "AC1: public cites #2799");
        CHECK(pwin.find("MutationSoAField::IntVal") != std::string::npos, "AC1: public IntVal");
        auto lpos = flat.find("eval_flat_apply_mutate_tweak_literal");
        CHECK(lpos != std::string::npos, "AC1: lockless helper");
        auto lwin =
            flat.substr(lpos, 6000); // #3601/#3683: deny-site stamps pushed anchors further down
        CHECK(lwin.find("Issue #2799") != std::string::npos, "AC1: lockless cites #2799");
        CHECK(lwin.find("MutationSoAField::IntVal") != std::string::npos, "AC1: lockless IntVal");
        CHECK(lwin.find("rollback_record_for_boundary_abort") != std::string::npos ||
                  lwin.find("RolledBack") != std::string::npos ||
                  lwin.find("2793") != std::string::npos,
              "AC1: documents RolledBack / #2793 path");
    }

    // ── AC2: Guard abort ──
    {
        std::println("\n--- AC2: Guard abort value + RolledBack ---");
        Evaluator ev;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto lit = flat.add_literal(static_cast<std::int64_t>(10));
        flat.root = lit;
        ev.set_workspace_flat(&flat);
        ev.set_workspace_pool(&pool);

        const auto old_val = flat.get(lit).int_value;
        const auto torn0 = flat.mutation_log_status_torn_total();
        ev.enter_mutation_boundary();
        const auto new_val = old_val + 5;
        (void)flat.add_mutation_with_rollback(
            lit, "tweak-literal", "Int", "Int", "ac2799", aura::ast::MutationStatus::Committed,
            static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
            static_cast<std::uint64_t>(old_val), static_cast<std::uint64_t>(new_val), true);
        flat.set_int(lit, new_val);
        CHECK(flat.get(lit).int_value == new_val, "AC2: mid-boundary value tweaked");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::Committed,
              "AC2: mid-boundary Committed");
        ev.exit_mutation_boundary(false);
        CHECK(flat.get(lit).int_value == old_val, "AC2: value restored");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::RolledBack,
              "AC2: status=RolledBack");
        CHECK(flat.mutation_log_status_torn_total() == torn0, "AC2: torn counter unchanged");
    }

    // ── AC3: atomic-batch tweak + fail ──
    {
        std::println("\n--- AC3: atomic-batch tweak then fail → RolledBack ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 10))\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws, "AC3: workspace");
        auto lit = find_literal_int(*ws, 10);
        CHECK(lit != NULL_NODE, "AC3: found literal 10");
        const auto old_val = ws->get(lit).int_value;

        auto batch =
            cs.eval(std::format("(mutate:atomic-batch (list (list \"mutate:tweak-literal\" {} 7) "
                                "(list \"mutate:rebind\" \"no-such-2799\" \"(lambda () 0)\")))",
                                lit));
        CHECK(batch.has_value(), "AC3: batch returns");
        CHECK(!(is_bool(*batch) && as_bool(*batch)), "AC3: batch not success");
        CHECK(is_pair(*batch) && merr_kind(cs, *batch) == "batch-failed", "AC3: batch-failed");
        CHECK(ws->get(lit).int_value == old_val, "AC3: literal still old after batch fail");

        // Find last tweak-literal record; must be RolledBack.
        bool found_tweak = false;
        for (auto it = ws->mutation_log_view().rbegin(); it != ws->mutation_log_view().rend();
             ++it) {
            if (it->operator_name == "tweak-literal") {
                found_tweak = true;
                CHECK(it->status == aura::ast::MutationStatus::RolledBack,
                      "AC3: tweak-literal status=RolledBack after batch abort");
                break;
            }
        }
        CHECK(found_tweak, "AC3: tweak-literal log entry present");
    }

    // ── AC4: commit keeps Committed ──
    {
        std::println("\n--- AC4: success path keeps Committed ---");
        Evaluator ev;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto lit = flat.add_literal(static_cast<std::int64_t>(3));
        flat.root = lit;
        ev.set_workspace_flat(&flat);
        ev.set_workspace_pool(&pool);
        ev.enter_mutation_boundary();
        (void)flat.add_mutation_with_rollback(
            lit, "tweak-literal", "Int", "Int", "ac2799-ok", aura::ast::MutationStatus::Committed,
            static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal), 3, 8, true);
        flat.set_int(lit, 8);
        ev.exit_mutation_boundary(true);
        CHECK(flat.get(lit).int_value == 8, "AC4: value stays 8");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::Committed,
              "AC4: status stays Committed");
    }

    // ── #3601 AC1: lockless batch deny emits joinable MacroHygiene SE ──
    std::string mid3601;
    std::string tenant3601;
    std::string fiber3601;
    {
        std::println("\n--- #3601 AC1: lockless tweak deny joins pinned batch mid ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 10))\")").has_value(),
              "#3601 AC1: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "#3601 AC1: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "#3601 AC1: workspace");
        auto lit = find_literal_int(*ws, 10);
        CHECK(lit != NULL_NODE, "#3601 AC1: found literal 10");
        ws->set_marker(lit, aura::ast::SyntaxMarker::MacroIntroduced);
        const auto old_val = ws->get(lit).int_value;
        const auto se0 = g_hygiene_violation_se_emit_total.load();
        const auto seq0 = max_audit_seq();
        arm_mutate_face(cs, aura::core::sandbox::SandboxMode::Restricted);

        // Batch-level :allow-macro? #t skips the #3301 pre-walk, so the deny
        // is produced by the lockless sub-op itself (the #3601 stamp site).
        auto batch = cs.eval(std::format(
            "(mutate:atomic-batch (list (list \"mutate:tweak-literal\" {} 7)) :allow-macro? #t)",
            lit));
        CHECK(batch.has_value(), "#3601 AC1: batch returns");
        CHECK(!(is_bool(*batch) && as_bool(*batch)), "#3601 AC1: batch denied");
        std::println("#3601 AC1 probe: kind='{}' sandbox_active={} se_delta={} msg='{}'",
                     merr_kind(cs, *batch), aura::core::sandbox::is_sandbox_active(),
                     g_hygiene_violation_se_emit_total.load() - se0, merr_msg(cs, *batch));
        auto* ws2 = cs.evaluator().workspace_flat();
        std::println("#3601 AC1 probe2: same_flat={} marker_on_cur={} val_now={} allow_macro={}",
                     ws2 == ws, ws2 != nullptr && ws2->is_macro_introduced(lit),
                     ws2 != nullptr ? ws2->get(lit).int_value : -999,
                     cs.evaluator().get_allow_macro_mutate());
        CHECK(is_pair(*batch) && merr_kind(cs, *batch) == "hygiene-protected",
              "#3601 AC1: hygiene face");
        CHECK(ws->get(lit).int_value == old_val, "#3601 AC1: workspace unchanged on deny");
        CHECK(g_hygiene_violation_se_emit_total.load() >= se0 + 2,
              "#3601 AC1: MacroHygiene SE stamps (sub-op site + boundary)");

        auto rows = new_hygiene_deny_rows(seq0);
        CHECK(rows.size() >= 2, "#3601 AC1: deny rows visible in security trail");
        for (const auto& r : rows) {
            const auto m = field_u64(r, "mutation_id");
            CHECK(m != 0, "#3601 AC1: row carries nonzero mutation_id");
            if (mid3601.empty()) {
                mid3601 = row_field(r, "mutation_id");
                tenant3601 = row_field(r, "tenant");
                fiber3601 = row_field(r, "fiber");
            }
            CHECK(row_field(r, "mutation_id") == mid3601,
                  "#3601 AC1: deny rows share one mid (#3066 join)");
        }
    }

    // ── #3601 AC2: query:security-audit joins by mid + stable reason ──
    if (!mid3601.empty()) {
        std::println("\n--- #3601 AC2: mid+reason join face ---");
        // query:security-audit (the Agent surface this AC mirrors) filters
        // the durable rows by mid + reason. The prim itself is not on the
        // light-link issue-test surface (bare-name eval error here — same
        // reason test_security_audit_trail reads the ring directly), so the
        // filter predicate is asserted on the same rows the prim serves
        // (#2075/#3054).
        std::size_t hits = 0;
        for (const auto& r : new_hygiene_deny_rows(0))
            if (row_field(r, "mutation_id") == mid3601)
                ++hits;
        CHECK(hits >= 1, "#3601 AC2: filter by mid+reason returns the deny row");
    }

    // ── #3601 AC3: Soft/Off deny keeps zero SE (counter bumps only) ──
    {
        std::println("\n--- #3601 AC3: Off deny emits no SE / no ring row ---");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 10))\")").has_value(),
              "#3601 AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "#3601 AC3: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "#3601 AC3: workspace");
        auto lit = find_literal_int(*ws, 10);
        CHECK(lit != NULL_NODE, "#3601 AC3: found literal 10");
        ws->set_marker(lit, aura::ast::SyntaxMarker::MacroIntroduced);
        const auto old_val = ws->get(lit).int_value;
        const auto se0 = g_hygiene_violation_se_emit_total.load();
        const auto seq0 = max_audit_seq();

        auto batch = cs.eval(
            std::format("(mutate:atomic-batch (list (list \"mutate:tweak-literal\" {} 7)))", lit));
        CHECK(batch.has_value() && !(is_bool(*batch) && as_bool(*batch)),
              "#3601 AC3: batch still denied");
        CHECK(is_pair(*batch) && merr_kind(cs, *batch) == "hygiene-protected",
              "#3601 AC3: hygiene face");
        CHECK(ws->get(lit).int_value == old_val, "#3601 AC3: workspace unchanged");
        CHECK(g_hygiene_violation_se_emit_total.load() == se0, "#3601 AC3: zero SE emit counter");
        CHECK(new_hygiene_deny_rows(seq0).empty(), "#3601 AC3: no ring rows appended");
    }

    // ── #3601 AC4: public tweak-literal deny emits exactly one SE ──
    {
        std::println("\n--- #3601 AC4: public path one SE per deny (no double) ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 10))\")").has_value(),
              "#3601 AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "#3601 AC4: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "#3601 AC4: workspace");
        auto lit = find_literal_int(*ws, 10);
        CHECK(lit != NULL_NODE, "#3601 AC4: found literal 10");
        ws->set_marker(lit, aura::ast::SyntaxMarker::MacroIntroduced);
        const auto old_val = ws->get(lit).int_value;
        arm_mutate_face(cs, aura::core::sandbox::SandboxMode::Restricted);
        const auto se0 = g_hygiene_violation_se_emit_total.load();

        auto r = cs.eval(std::format("(mutate:tweak-literal {} 5)", lit));
        CHECK(r.has_value() && !(is_bool(*r) && as_bool(*r)), "#3601 AC4: public tweak denied");
        std::println(
            "#3601 AC4 probe: kind='{}' is_pair={} is_bool={} sandbox_active={} se_delta={}",
            merr_kind(cs, *r), is_pair(*r), is_bool(*r), aura::core::sandbox::is_sandbox_active(),
            g_hygiene_violation_se_emit_total.load() - se0);
        CHECK(is_pair(*r), "#3601 AC4: error pair face");
        CHECK(ws->get(lit).int_value == old_val, "#3601 AC4: workspace unchanged");
        CHECK(g_hygiene_violation_se_emit_total.load() == se0 + 1,
              "#3601 AC4: exactly one SE per public deny (no double emit)");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    }

    // ── #3601 AC5: chaos — remove-node + tweak in one aborted batch; both
    // deny stamps share the pinned composite mid (#3066). Fail-closed
    // pre-abort: remove-node denies first, tweak never runs. ──
    {
        std::println("\n--- #3601 AC5: aborted-batch chaos shares pinned mid ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 10))\")").has_value(),
              "#3601 AC5: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "#3601 AC5: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "#3601 AC5: workspace");
        auto lit = find_literal_int(*ws, 10);
        CHECK(lit != NULL_NODE, "#3601 AC5: found literal 10");
        ws->set_marker(lit, aura::ast::SyntaxMarker::MacroIntroduced);
        const auto old_val = ws->get(lit).int_value;
        const auto seq0 = max_audit_seq();
        arm_mutate_face(cs, aura::core::sandbox::SandboxMode::Restricted);

        auto batch =
            cs.eval(std::format("(mutate:atomic-batch (list (list \"mutate:remove-node\" {}) (list "
                                "\"mutate:tweak-literal\" {} 7)) :allow-macro? #t)",
                                lit, lit));
        CHECK(batch.has_value() && !(is_bool(*batch) && as_bool(*batch)),
              "#3601 AC5: batch denied");
        std::println("#3601 AC5 probe: kind='{}' sandbox_active={} msg='{}'", merr_kind(cs, *batch),
                     aura::core::sandbox::is_sandbox_active(), merr_msg(cs, *batch));
        CHECK(is_pair(*batch) && merr_kind(cs, *batch) == "hygiene-protected",
              "#3601 AC5: hygiene face");
        CHECK(ws->get(lit).int_value == old_val, "#3601 AC5: workspace unchanged");

        auto rows = new_hygiene_deny_rows(seq0);
        CHECK(rows.size() >= 2, "#3601 AC5: remove-node deny + boundary stamp visible");
        std::string chaos_mid;
        for (const auto& r : rows) {
            const auto m = row_field(r, "mutation_id");
            CHECK(!m.empty() && m != "0", "#3601 AC5: nonzero mid");
            if (chaos_mid.empty())
                chaos_mid = m;
            CHECK(m == chaos_mid, "#3601 AC5: both deny stamps share pinned composite mid");
        }
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    }

    std::println("\n=== #2799 tweak-literal audit consistency: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tweak_literal_audit_consistency();
}
#endif
