#!/usr/bin/env python3
"""Issue #4727 AC3 after ir_exec_const left run_function.

The quoted-symbol atom is still interned by the same evaluator, from the
same module string-pool slot, and run_function still dispatches ConstString
into that helper. A file-wide search for intern_symbol is not the contract.

Counterexamples (same predicates, must fail):
  - intern owner is not st.context.evaluator
  - the argument is not the content view filled from st.module.string_pool
  - ConstString is not sent to ir_exec_const(st, ...)
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _function_body(text: str, signature: str) -> str:
    start = text.find(signature)
    if start < 0:
        return ""
    i = text.find("{", start)
    if i < 0:
        return ""
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            nxt = text.find("\n", i)
            i = n if nxt < 0 else nxt
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if c in "\"'":
            q = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    return ""


def _symbol_branch(const_body: str) -> str:
    start = const_body.find("if (ops[2] == 2)")
    end = const_body.find("if (ops[2] != 0)", start if start >= 0 else 0)
    if start < 0 or end <= start:
        return ""
    return const_body[start:end]


def _intern_ok(const_body: str) -> str | None:
    branch = _symbol_branch(const_body)
    if not branch:
        return "ops[2] == 2 branch missing from ir_exec_const"
    pool = branch.find("content = st.module.string_pool[pool_idx]")
    call = branch.find("st.context.evaluator->intern_symbol(content)")
    if pool < 0 or call < 0 or not (pool < call):
        return "symbol atom is not intern_symbol(content) after content = st.module.string_pool[pool_idx]"
    if "intern_keyword" in branch:
        return "symbol branch calls intern_keyword"
    if "->intern_symbol(" in branch.replace("st.context.evaluator->intern_symbol(content)", "", 1):
        return "symbol branch has a second intern_symbol"
    return None


def _dispatch_ok(run_body: str) -> str | None:
    marker = run_body.find("case IROpcode::ConstString:")
    if marker < 0:
        return "run_function has no ConstString case"
    window = run_body[marker : marker + 500]
    call = window.find("ir_exec_const(st,")
    if call < 0:
        return "ConstString does not call ir_exec_const(st, ...)"
    init = run_body.find("IrFnState st{")
    if init < 0 or init > marker:
        return "IrFnState is not built before the ConstString dispatch"
    init_end = run_body.find("};", init)
    if init_end < 0:
        return "IrFnState initializer does not close"
    bound = run_body[init:init_end]
    for needle in ("module_", "context_", "const_symbol_cache_"):
        if needle not in bound:
            return f"IrFnState is not bound to this interpreter's {needle}"
    return None


def main() -> int:
    fails: list[str] = []
    impl = _read("src/compiler/ir_executor_impl.cpp")
    const_body = _function_body(impl, "static IrFlow ir_exec_const(")
    run_body = _function_body(impl, "IRInterpreter::RunResult IRInterpreter::run_function(")
    if not const_body:
        fails.append("AC3: ir_exec_const body missing")
    else:
        why = _intern_ok(const_body)
        if why:
            fails.append(f"AC3: {why}")
    if not run_body:
        fails.append("AC3: run_function body missing")
    else:
        why = _dispatch_ok(run_body)
        if why:
            fails.append(f"AC3: {why}")

    # Wrong owner: a different receiver still named intern_symbol(content).
    wrong_owner = const_body.replace(
        "st.context.evaluator->intern_symbol(content)",
        "other.evaluator->intern_symbol(content)",
        1,
    )
    if const_body and _intern_ok(wrong_owner) is None:
        fails.append("AC3 negative: wrong intern owner was accepted")

    # Wrong text: the pool slot is copied into `name`, then interned.
    wrong_content = const_body.replace(
        "st.context.evaluator->intern_symbol(content)",
        "st.context.evaluator->intern_symbol(name)",
        1,
    )
    if const_body and _intern_ok(wrong_content) is None:
        fails.append("AC3 negative: intern of a non-content argument was accepted")

    # Dispatch not wired: only the ConstString arm loses ir_exec_const.
    marker = run_body.find("case IROpcode::ConstString:")
    window_end = marker + 500 if marker >= 0 else 0
    unwired = (
        run_body[:marker]
        + run_body[marker:window_end].replace("ir_exec_const(st,", "ir_exec_arith(st,", 1)
        + run_body[window_end:]
        if marker >= 0
        else ""
    )
    if run_body and _dispatch_ok(unwired) is None:
        fails.append("AC3 negative: ConstString not reaching ir_exec_const was accepted")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: #4727 ConstString intern_symbol(content) via ir_exec_const(st)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
