#!/usr/bin/env python3
"""Issue #4146 source-cite gate: production facade cone JIT drop.

Under production defaults the facade early-return (#3188/#3345/#3474/#3823)
marked every dependent IR body-dirty, but #3749 dropped jit_cache_ +
AuraJIT native for the ROOT define only. Under owner-scoped multi-eval the
C clocks are intentionally frozen (#2841/#2951/#3605) and dual-fresh only
covers live closure views, so a caller D of mutated F kept executing
pre-mutate JIT/AOT native that still targets F while root F was fail-closed.

#4146 arms `drop_cone_jit_after_production_facade_`: the same cone that
receives IR body-dirty (#3474 called_by BFS ∪ #3823 node-dep dependents)
also drops jit_cache_ + AuraJIT native per dependent (#1378 lock window,
#4100 drop shape), MustDeopts owner live closures named d gated on the
owner-scoped freeze (#3975 walk + TLS apply-epoch bump), and feeds the
#3373 production dirty ring. Soft / Off never reach it (facade-success
path only); no process-wide table epoch bump (#2951 owner scope); no
second closure table.

ACs:
  AC1  cone helper lives in service.ixx, cites #4146, and drops per
       dependent: jit_cache_.erase(dependent) under jit_cache_mtx_,
       aura_drop_jit_fn_native_for_define(dependent), then
       jit_.invalidate(dependent) + invalidate_prefix(dependent) —
       ordered erase → drop → invalidate like the #3749 root drop.
  AC2  cone collection mirrors the IR-dirty set: called_by BFS (#3474
       deque/pop_front) ∪ node-dep dependents (#3823 dependents walk);
       the #3749 root eviction fans out into the cone helper, and both
       production sites still evict after the joint stamp.
  AC3  owner scope preserved: MustDeopt per dependent is gated on
       `aura_aot_last_table_bump_owner_scoped() != 0`, calls
       must_deopt_owner_live_closures_for_define_(dependent) and bumps
       the TLS apply epoch; the helper never touches
       aura_aot_bump_func_table_epoch.
  AC4  cone names feed the production dirty ring behind the
       `aura_production_defaults_active_probe()` gate (#3373 pattern).
  AC5  Soft / Off unchanged: the Soft same-lock erase+invalidate block
       (#491/#1378) stays intact and service_dirty.cpp never calls the
       cone helper (facade-success path only).
  AC6  runtime doors + wiring: test_compiler_hot_update_facade.cpp cites
       #4146 with the dispatched ac4146 runner; no
       tests/compiler/test_issue_4146.cpp; no docs/design/4146-*; no new
       query key; build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SVC = ROOT / "src" / "compiler" / "service_dirty.cpp"
IXX = ROOT / "src" / "compiler" / "service.ixx"
TST = ROOT / "tests" / "compiler" / "test_compiler_hot_update_facade.cpp"

CONE_DEF = "void drop_cone_jit_after_production_facade_(const std::string& name)"
ROOT_EVICT_DEF = "void evict_jit_cache_after_production_facade_(const std::string& name)"


def _strip_comments(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines():
        cut = line.find("//")
        out.append(line[:cut] if cut >= 0 else line)
    return "\n".join(out)


def main() -> int:
    svc_raw = SVC.read_text() if SVC.exists() else ""
    svc = _strip_comments(svc_raw)
    ixx_raw = IXX.read_text() if IXX.exists() else ""
    ixx = _strip_comments(ixx_raw)
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        if not good:
            ok = False
        print(f"  [{'PASS' if good else 'FAIL'}] {name}: {msg}")

    # AC1 — cone helper drops jit_cache_ + native + prefix per dependent,
    # ordered erase → native drop → AuraJIT invalidate inside the #1378
    # jit_cache_mtx_ window shape (#4100: native drop outside the lock).
    fn = ixx.find(CONE_DEF)
    report("AC1.helper", fn != -1, "drop_cone_jit_after_production_facade_ present in service.ixx")
    report(
        "AC1.cite",
        ixx_raw.find("Issue #4146", 0, fn if fn != -1 else len(ixx_raw)) != -1 or "Issue #4146" in ixx_raw,
        "service.ixx cites Issue #4146",
    )
    if fn != -1:
        win = ixx[fn : fn + 4200]
        erase = win.find("jit_cache_.erase(dependent)")
        drop = win.find("aura_drop_jit_fn_native_for_define(dependent.c_str())")
        inval = win.find("jit_.invalidate(dependent.c_str())")
        prefix = win.find("jit_.invalidate_prefix(dependent.c_str())")
        lock = win.find("std::unique_lock cache_write(jit_cache_mtx_)")
        report(
            "AC1.drop",
            erase != -1 and drop != -1 and inval != -1 and prefix != -1,
            "per-dependent erase / native drop / invalidate / prefix present",
        )
        report(
            "AC1.order",
            0 <= erase < drop < inval and inval < prefix and lock != -1 and lock < erase,
            "erase (under jit_cache_mtx_) precedes the native drop and the invalidate pair",
        )
    else:
        report("AC1.drop", False, "cone helper missing")
        report("AC1.order", False, "cone helper missing")

    # AC2 — cone = same set that receives IR body-dirty; root eviction fans
    # out; production sites unchanged.
    if fn != -1:
        win = ixx[fn : fn + 4200]
        bfs = win.find("std::deque<std::string> bfs")
        pop = win.find("bfs.pop_front()")
        called_by = win.find("dit->second.called_by")
        node_dep = win.find("node_dep_graph_.dependents(encode_fn_node")
        report(
            "AC2.called_by_bfs",
            bfs != -1 and pop != -1 and called_by != -1,
            "called_by BFS (#3474 shape) inside the cone helper",
        )
        report(
            "AC2.node_dep_union",
            node_dep != -1,
            "node-dep dependents union (#3823 shape) inside the cone helper",
        )
    else:
        report("AC2.called_by_bfs", False, "cone helper missing")
        report("AC2.node_dep_union", False, "cone helper missing")
    ev = ixx.find(ROOT_EVICT_DEF)
    if ev != -1:
        ewin = ixx[ev : ev + 1200]
        report(
            "AC2.fanout",
            ewin.find("drop_cone_jit_after_production_facade_(name);") != -1,
            "root eviction calls the cone drop (#3749 root kept)",
        )
    else:
        report("AC2.fanout", False, "root eviction helper missing")
    for site, bound in (
        ("void CompilerService::mark_define_dirty", 9000),
        (
            "void CompilerService::invalidate_function",
            13000,
        ),
    ):
        pos = svc.find(site)
        end = svc.find("\nvoid CompilerService::", pos + 1)
        swin = svc[pos : (end if end != -1 else pos + bound)]
        stamp = swin.find("stamp_eval_core_joint_after_production_facade_(name)")
        evict = swin.find("evict_jit_cache_after_production_facade_(name)")
        report(
            f"AC2.{site.rsplit('::', 1)[1]}_evict_after_stamp",
            stamp != -1 and evict != -1 and evict > stamp,
            "production site evicts after the joint stamp (facade-success only)",
        )

    # AC3 — owner scope preserved (no process-wide table bump; gated
    # name-precise MustDeopt + TLS apply-epoch bump).
    if fn != -1:
        win = ixx[fn : fn + 4200]
        gate = win.find("aura_aot_last_table_bump_owner_scoped() != 0")
        deopt = win.find("must_deopt_owner_live_closures_for_define_(dependent)")
        bump = win.find("bump_closures_apply_epoch_public()")
        report(
            "AC3.gated_mustdeopt",
            gate != -1 and deopt != -1 and gate < deopt,
            "MustDeopt per dependent gated on the owner-scoped freeze (#3975 walk)",
        )
        report(
            "AC3.apply_epoch",
            bump != -1,
            "TLS apply epoch bumped after the cone MustDeopt walk",
        )
        report(
            "AC3.no_process_bump",
            win.find("aura_aot_bump_func_table_epoch") == -1,
            "helper never bumps the process table epoch (#2951 owner scope)",
        )
    else:
        report("AC3.gated_mustdeopt", False, "cone helper missing")
        report("AC3.apply_epoch", False, "cone helper missing")
        report("AC3.no_process_bump", False, "cone helper missing")

    # AC4 — cone names feed the production dirty ring behind the probe gate.
    if fn != -1:
        win = ixx[fn : fn + 4200]
        probe = win.find("aura_production_defaults_active_probe() != 0")
        ring = win.find("aura_production_dirty_ring_push(dependent.c_str()")
        report(
            "AC4.dirty_ring",
            probe != -1 and ring != -1 and probe < ring,
            "per-dependent ring push behind the production probe gate (#3373)",
        )
    else:
        report("AC4.dirty_ring", False, "cone helper missing")

    # AC5 — Soft / Off unchanged (facade not taken → no cone drop).
    report(
        "AC5.soft_block",
        svc_raw.find("Issue #491 + #1378: erase jit_cache_ AND jit_.invalidate") != -1,
        "Soft same-lock erase+invalidate block intact",
    )
    report(
        "AC5.soft_no_call",
        svc.find("drop_cone_jit_after_production_facade_") == -1,
        "service_dirty.cpp never calls the cone helper (facade-success path only)",
    )

    # AC6 — runtime doors + wiring; no invented test file / doc / query key.
    runner = "ac4146_production_facade_cone_jit_drop"
    report(
        "AC6.tests",
        "#4146" in tst and f"static void {runner}()" in tst and tst.count(f"{runner}();") == 1,
        "test_compiler_hot_update_facade.cpp cites #4146 with the dispatched ac4146 runner",
    )
    no_test_file = not (ROOT / "tests" / "compiler" / "test_issue_4146.cpp").exists()
    no_doc = not any((ROOT / "docs" / "design").glob("4146-*")) if (ROOT / "docs" / "design").exists() else True
    report("AC6.no_invent", no_test_file and no_doc, "no tests/compiler/test_issue_4146.cpp; no docs/design/4146-*")
    report(
        "AC6.no_query_key",
        "schema-4146" not in svc_raw and "schema-4146" not in ixx_raw,
        "no new query key",
    )
    report(
        "AC6.wiring",
        "check_facade_cone_jit_drop_4146.py" in build and "check_facade_cone_jit_drop_4146.py" in allow,
        "build.py wires this linter; root_check_allowlist.txt lists it",
    )

    print(f"check_facade_cone_jit_drop_4146: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
