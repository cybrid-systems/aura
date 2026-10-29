#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #1998 / B-024 and #898.
#
# The IR MakePair mirror still mallocs a PairSlot and push_backs it onto
# g_owned_pair_slots_. That vector reallocates, so the push stays inside
# aura_lock_workspace_write / aura_unlock_workspace_write.
#
# Issue #4755: the workspace hook is NOT a process-wide exclusion (it binds
# ONE Evaluator at a time, and lock_workspace_unique adopts a shared hold on
# the calling thread), so the push must ALSO take the registry's own
# process-global mutex, g_owned_pair_slots_mtx. Two overlapping fiber bodies
# both "held" the workspace write lock and reallocated this vector at once
# (glibc double free in IRInterpreter::run_function).
#
# aura_alloc_pair (#898) does not take that lock and does not push the
# vector. The slot lives on a thread-local slab (aura_pair_tls_bump).
# A shared fetch_add on that path does not scale; do not add one.
#
#   python3 scripts/coverage/checks/check_owned_pair_slots_lock_coverage.py --self-test
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _strip_cpp_comments(src: str) -> str:
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []
    ir = _strip_cpp_comments(_read("src/compiler/ir_executor_impl.cpp"))
    rt = _strip_cpp_comments(_read("src/compiler/aura_jit_runtime.cpp"))

    needle = "g_owned_pair_slots_.push_back(slot)"
    at = ir.find(needle)
    if at < 0:
        fails.append("IR MakePair no longer records g_owned_pair_slots_.push_back(slot)")
    else:
        win = ir[max(0, at - 600) : at + len(needle) + 200]
        if "aura_lock_workspace_write()" not in win:
            fails.append("IR owned push is not inside aura_lock_workspace_write()")
        if "aura_unlock_workspace_write()" not in win:
            fails.append("IR owned push is missing aura_unlock_workspace_write()")
        lock_at = win.rfind("aura_lock_workspace_write()")
        unlock_at = win.find("aura_unlock_workspace_write()")
        push_at = win.find(needle)
        if not (0 <= lock_at < push_at < unlock_at):
            fails.append("IR lock / push / unlock are out of order")
        # Issue #4755: the process-global registry owns its own mutex.
        owned_at = win.rfind("g_owned_pair_slots_mtx")
        if owned_at < 0 or not (lock_at < owned_at < push_at):
            fails.append(
                "#4755: IR owned push is not under g_owned_pair_slots_mtx "
                "(the workspace hook is not a process-wide lock)"
            )

    alloc_at = rt.find("int64_t aura_alloc_pair(")
    car_at = rt.find("int64_t aura_pair_car(", alloc_at if alloc_at >= 0 else 0)
    if alloc_at < 0 or car_at < 0:
        fails.append("aura_alloc_pair window not found")
    else:
        body = rt[alloc_at:car_at]
        if "aura_lock_workspace_write()" in body:
            fails.append("#898: aura_alloc_pair still takes the workspace write lock")
        if "g_owned_pair_slots_.push_back" in body:
            fails.append("#898: aura_alloc_pair still pushes g_owned_pair_slots_")
        if "aura_pair_tls_bump()" not in body:
            fails.append("#898: aura_alloc_pair does not allocate from the TLS slab")

    if fails:
        print("check_owned_pair_slots_lock_coverage: FAIL")
        for f in fails:
            print(" ", f)
        return 1
    print("check_owned_pair_slots_lock_coverage: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
