#!/usr/bin/env python3
"""Issue #3267: publish_live_env_linear_to_bridge env_frames_ lock + paired C ABI.

publish iterates env_frames_ under shared_lock (or the holding-lock
sibling when compact/truncate already hold unique_lock — mutex is
non-recursive). Combined aura_set/get_aot_live_bridge_state packs
version+lin as one release/acquire word. Existing getters/setters
stay. stamp_closure_bridge_epoch epoch-before-lock is a
closure-creation timestamp. LEGACY_TRUST getenv is read-once.

Contract:
  AC1  publish takes shared_lock; compact/truncate use holding sibling
  AC2  combined C ABI; old setters stay; packed SSOT
  AC3  stamp epoch-before-lock documented as creation timestamp
  AC4  LEGACY_TRUST read-once (not a race)
  AC5  extend test_envframe_epoch_batch; linter after #3266; no invent

Exit 0 = all rows satisfied.

Follow-up #3268: MutationBoundaryGuard flag_ atomic_ref fail-close + region_lock_ move.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    env = _read("src/compiler/evaluator_env.cpp")
    ssot = _read("src/compiler/runtime_ssot.cpp")
    hdr = _read("src/compiler/aura_jit_bridge.h")
    tree = _read("src/compiler/evaluator_workspace_tree.cpp")
    test = _read("tests/compiler/test_envframe_epoch_batch.cpp")
    build = _read("build.py")
    l3266 = _read("scripts/coverage/checks/check_module_realpath_toctou_3266.py")

    pos = env.find("void Evaluator::publish_live_env_linear_to_bridge() const noexcept")
    win = env[pos : pos + 900] if pos >= 0 else ""
    must("Issue #3267", "AC1 cite", win)
    must("std::shared_lock<std::shared_mutex> env_rlock(env_frames_mtx_)", "AC1 shared_lock", win)
    must("publish_live_env_linear_to_bridge_holding_env_lock()", "AC1 holding", win)
    must("non-recursive", "AC1 non-recursive", env)
    tpos = env.find("Evaluator::truncate_env_frames_to_checkpoint()")
    twin = env[tpos : tpos + 5000] if tpos >= 0 else ""
    must("publish_live_env_linear_to_bridge_holding_env_lock()", "AC1 truncate holding", twin)
    if "publish_live_env_linear_to_bridge();" in twin:
        fails.append("AC1: truncate must not nested-lock publish()")
    cpos = env.find("std::size_t Evaluator::compact_env_frames()")
    cwin = env[cpos : cpos + 20000] if cpos >= 0 else ""
    must("publish_live_env_linear_to_bridge_holding_env_lock()", "AC1 compact holding", cwin)
    if env.count("publish_live_env_linear_to_bridge_holding_env_lock()") < 4:
        fails.append("AC1: holding sibling must be defined and used at compact+truncate")
    must("publish_live_env_linear_to_bridge()", "AC1 post-restore shared", tree)
    must("ac3267_1_publish_takes_env_frames_lock", "AC1 test", test)

    must("aura_set_aot_live_bridge_state", "AC2 setter", ssot)
    must("aura_get_aot_live_bridge_state", "AC2 getter", ssot)
    must("g_aot_live_bridge_state", "AC2 packed", ssot)
    must("aura_set_aot_live_env_frame_version", "AC2 old version setter", ssot)
    must("aura_set_aot_live_linear_state_fingerprint", "AC2 old lin setter", ssot)
    must("void aura_set_aot_live_bridge_state", "AC2 header setter", hdr)
    must("void aura_get_aot_live_bridge_state", "AC2 header getter", hdr)
    must("aura_set_aot_live_bridge_state(env_generation_, max_lin)", "AC2 publish packed", env)
    must("ac3267_2_combined_bridge_state", "AC2 test", test)

    spos = env.find("void Evaluator::stamp_closure_bridge_epoch(Closure& cl) const noexcept")
    swin = env[spos : spos + 900] if spos >= 0 else ""
    must("closure-creation timestamp", "AC3 comment", swin)
    epoch = swin.find("cl.bridge_epoch = current_bridge_epoch()")
    lock = swin.find("std::shared_lock<std::shared_mutex> env_rlock(env_frames_mtx_)")
    if epoch < 0 or lock < 0 or epoch > lock:
        fails.append("AC3: epoch read must stay before env lock")
    must("ac3267_3_epoch_before_lock_comment", "AC3 test", test)

    bpos = env.find("bool Evaluator::is_bridge_stale")
    bwin = env[bpos : bpos + 1400] if bpos >= 0 else ""
    must("read once at first is_bridge_stale", "AC4 comment", bwin)
    must("static const bool legacy_trust", "AC4 static cache", bwin)
    must("ac3267_4_legacy_trust_read_once", "AC4 test", test)

    must("ac3267_5_source_and_linter", "AC5 test", test)
    must("check_env_publish_lock_3267", "AC5 build.py", build)
    prev = build.find("check_module_realpath_toctou_3266")
    ours = build.find("check_env_publish_lock_3267")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3266")
    must("3267", "AC5 extend 3266 linter", l3266)
    if (ROOT / "tests" / "issues" / "test_issue_3267.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3267.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3267.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3267.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3267-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3267" in q or "schema-3267" in test:
        fails.append("AC5: new schema-3267 query key (SlimSurface)")

    if fails:
        print("FAIL #3267 env_publish_lock:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3267 env_publish_lock: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
