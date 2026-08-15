#!/usr/bin/env python3
"""Issue #3072: every stolen-fiber Ready enqueue is dominated by
steal_safety_transaction Ok (static proof residual of #2844/#2929).

#2844 already greps worker.cpp for exactly one local_queue_.push(stolen)
after StealSafetyDecision::Ok. That is implementation-level: a new helper,
recovery path, or differently-named steal result can enqueue a residual-
armed fiber without matching `push(stolen)`.

#3072 scans every production TU under src/ for steal-result bindings
(try_steal / local_queue_.steal) and asserts each thief Ready enqueue
of those bindings is textually dominated by steal_safety_transaction Ok.
Naked push fails CI. Owner enqueue / post-yield requeue (non-steal
bindings) and victim->enqueue(stolen) (return to victim) stay allowed.

Contract (one row per AC):
  AC1 Linter fails on a new stolen-fiber enqueue without transaction Ok.
  AC2 Existing try_steal_from → transaction → push(stolen) stays green.
  AC3 Soft / sandbox paths unchanged (no new runtime gate).
  AC4 Additive schema-3072 / wired; #2699/#2721/#2844/#2929 preserved.
  AC5 Extend test_steal_complete_restamp_txn; linter wired; no invent.
  AC6 Source-cite; no docs/design/* per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Assignment of a steal result. Covers try_steal() and Chase-Lev steal().
_STEAL_ASSIGN = re.compile(
    r"\b(\w+)\s*=\s*(?:[\w:>-]+(?:->|\.))?(?:try_steal|steal)\s*\(",
)
_PUSH = re.compile(r"local_queue_\.push\s*\(\s*(\w+)\s*\)")
_VICTIM_ENQUEUE = re.compile(r"\b(\w+)\s*->\s*enqueue\s*\(\s*(\w+)\s*\)")
_BARE_ENQUEUE = re.compile(r"(?<![\w.>])enqueue\s*\(\s*(\w+)\s*\)")
_TXN = re.compile(r"steal_safety_transaction\s*\(\s*(\w+)\s*\)")
_OK = re.compile(r"StealSafetyDecision\s*::\s*Ok")


def _code_only(text: str) -> str:
    """Drop // comments so string/order checks ignore documentation."""
    lines: list[str] = []
    for ln in text.splitlines():
        lines.append(ln.split("//", 1)[0])
    return "\n".join(lines)


