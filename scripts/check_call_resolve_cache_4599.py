#!/usr/bin/env python3
"""Issue #4599 source-cite gate (phase 2: call-path name resolution).

Phase 1 (c6a18e8a5) cached prim calls + cells per tree-walker node.
This phase finishes payoff step 1 on the call path — a hot call performs
no StringPool resolve and no string-keyed env / prim-table probes:

  §1  Local-shadow skip. A body-pool frame binding for the callee name
      answers Env::lookup before the prim table can, so the prim prelude
      (pool resolve + std::string + env probe + prim-table probe) is
      skipped when the SymId hit holds a non-prim value. SymIds are
      pool-local: the frame pool must be the call-site pool and the
      string mirror aligned. A prim-VALUED local keeps the string
      prelude (today's path invokes the prim with try-face error
      handling the value-apply fallback lacks).
  §2  CallNoPrim per-node decision cache. When the prelude computes
      "not a prim", the decision is stored on the call node (host only,
      string-mirror aligned, top binding-generation + capture-snapshot
      guarded, top SymId hits must agree with the string probe's value).
      Prim-table names are refused: an unbind between visits could flip
      the decision to the prim. Consult reconstructs a Prim entry via
      name_for_slot + slot_lookup_fast without the probes.
  §3  and/or clause cache. The short-circuit block currently resolves
      the callee name on every visit before the prim prelude runs. The
      clause list is cached per node (no env guards — the decision is
      the name table's, not the env's) and re-checked against the live
      flat size on consult.

The invalidation contract is phase 1's: every top bind / unbind bumps
top_bind_gen_ via note_top_binding_changed, and pool / flat switches
clear the whole cache (bind_sym_intern_pool / bind_float_intern_flat).

ACs:
  AC1  evaluator.ixx: NameResEntry grows CallNoPrim + ShortCircuit
       kinds; the ShortCircuit store carries no generation / snapshot
       guards (aux is the and/or marker); the remember helper
       declarations exist.
  AC2  evaluator_eval_flat.cpp §3: the short-circuit branch consults
       the cached clause list before the resolve (sym match, child
       validity vs f->size(), and/or marker honored, tail continue),
       and both arms remember the clause ids (children.subspan(1)).
  AC3  evaluator_eval_flat.cpp §1: the prim prelude fetches the local
       SymId hit under the pool-identity + mirror-alignment + non-top_
       guards, skips the resolve for non-prim locals, and keeps the
       prelude for prim-valued locals.
  AC4  evaluator_eval_flat.cpp §2: the consult matches sym +
       top_bind_gen_ + capture snapshot; a Prim entry reconstructs
       cname/prim without probes; a CallNoPrim entry sets the skip.
  AC5  evaluator_eval_flat.cpp: the store path refuses prim-table
       names (slot_for_name), top SymId collisions (value agreement
       with the string probe), fiber and mirror-misaligned states, and
       keeps PrimCall entries winning.
  AC6  evaluator_env.cpp: the phase-1 invalidation contract this phase
       leans on is still wired — note_top_binding_changed fires on
       bind_symid_with_linear_state, bind_with_linear_state,
       unbind_local, and unbind_local_symid, top_-only.
  AC7  tests/compiler/test_pmr_alloc_fiber_safe.cpp:
       ac4599_call_resolve_cache registered in
       run_test_pmr_alloc_fiber_safe; zero pool resolves on a hot
       self-TCO loop, the bind-mirror resolve budget for a loop with
       one materialized call per iteration, late-define invalidation,
       and the 1.2x 4000-define relative budget. No
       tests/**/test_issue_4599*.cpp, no docs/design/4599-* (per
       #1655).
  AC8  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
EFL = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
ENV = ROOT / "src" / "compiler" / "evaluator_env.cpp"
TST = ROOT / "tests" / "compiler" / "test_pmr_alloc_fiber_safe.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i : j if j > 0 else len(text)]


def main() -> int:
    ixx = IXX.read_text()
    efl = EFL.read_text()
    env = ENV.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: cache shapes ────────────────────────────────────────────
    must(
        "CallNoPrim = 4" in ixx and "ShortCircuit = 5" in ixx, "AC1: NameResEntry grows CallNoPrim + ShortCircuit kinds"
    )
    sc_store = _between(
        efl, "void Evaluator::remember_short_circuit", "std::optional<EvalResult> Evaluator::try_eval_flat_hot"
    )
    must(
        "e.top_gen = 0;" in sc_store and "e.cap = nullptr;" in sc_store,
        "AC1: ShortCircuit store carries no generation / snapshot guards",
    )
    must("e.aux = is_and ? 1u : 0u;" in sc_store, "AC1: aux is the and/or marker")
    must(
        "void remember_call_no_prim(aura::ast::NodeId id, aura::ast::SymId sym, const Env& env," in ixx
        and "void remember_short_circuit(aura::ast::NodeId id, aura::ast::SymId sym, bool is_and," in ixx,
        "AC1: remember helper declarations exist",
    )

    # ── AC2: and/or consult before the resolve + clause remember ─────
    andor = _between(
        efl,
        "// and/or: short-circuit evaluation (not eager arg eval)",
        "// Primitive call (all arg evals are recursive).",
    )
    must(bool(andor), "AC2: short-circuit block found")
    must(
        "NameResEntry::Kind::ShortCircuit" in andor
        and "sc.sym == callee.sym_id" in andor
        and "sc.child[sci] >= f->size()" in andor,
        "AC2: consult matches the cached shape and re-checks child validity",
    )
    must(
        "sc_and ? !is_truthy(*ar) : is_truthy(*ar)" in andor and "current_id = sc.child[sc.nchild - 1];" in andor,
        "AC2: and/or marker honored, tail clause continues",
    )
    must(
        "remember_short_circuit(current_id, callee.sym_id, true," in andor
        and "remember_short_circuit(current_id, callee.sym_id, false," in andor
        and andor.count("v.children.subspan(1)") == 2,
        "AC2: both arms remember the clause ids",
    )

    # ── AC3: local-shadow skip ──────────────────────────────────────
    prelude = _between(efl, "Issue #4599: a body-pool frame binding for this name", "if (prim) {")
    must(bool(prelude), "AC3: prim prelude window found")
    must(
        "eval_env.pool() == p" in prelude
        and "&eval_env != &top_" in prelude
        and "eval_env.binding_mirrors_aligned()" in prelude,
        "AC3: SymId hit guarded by pool identity, non-top_, mirror alignment",
    )
    must(
        "callee_local.has_value() && !is_primitive(*callee_local)" in prelude,
        "AC3: non-prim locals skip the prelude; prim-valued locals keep it",
    )

    # ── AC4: CallNoPrim / Prim consult ──────────────────────────────
    consult = _between(efl, "Issue #4599: per-node decision cache (host", "if (!prim && !known_not_prim)")
    must(bool(consult), "AC4: consult window found")
    must(
        "cc.sym == callee.sym_id" in consult
        and "cc.top_gen == top_bind_gen_" in consult
        and "cc.cap == eval_env.cap_snapshot().get()" in consult,
        "AC4: consult matches sym + generation + capture snapshot",
    )
    must(
        "primitives_.name_for_slot(cc.aux)" in consult and "primitives_.slot_lookup_fast(cc.aux)" in consult,
        "AC4: Prim entry reconstructs the prim without probes",
    )
    must(
        "NameResEntry::Kind::CallNoPrim" in consult and "known_not_prim = true;" in consult,
        "AC4: CallNoPrim entry sets the skip",
    )

    # ── AC5: store guards ───────────────────────────────────────────
    store = _between(efl, "void Evaluator::remember_call_no_prim", "void Evaluator::remember_short_circuit")
    must(bool(store), "AC5: store window found")
    must(
        "g_current_fiber != nullptr" in store and "sym == aura::ast::INVALID_SYM" in store,
        "AC5: host-only + valid-sym guards",
    )
    must("!env.binding_mirrors_aligned()" in store, "AC5: mirror alignment required")
    must(
        "primitives_.slot_for_name(cname) != std::numeric_limits<std::size_t>::max()" in store,
        "AC5: prim-table names refused (unbind could flip the decision)",
    )
    must(
        "if (const auto tv = top_.local_symid_last(sym))" in store
        and (
            "!bound || is_primitive(*tv) || !(*tv == *bound)" in store
            # Issue #4599 phase 3: deref-aware agreement — the SymId side may
            # hold the raw closure while the string probe answers the cell.
            or (
                "bool agrees = bound.has_value() && !is_primitive(*tv) && *tv == *bound;" in store
                and "cells_[ci] == *tv" in store
            )
        ),
        "AC5: top SymId hits must agree with the string probe's value",
    )
    must("if (e.kind == NameResEntry::Kind::PrimCall)" in store, "AC5: PrimCall entries keep winning")
    call_site = _between(efl, "Issue #4599: record the not-a-prim decision", "if (prim) {")
    must(
        "remember_call_no_prim(current_id, callee.sym_id," in call_site,
        "AC5: store fires from the prelude's not-prim arm",
    )

    # ── AC6: invalidation contract (phase 1, still wired) ───────────
    bind_sym = _between(env, "void Env::bind_symid_with_linear_state", "void Env::bind_with_linear_state")
    must(
        "this == &owner_->top_env()" in bind_sym and "note_top_binding_changed()" in bind_sym,
        "AC6: param binds on a call frame must not bump the generation; top binds must",
    )
    bind_str = _between(env, "void Env::bind_with_linear_state", "bool Env::unbind_local(")
    must("note_top_binding_changed()" in bind_str, "AC6: string binds bump the top generation")
    unbind = _between(env, "bool Env::unbind_local(", "bool Env::unbind_local_symid(")
    must("note_top_binding_changed()" in unbind, "AC6: unbind_local bumps the top generation")
    unbind_sym = _between(
        env, "bool Env::unbind_local_symid(", "std::optional<types::EvalValue> Env::local_symid_last("
    )
    must("note_top_binding_changed()" in unbind_sym, "AC6: unbind_local_symid bumps the top generation")

    # ── AC7: runtime ACs ────────────────────────────────────────────
    must("static void ac4599_call_resolve_cache()" in tst, "AC7: runtime AC block exists")
    must(
        "ac4599_call_resolve_cache();" in tst.split("int run_test_pmr_alloc_fiber_safe()")[1],
        "AC7: AC block registered in the dispatched runner",
    )
    must("stringpool_intern_concurrent_readers_total" in tst, "AC7: pool-read oracle is the StringPool reader counter")
    must(
        "4599 AC3: hot loop pool reads stay within the 8/iter budget" in tst,
        "AC7: hot self-TCO loop pins the 8/iter pool-read budget",
    )
    must(
        "4599 AC4: call loop pool reads stay within the 16/iter budget" in tst,
        "AC7: materialized-call loop pins the 16/iter pool-read budget",
    )
    must("4599 AC2: late define visible after invalidation" in tst, "AC7: late-define invalidation pinned at runtime")
    must(
        "4599 AC5: 4000-define loop within 1.2x of 0-define" in tst,
        "AC7: 1.2x 4000-define relative budget pinned (#4343/#4350/#4474)",
    )
    must(
        not (ROOT / "tests" / "core" / "test_issue_4599.cpp").exists()
        and not (ROOT / "docs" / "design" / "4599-perf-call-resolve.md").exists(),
        "AC7: no test_issue_4599.cpp, no docs/design/4599-* (per #1655)",
    )

    # ── AC9: phase 3 — same-pool SymId chain read (Variable fast path) ──
    must(
        "auto chain_read_var = [&](const Env& start," in efl,
        "AC9: member chain-read lambda exists (raw parent_ + SoA hops)",
    )
    must(
        "env_frame_shard_mu(env_frame_shard_index(cur))" in efl and "pfr.bindings_symid_.rbegin()" in efl,
        "AC9: SoA hops take the per-frame shard lock and reverse-scan the SymId array",
    )
    must(
        "pfr.pool_ == p" in efl and "pfr.bindings_.size() != pfr.bindings_symid_.size()" in efl,
        "AC9: SoA hops are same-pool + mirror-length guarded (decline on divergence)",
    )
    must("if (cur == static_cast<EnvId>(0))" in efl, "AC9: root frame declines — live top_ stays on the string walk")
    varfast = _between(efl, "host-thread same-pool fast read", "std::string_view name = p->resolve(v.sym_id);")
    must(
        bool(varfast)
        and "g_current_fiber == nullptr" in varfast
        and "eval_env.pool() == p" in varfast
        and "binding_mirrors_aligned()" in varfast,
        "AC9: fast read is host-only, same-pool, mirror-aligned",
    )
    must("is_void(cells_[ci])" in varfast, "AC9: live cells deref through the walk; void cells fall to the named path")

    # ── AC10: phase 3 — deref-aware store agreement (define-bound callees) ──
    rrv = _between(efl, "void Evaluator::remember_resolved_var", "void Evaluator::remember_prim_call")
    must(
        "cells_[ci] == *topv" in rrv and "(void)note_cell(bound);" in rrv,
        "AC10: Cell store compares the re-def cell's content (closure-on-SymId-side face)",
    )
    cnp = _between(efl, "void Evaluator::remember_call_no_prim", "void Evaluator::remember_short_circuit")
    must(
        "cells_[ci] == *tv" in cnp and "!is_primitive(*tv)" in cnp,
        "AC10: CallNoPrim store deref-aware; prims still refused",
    )
    must(
        "stringpool_intern_concurrent_readers_total" in tst and "4599 AC6:" in tst,
        "AC10: runtime ACs cover the walk (free var, set!-cell, re-define, keyword, unbound, shadow)",
    )

    # ── AC8: wiring ─────────────────────────────────────────────────
    must("check_call_resolve_cache_4599.py" in build, "AC8: build.py registers the linter")
    must("check_call_resolve_cache_4599.py" in allow, "AC8: root_check_allowlist.txt lists the linter")

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
