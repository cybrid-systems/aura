#!/usr/bin/env python3
"""Issue #3394: thread-fiber spawn workers are joinable + drained at ~Evaluator.

Contract:
  AC1 spawn fallback registers the worker in the joinable registry
     (no .detach() on the spawn path)
  AC2 fiber:join joins + erases the worker on successful result fetch
  AC3 ~Evaluator drains this evaluator's fibers before arena teardown
     (before cleanup_orch_agents, #2078 order)
  AC4 drain is bounded + best-effort (stuck body detaches + WARN + counter)
  AC5 unit test AC8 (spawn-abandon dtor stress) wired in test_fiber_spawn_cli
  AC6 docs/stdlib/fiber-spawn.md shutdown/lifetime note cites #3394

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    ctor = _read("src/compiler/evaluator_ctor.cpp")
    _read("src/compiler/messaging_bridge.h")
    doc = _read("docs/stdlib/fiber-spawn.md")
    test = _read("tests/compiler/test_fiber_spawn_cli.cpp")
    build = _read("build.py")

    # AC1: registry + no detach on the spawn path.
    must("#3394" in msg, "AC1: messaging cites #3394")
    must("s_thread_fiber_threads" in msg, "AC1: joinable registry declared")
    must(
        "std::thread(complete_fiber).detach()" not in msg,
        "AC1: spawn path no longer detaches",
    )
    must(
        "s_thread_fiber_threads.emplace(" in msg,
        "AC1: spawn registers worker via emplace",
    )

    # AC2: join joins + erases on successful fetch. Zone = the fiber:join
    # registration up to the next add(...) registration.
    join_start = msg.find('"fiber:join"')
    join_end = msg.find('add("', join_start + 10) if join_start >= 0 else -1
    join_zone = msg[join_start:join_end] if join_start >= 0 and join_end > join_start else ""
    must(
        len(join_zone) > 0,
        "AC2: fiber:join scope found",
    )
    must(
        "s_thread_fiber_threads.find(fid)" in join_zone,
        "AC2: join fetches worker from registry",
    )
    must(
        "s_thread_fiber_threads.erase(it)" in join_zone,
        "AC2: join erases registry entry",
    )
    must(
        "worker.join()" in join_zone,
        "AC2: join joins the worker",
    )

    # AC3: ~Evaluator drains before cleanup_orch_agents.
    dtor_zone = ctor[ctor.find("Evaluator::~Evaluator()") : ctor.find("Evaluator::cleanup_orch_agents()")]
    must(len(dtor_zone) > 0, "AC3: ~Evaluator scope found")
    must(
        "drain_thread_fibers();" in dtor_zone,
        "AC3: ~Evaluator calls drain",
    )
    must(
        dtor_zone.find("drain_thread_fibers();") < dtor_zone.find("cleanup_orch_agents();"),
        "AC3: drain runs before cleanup_orch_agents",
    )
    must(
        "drain_thread_fibers() noexcept" in _read("src/compiler/evaluator.ixx"),
        "AC3: Evaluator declares drain member",
    )

    # AC3b: ~CompilerService drains BEFORE release_children_for_teardown
    # (the service dtor body frees the flat children vectors first — that
    # earlier window is exactly where the #3394 abort lived).
    svc = _read("src/compiler/service.ixx")
    svc_dtor = svc[svc.find("~CompilerService() {") :]
    drain_pos = svc_dtor.find("evaluator_.drain_thread_fibers();")
    release_pos = svc_dtor.find("->release_children_for_teardown();")
    must(drain_pos >= 0, "AC3: ~CompilerService calls drain")
    must(
        0 <= drain_pos < release_pos,
        "AC3: service drain runs before release_children_for_teardown",
    )

    # AC4: drain bounded + best-effort.
    drain_zone = msg[msg.find("void Evaluator::drain_thread_fibers() noexcept {") :]
    must(len(drain_zone) > 0, "AC4: drain definition found")
    must(
        "wait_for(" in drain_zone and "milliseconds(2000)" in drain_zone,
        "AC4: bounded 2s wait",
    )
    must(
        "g_thread_fiber_dtor_abandon_total" in drain_zone,
        "AC4: abandon counter bumped",
    )
    must(
        "[fiber:drain] WARN" in drain_zone,
        "AC4: abandon WARN observability",
    )
    must(
        "worker.detach();" in drain_zone,
        "AC4: stuck body detached (teardown stays bounded)",
    )
    must(
        "catch (const std::system_error&)" in drain_zone and "catch (const std::bad_alloc&)" in drain_zone,
        "AC4: drain never propagates (specific catches, noexcept best-effort)",
    )

    # AC5: test AC8 present + registered.
    must("#3394" in test, "AC5: test cites #3394")
    must("ac8_spawn_abandon_dtor_drain" in test, "AC5: AC8 defined")
    must(
        test.count("ac8_spawn_abandon_dtor_drain()") >= 2,
        "AC5: AC8 defined + registered in run list",
    )
    must(
        'std::thread(complete_fiber).detach()") == std::string::npos' in test,
        "AC5: test asserts no detached spawn",
    )

    # AC6: docs note.
    must("#3394" in doc, "AC6: fiber-spawn.md cites #3394")
    must(
        "drain" in doc.lower(),
        "AC6: doc mentions drain/shutdown",
    )

    # Gate wiring.
    must(
        "check_fiber_spawn_cli_dtor_drain_3394" in build,
        "gate: build.py wires the linter",
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("OK: Issue #3394 thread-fiber dtor drain — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
