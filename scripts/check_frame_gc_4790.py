#!/usr/bin/env python3
"""Issue #4790 source-cite gate: three pre-existing frame/GC items that
#4787 (the capture-snapshot cache is not a GC root) exposed by making
EnvFrame slot REUSE hot.

Mechanism (the issue):
  #4787 removed the over-rooting that kept every dead capture frame alive,
  so the #4459 freelist finally recycled slots. That turned three latent
  problems from theoretical into hot paths:

  1. The #4459 reuse path rewrote a PUBLISHED EnvFrame slot without taking
     that slot's EnvFramesShard lock, although the allocation site's own
     comment says it must. Stop-the-world hid it; any park-during-sweep or
     future concurrent allocation makes it a data race on bindings_ /
     bindings_symid_ / parent_id / version_ against every shard-locked
     reader (lookup, materialize_call_env, typecheck/metrics iteration, the
     GC mark).
  2. Frame-slot exhaustion degraded a capture SILENTLY: at kMaxSlots
     (1,048,576) EnvFrameStore::push_back returned NULL_ENV_ID,
     alloc_env_frame propagated it, and every capture caller built a
     closure with NO capture. The free variable then read unbound hundreds
     of calls away from the cause ("unbound variable: cap", exactly how
     #4787 presented).
  3. reclaim_file_mode_unreachable rooted every frame plus the whole
     capture-snapshot cache, so the file-mode path reclaimed no frame by
     construction.

Landed shape:
  AC1 (#4790 item 1) evaluator_env.cpp: the reuse branch takes
      env_frame_shards_[env_frame_shard_index(id)].mu exclusive BEFORE it
      touches EnvFrame& fr, and drops the #4448 snapshot after the
      restamp. The lock order is alloc -> shard; the sweep takes all
      sixteen shards and never takes env_frame_alloc_mtx_, and
      alloc_env_frame is its only user, so the order cannot cycle. The
      runtime door is run_4790_reuse_takes_shard_lock: one thread drives
      the named-let capture + reclaim loop (which reuses slots) while a
      second thread iterates every published frame through
      env_frame_for_test (the shard-locked reader). TSan reports the
      unlocked restamp before the fix and is clean after.
  AC2 (#4790 item 2) alloc_env_frame never hands a capture caller
      NULL_ENV_ID because the arena is full: the #4459 freelist is
      consulted first, then env_frame_arena::fail() names the arena and
      terminates. env_frame_arena.hh carries the typed Exhausted failure
      and the g_throw_on_exhaustion test switch, so
      run_4790_env_frame_exhaustion_is_loud drives a real Evaluator to
      EnvFrameStore::kMaxSlots and asserts the raise instead of aborting
      the runner. A capture at the cap raises; it never returns a
      silently captureless frame.
  AC3 (#4790 item 3) evaluator_gc.cpp: reclaim_file_mode_unreachable keeps
      every frame a root DELIBERATELY, with the reason written at the
      site: the file-mode sweep runs at an IR instruction boundary that
      can be nested inside a live tree-walked evaluation (#4481/#4617),
      whose Env stack is C++ locals on the fiber stack; the cross-direction
      root hand-off carries values, never EnvIds, so freelisting a frame
      whose id is still held (parent_id_ / cap_snapshot source_frame) would
      let the next capture restamp it under the holder. The
      capture-snapshot cache, by contrast, is NOT a root here: a snapshot
      is an exact copy of its source frame's bindings, every frame is
      rooted, and this path never clears a frame, so the #4608 cache root
      is subsumed (it only made every file-mode mark walk the whole cache).
      run_4790_file_mode_reclaim_is_conservative pins both halves: the
      file-mode sweep recycles no frame, and the tree-walker sweep on the
      same Evaluator still does (#4787 row stays green).
  AC4 Wiring: tests/compiler/test_envframe_epoch_batch.cpp defines and
      registers the three run_4790_* doors; build.py registers this
      linter; scripts/coverage/root_check_allowlist.txt lists it; no
      docs/design/4790-* (per #1655).

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENV = ROOT / "src" / "compiler" / "evaluator_env.cpp"
GC = ROOT / "src" / "compiler" / "evaluator_gc.cpp"
ARENA = ROOT / "src" / "compiler" / "env_frame_arena.hh"
TST = ROOT / "tests" / "compiler" / "test_envframe_epoch_batch.cpp"
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
    env = ENV.read_text()
    gc = GC.read_text()
    arena = ARENA.read_text() if ARENA.exists() else ""
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # AC1: the reuse path holds the slot's shard lock.
    reuse = _between(
        env,
        "// Issue #4459: reuse a frame the tree-walker sweep cleared.",
        "// Issue #1384: construct the frame locally",
    )
    must(bool(reuse), "AC1: the #4459 reuse branch is locatable")
    must("Issue #4790" in reuse, "AC1: the reuse branch cites #4790")
    must(
        "std::unique_lock<std::shared_mutex> wlock(" in reuse
        and "env_frame_shards_[env_frame_shard_index(id)].mu" in reuse,
        "AC1: the reuse restamp takes that slot's EnvFramesShard exclusive lock",
    )
    i_lock = reuse.find("env_frame_shard_index(id)")
    i_frame = reuse.find("EnvFrame& fr = env_frames_[id];")
    i_clear = reuse.find("fr.bindings_.clear();")
    must(
        0 <= i_lock < i_frame < i_clear,
        "AC1: the lock is taken BEFORE the published slot is rewritten",
    )
    must(
        "cap_snap_cache_.erase(id)" in reuse,
        "AC1: the reused id still drops its #4448 capture snapshot",
    )
    must(
        "Issue #4790"
        in _between(env, "Issue #4717 step 2: allocation fast path", "std::lock_guard<std::mutex> alloc_lock"),
        "AC1: the allocation-site comment records why reuse takes the lock",
    )

    # AC2: exhaustion is loud at the allocation site.
    alloc = _between(
        env,
        "aura::compiler::EnvId Evaluator::alloc_env_frame(",
        "// Evaluator::alloc_env_frame_from_env",
    )
    must(bool(alloc), "AC2: alloc_env_frame is locatable")
    must("env_frame_arena.hh" in env, "AC2: evaluator_env.cpp includes the arena contract header")
    must("env_frame_arena::fail(" in alloc, "AC2: the allocation site calls the loud exit")
    must(
        alloc.count("env_frame_arena::fail(") >= 2,
        "AC2: both exhaustion exits (4G guard + store refusal) are loud",
    )
    push_tail = _between(
        alloc,
        "const auto new_id = env_frames_.push_back",
        "const EnvId id = static_cast<EnvId>(new_id);",
    )
    must("return NULL_ENV_ID" not in push_tail, "AC2: the store refusal no longer propagates a silent NULL")
    must("struct Exhausted" in arena, "AC2: the typed Exhausted failure exists")
    must("[[noreturn]] inline void fail(" in arena, "AC2: the fatal exit is [[noreturn]]")
    must("g_throw_on_exhaustion" in arena, "AC2: the test switch exists")
    must(
        "kMaxSlots" in arena and "EnvFrame arena exhausted" in arena,
        "AC2: the message names the arena and the cap",
    )

    # AC3: file-mode frame conservatism is documented; the cache is not a root.
    fm = _between(
        gc,
        "bool Evaluator::reclaim_file_mode_unreachable(",
        "bool Evaluator::reclaim_tw_unreachable(",
    )
    must(bool(fm), "AC3: reclaim_file_mode_unreachable is locatable")
    must("Issue #4790" in fm, "AC3: the conservatism cites #4790")
    must("every frame is a root" in fm, "AC3: the root rule is stated")
    must("#4481" in fm and "#4617" in fm, "AC3: the missing EnvId root set is cited")
    must(
        "cap_snap_cache_" not in fm,
        "AC3: the capture-snapshot cache is not a file-mode root (subsumed, #4787)",
    )

    # AC4: the runtime doors.
    for name, blurb in (
        ("run_4790_reuse_takes_shard_lock", "the two-body capture+reclaim loop"),
        ("run_4790_env_frame_exhaustion_is_loud", "the kMaxSlots exhaustion door"),
        ("run_4790_file_mode_reclaim_is_conservative", "the file-mode contract door"),
    ):
        must(f"static void {name}(" in tst, f"AC4: {blurb} is defined")
        must(f"{name}();" in tst, f"AC4: {blurb} is registered in main")
    must("EnvFrameStore::kMaxSlots" in tst, "AC4: the exhaustion door drives to the real cap")
    must("std::thread" in tst, "AC4: the reuse door uses a second body")
    must("env_frame_arena::Exhausted" in tst, "AC4: the door asserts the typed failure")
    must(
        "reclaim_file_mode_unreachable" in tst and "reclaim_tw_unreachable" in tst,
        "AC4: the file-mode door compares both reclaim paths",
    )

    # AC5: wiring + no docs/design/4790-*.
    must("check_frame_gc_4790.py" in build, "AC5: build.py registers this linter")
    must("check_frame_gc_4790.py" in allow, "AC5: root_check_allowlist.txt lists this linter")
    must(
        not glob.glob(str(ROOT / "docs" / "design" / "4790-*")),
        "AC5: no docs/design/4790-* (per #1655)",
    )

    if failures:
        print(f"\ncheck_frame_gc_4790: {len(failures)} row(s) failed")
        return 1
    print("\ncheck_frame_gc_4790: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