def classify_stolen_enqueues(text: str) -> list[str]:
    """Return classifier failures for one TU (empty = green).

    Used on production sources and on synthetic snippets so AC1 is
    executable: a naked steal→push snippet must fail.
    """
    fails: list[str] = []
    code = _code_only(text)
    steal_names = {m.group(1) for m in _STEAL_ASSIGN.finditer(code)}
    # Conventional name even if a future TU copies the identifier.
    if ("try_steal" in code or "local_queue_.steal" in code) and "stolen" in code:
        steal_names.add("stolen")

    if not steal_names:
        # No steal-result binding: any push(stolen) is still a bypass.
        if _PUSH.search(code) and "push(stolen)" in code.replace(" ", ""):
            fails.append("local_queue_.push(stolen) without steal binding")
        return fails

    txn_ok_names: set[str] = set()
    for m in _TXN.finditer(code):
        txn_ok_names.add(m.group(1))

    for m in _PUSH.finditer(code):
        arg = m.group(1)
        if arg not in steal_names:
            continue
        prefix = code[: m.start()]
        if not _TXN.search(prefix) or arg not in txn_ok_names:
            fails.append(f"naked local_queue_.push({arg}) without steal_safety_transaction")
            continue
        txn_pos = prefix.rfind(f"steal_safety_transaction({arg})")
        if txn_pos < 0:
            # whitespace-tolerant
            tm = None
            for tm in _TXN.finditer(prefix):
                if tm.group(1) == arg:
                    txn_pos = tm.start()
        if txn_pos < 0 or not _OK.search(prefix[txn_pos:]):
            fails.append(f"local_queue_.push({arg}) not dominated by StealSafetyDecision::Ok")

    for m in _VICTIM_ENQUEUE.finditer(code):
        obj, arg = m.group(1), m.group(2)
        if arg not in steal_names:
            continue
        # Return-to-victim is allowed; thief-side enqueue is not.
        if obj in {"this", "self", "thief"}:
            fails.append(f"{obj}->enqueue({arg}) is a thief Ready enqueue of a steal result")

    for m in _BARE_ENQUEUE.finditer(code):
        arg = m.group(1)
        if arg not in steal_names:
            continue
        # Allow victim->enqueue already matched above; bare enqueue(stolen)
        # inside try_steal_from would be a thief bypass.
        # WorkerThread::enqueue(Fiber* fiber) definition is not a call.
        fails.append(f"bare enqueue({arg}) of steal result (thief bypass)")

    return fails


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _iter_src_tUs() -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    src = ROOT / "src"
    if not src.is_dir():
        return out
    for p in sorted(src.rglob("*")):
        if p.suffix not in {".cpp", ".h", ".hh", ".ixx", ".cc"}:
            continue
        rel = str(p.relative_to(ROOT))
        out.append((rel, p.read_text(encoding="utf-8", errors="replace")))
    return out


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    worker = _read("src/serve/worker.cpp")
    hdr = _read("src/serve/steal_safety.h")
    qjit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")
    lint2844 = _read("scripts/coverage/checks/check_steal_sole_enqueue_gate_2844.py")

    # AC1 — scanner rejects a naked steal→push (synthetic) and scans all src/.
    naked = "Fiber* stolen = victim->try_steal();\nlocal_queue_.push(stolen);\n"
    naked_fails = classify_stolen_enqueues(naked)
    if not naked_fails:
        fails.append("AC1: classifier accepted naked steal→push (must fail CI)")

    renamed = "Fiber* cand = victim->try_steal();\nlocal_queue_.push(cand);\n"
    if not classify_stolen_enqueues(renamed):
        fails.append("AC1: classifier accepted renamed steal→push without transaction")

    thief_enq = "Fiber* stolen = victim->try_steal();\nthis->enqueue(stolen);\n"
    if not classify_stolen_enqueues(thief_enq):
        fails.append("AC1: classifier accepted this->enqueue(stolen) thief bypass")

    for rel, text in _iter_src_tUs():
        for msg in classify_stolen_enqueues(text):
            fails.append(f"AC1 {rel}: {msg}")

    # AC2 — existing production path stays green (order + sole push(stolen)).
    must("steal_safety_transaction(stolen)", "AC2", worker)
    must("StealSafetyDecision::Ok", "AC2", worker)
    must("local_queue_.push(stolen)", "AC2", worker)
    must("Issue #3072", "AC2", worker)
    must("steal_safety_transaction_is_sole_enqueue_gate_for_stolen", "AC2 #2844 marker", worker)
    code_only = _code_only(worker)
    txn_pos = code_only.find("steal_safety_transaction(stolen)")
    ok_pos = code_only.find("StealSafetyDecision::Ok")
    push_pos = code_only.find("local_queue_.push(stolen)")
    if not (0 <= txn_pos < ok_pos < push_pos):
        fails.append("AC2: expected steal_safety_transaction → Ok → local_queue_.push(stolen)")
    stolen_pushes = len(re.findall(r"local_queue_\.push\(stolen\)", code_only))
    if stolen_pushes != 1:
        fails.append(f"AC2: expected exactly 1 code local_queue_.push(stolen), found {stolen_pushes}")
    # Owner / yield requeue must not use the steal binding.
    if "local_queue_.push(fiber)" not in code_only:
        fails.append("AC2: owner/yield local_queue_.push(fiber) missing (non-steal path)")
    must("victim->enqueue(stolen)", "AC2 return-to-victim", worker)

    # AC3 — Soft path unchanged (existing helpers; no new runtime force).
    must("is_steal_snapshot_soft_mode", "AC3", worker)
    must("steal_snapshot_soft_production_locked", "AC3", worker)

    # AC4 — additive query + prior counters
    must("kStealEnqueueSoleGateIssue = 3072", "AC4", hdr)
    must("g_steal_enqueue_sole_gate_wired", "AC4", hdr)
    must_key("schema-3072", "AC4", qjit)
    must_key("issue-3072", "AC4", qjit)
    must_key("steal-enqueue-sole-gate-wired", "AC4", qjit)
    must_key("steal-enqueue-vs-ok-delta", "AC4", qjit)
    must_key("schema-2929", "AC4 preserved", qjit)
    # 2844 schema is source-cite (worker / 2844 linter); transaction surface
    # is schema-2699.
    must("g_steal_safety_transaction_ok_total", "AC4 #2699 ok", hdr)
    must("g_steal_safety_transaction_reject_hard_total", "AC4 #2699 reject", hdr)
    must("g_steal_safety_residual_boundary_unsafe_total", "AC4 #2721 residual", hdr)
    must("kStealSafetyInvariantTableIssue = 2929", "AC4 #2929", hdr)
    must("check_steal_sole_enqueue_gate_2844", "AC4 #2844 linter preserved", lint2844)

    # schema-2844 is optional in query (2844 was source-cite). Require
    # schema-2699 which is the transaction surface.
    must_key("schema-2699", "AC4 #2699 schema", qjit)
    must_key("steal-safety-transaction-ok-total", "AC4 ok key", qjit)

    # AC5 — tests + linter + no invent
    must("ac3072_1_linter_rejects_naked_push", "AC5", test)
    must("ac3072_2_existing_path_green", "AC5", test)
    must("ac3072_3_soft_unchanged", "AC5", test)
    must("ac3072_4_query_keys", "AC5", test)
    must("ac3072_5_source_and_linter", "AC5", test)
    must("ac2844_1_sole_enqueue_gate", "AC5 #2844 preserved", test)
    must("ac2929_1_invariant_table_and_counters", "AC5 #2929 preserved", test)
    must("check_steal_enqueue_sole_gate_3072", "AC5", build)
    if (ROOT / "tests" / "serve" / "test_issue_3072.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_3072.cpp present (forbidden invent)")

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3072-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        # Drop the no-op ternary leftover if I accidentally appended None
        fails = [f for f in fails if f]
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3072 steal enqueue sole-gate static proof — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
