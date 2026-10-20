#!/usr/bin/env python3
"""Issue #4615: mutate:rebind cascade prefix sums are O(1) per block.

mark_body_only_dirty() looped every body block and called
cascade_block_to_instructions(body_idx, bi). Each call re-summed
instructions.size() over *all* blocks for the total and over blocks 0..bi for
the prefix base, so one body-dirty mark was O(B^2) in the calling define's
basic blocks (the issue's K=4000 caller: ~250 ms per rebind of a callee that
caller references). is_instruction_dirty() recomputed the same prefix on
every query, so any loop over the function's instructions was O(B^2) too.

Contract:
  AC1 IRCacheEntry caches per-function block→instruction prefix offsets
      (block_inst_offsets_) rebuilt lazily once per irs layout epoch.
  AC2 cascade_block_to_instructions reads the cached offsets; no per-block
      total scan and no 0..block_idx prefix scan.
  AC3 is_instruction_dirty / mark_instruction_dirty / clear_instruction_dirty
      read the cached prefix (O(1) per query).
  AC4 mark_body_only_dirty batches the whole body span
      (mark_body_instructions_dirty_ = one resize + one std::fill); the
      per-block cascade call is gone.
  AC5 every write to irs[] in src/compiler/service.ixx invalidates the
      offset cache (invalidate_irs_layout_ within the next 6 lines). A bare
      .irs.clear() is exempt: irs.size() drops, and the accessor's
      size-mismatch check rebuilds the table before the next lookup.
  AC6 runtime evidence: tests/python/test_regression.py
      test_rebind_cascade_linear_4615 is registered in the runner (correctness
      of redefinition + cascade, plus a scaling guard).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _body(text: str, sig: str) -> str:
    """Body text between the braces that follow the first occurrence of sig."""
    i = text.find(sig)
    if i < 0:
        return ""
    j = text.find("{", i)
    if j < 0:
        return ""
    depth = 0
    for k in range(j, len(text)):
        c = text[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[j : k + 1]
    return text[j:]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    svc = _read("src/compiler/service.ixx")
    if not svc:
        print("check_block_prefix_offsets_4615: src/compiler/service.ixx missing")
        return 1

    # AC1 cached prefix table + epoch invalidation
    must("mutable std::vector<std::vector<std::size_t>> block_inst_offsets_;", "AC1", svc)
    must("mutable std::uint64_t block_inst_offsets_epoch_ = 0;", "AC1", svc)
    must("std::uint64_t irs_layout_epoch_ = 0;", "AC1", svc)
    must("void invalidate_irs_layout_() noexcept", "AC1", svc)
    must("block_inst_offsets_epoch_ = irs_layout_epoch_;", "AC1", svc)
    must("offs.assign(blocks.size() + 1, 0);", "AC1", svc)
    must("offs[bi + 1] = acc;", "AC1", svc)

    # AC2 cascade reads the cache, never rescans blocks
    cascade_sig = "void cascade_block_to_instructions("
    cascade = _body(svc, cascade_sig)
    if not cascade:
        fails.append(f"AC2: no body for {cascade_sig!r}")
    else:
        must("block_inst_offsets_at_(func_idx)", "AC2", cascade)
        must_not("for (const auto& b : func.blocks)", "AC2", cascade)
        must_not("bi < block_idx", "AC2", cascade)

    # AC3 per-instruction queries are O(1)
    for sig, label in (
        ("bool is_instruction_dirty(", "AC3 is_instruction_dirty"),
        ("void mark_instruction_dirty(", "AC3 mark_instruction_dirty"),
        ("void clear_instruction_dirty(", "AC3 clear_instruction_dirty"),
    ):
        body = _body(svc, sig)
        if not body:
            fails.append(f"{label}: no body for {sig!r}")
            continue
        must("block_inst_offsets_at_(func_idx)", label, body)
        must_not("bi < block_idx", label, body)

    # AC4 mark_body_only_dirty is a single batch fill
    body_only = _body(svc, "std::size_t mark_body_only_dirty(")
    if not body_only:
        fails.append("AC4: no body for mark_body_only_dirty")
    else:
        must("mark_body_instructions_dirty_(body_idx, fb.size());", "AC4", body_only)
        must_not("cascade_block_to_instructions(body_idx,", "AC4", body_only)
        must_not("bi < block_idx", "AC4", body_only)
    batch = _body(svc, "void mark_body_instructions_dirty_(")
    if not batch:
        fails.append("AC4: no body for mark_body_instructions_dirty_")
    else:
        must("std::fill(", "AC4", batch)
        must("block_inst_offsets_at_(func_idx)", "AC4", batch)
        must_not("bi < block_idx", "AC4", batch)
        must_not("for (const auto& b : func.blocks)", "AC4", batch)

    # AC5 every irs[] write invalidates the cached layout
    lines = svc.splitlines()
    write_re = re.compile(r"\.irs\s*=|(\.irs\[\s*[^\]]*\]\s*=\s*std::move)|\.irs\.insert\(")
    for idx, line in enumerate(lines):
        if not write_re.search(line):
            continue
        window = "\n".join(lines[idx : idx + 7])
        if "invalidate_irs_layout_" not in window:
            fails.append(
                f"AC5: irs write at src/compiler/service.ixx:{idx + 1} has no "
                f"invalidate_irs_layout_() within 6 lines: {line.strip()!r}"
            )

    # AC6 runtime evidence registered in the regression net
    reg = _read("tests/python/test_regression.py")
    must("def test_rebind_cascade_linear_4615()", "AC6", reg)
    must("test_rebind_cascade_linear_4615,", "AC6", reg)
    must("mutate:rebind", "AC6", reg)

    if fails:
        print("Issue #4615 block-prefix offset linter FAILED:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("Issue #4615 block-prefix offset linter OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
