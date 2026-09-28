#!/usr/bin/env python3
"""Issue #4144 source-cite gate: EnvFrame binding-cell densify-tracked
aliases get a lasting slot remap.

`Evaluator::register_known_moving_densify_root_slots` registered lasting
`void**` slots for workspace members, WorkspaceTree layers, RootRemap
stable refs, opaque_heap_ (#3057), modules_ (#3443) and the closures_ map
flat/pool slots (#3647) — but never walked `env_frames_`. A live frame's
own `pool_` member is a raw densify-tracked StringPool* whose only covers
were the observe-only #3210 temp canary (inventory can miss the frame) and
the steal-time #3479 elevation (`refreshed > 0` + production), so between
densify and the next steal a moved pool left a densify-old alias reachable
from EnvFrame walks / cell lookup (lookup_by_intern /
bindings_with_names resolve through pool_) — a missed-remap / UAF bypass
class under high-freq mutate×densify without an intervening steal.
#4144: the densify-entry walk registers `&fr.pool_` as a lasting window
slot (env_frames_ is a deque — element references are stable across
appends; frames die in place via INVALID_VERSION; slot registration is
consumed at window end), so a green Moving window remaps the frame with
it. No second pin registry — the #2889 registration SSOT and the #3569
window-end consume stay unchanged.

ACs:
  AC1  the densify-entry walk covers live EnvFrame pool_ slots: the
       #4144-marked walk under `env_frame_shards_` shared locks pushes
       `&fr.pool_` (const_cast to a lasting void**) for every live frame
       and skips INVALID_VERSION (dead) frames.
  AC2  shard locks release before slot registration: the registration loop
       (`for (void** slot : known_slots)`) follows the walk block — no
       arena lock is ever taken under the shard locks.
  AC3  slot XOR canary (#3368): the pool slot is the cover — no
       note_post_moving_live_ptr_canary_all / note_ffi_opaque_alias_densify_cover
       call in the walk (comment-stripped check).
  AC4  no second registry + additive counter: registration still goes
       through `register_external_root_slot_for_densify_all`, arena.ixx
       keeps the `*slot = it->second` rewrite walk and the window-end
       `external_root_slots_for_densify_.clear()` consume, and the #4144
       counter row (g_moving_envframe_pool_slots_registered_total) is
       additive-only — never read to gate densify behavior.
  AC5  tests cite #4144 (runtime ACs in test_moving_densify_fail_closed.cpp,
       wired into the dispatched runner); no tests/core/test_issue_4144.cpp;
       no docs/design/4144-*; build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MUT = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
ARENA = ROOT / "src" / "core" / "arena.ixx"
HDR = ROOT / "src" / "core" / "densify_consistency_report.h"
TST = ROOT / "tests" / "core" / "test_moving_densify_fail_closed.cpp"

FN = "std::size_t Evaluator::register_known_moving_densify_root_slots()"


def _strip_comments(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines():
        cut = line.find("//")
        out.append(line[:cut] if cut >= 0 else line)
    return "\n".join(out)


def main() -> int:
    mut = MUT.read_text() if MUT.exists() else ""
    arena = ARENA.read_text() if ARENA.exists() else ""
    hdr = HDR.read_text() if HDR.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    fn = mut.find(FN)
    # 9500 chars covers the full walk through the #3210 temp-canary drain.
    win = mut[fn : fn + 9500] if fn >= 0 else ""
    code_only = _strip_comments(win)

    walk = code_only.find("env_frame_shards_[ef_i].mu")
    push = code_only.find("reinterpret_cast<const void**>(&fr.pool_)")
    skip = code_only.find("INVALID_VERSION")
    good = fn >= 0 and "Issue #4144" in win and walk >= 0 and push >= 0 and skip >= 0
    report(
        "AC1",
        good,
        "densify-entry walk registers &fr.pool_ lasting slots under env_frame_shards_ shared locks, skipping INVALID_VERSION frames",
    )

    reg = code_only.find("for (void** slot : known_slots)")
    good = fn >= 0 and walk >= 0 and reg >= 0 and walk < reg
    report(
        "AC2",
        good,
        "shard locks release before slot registration (no arena lock under the shard locks)",
    )

    good = (
        "note_post_moving_live_ptr_canary_all(" not in code_only
        and "note_ffi_opaque_alias_densify_cover(" not in code_only
    )
    report(
        "AC3",
        good,
        "slot XOR canary (#3368): pool slots are the cover — no dual-note in the walk",
    )

    good = (
        "arena_group_->register_external_root_slot_for_densify_all(slot)" in code_only
        and "*slot = it->second;" in arena
        and "external_root_slots_for_densify_.clear();" in arena
        and "g_moving_envframe_pool_slots_registered_total" in code_only
        and "g_moving_envframe_pool_slots_registered_total{0}" in hdr
        and "kMovingEnvFramePoolSlotsIssue = 4144" in hdr
        and "moving_envframe_pool_slots_registered_total_v_read()" not in mut
    )
    report(
        "AC4",
        good,
        "#2889 registration SSOT + #3569 window-end consume unchanged; #4144 counter additive-only",
    )

    no_doc = not any((ROOT / "docs" / "design").glob("4144-*")) if (ROOT / "docs" / "design").exists() else True
    good = (
        "ac4144_1_pool_slot_registered_and_remapped" in tst
        and "ac4144_2_no_dual_note_slot_xor" in tst
        and "ac4144_3_source_cite_and_wiring" in tst
        and tst.count("ac4144_1_pool_slot_registered_and_remapped();") == 1
        and tst.count("ac4144_2_no_dual_note_slot_xor();") == 1
        and tst.count("ac4144_3_source_cite_and_wiring();") == 1
        and not (ROOT / "tests" / "core" / "test_issue_4144.cpp").exists()
        and no_doc
        and "check_envframe_pool_slot_remap_4144.py" in build
        and "check_envframe_pool_slot_remap_4144.py" in allow
    )
    report(
        "AC5",
        good,
        "tests cite #4144 and are dispatched; no issue-file/doc; build.py + allowlist wired",
    )

    print(f"check_envframe_pool_slot_remap_4144: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
