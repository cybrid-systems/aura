#!/usr/bin/env python3
"""Issue #5476: lock order and the shared set-code error state.

Two fixes in this batch are structural:

- bind_define_value_in_env_at used to hold cells_storage_mutex across
  env.bind; bind seals a workspace value, whose hash branch takes
  hash_tables_mtx_, while release_unreachable_hash_tables takes the two
  in the opposite order (ABBA).
- the pending set-code/load parse error was two unguarded strings moved
  by eval-current on another fiber.

The source rows pin both shapes; the runtime row runs the #4709 stress
program so the bind/seal path is exercised under four rebind fibers.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AURA_BIN = ROOT / "build" / "aura"

PROGRAM = r"""(set-code "(define (pol-0 x) (+ x 0))\n(define (pol-1 x) (+ x 1))\n(define (pol-2 x) (+ x 2))\n(define (pol-3 x) (+ x 3))")
(eval-current)
(define stats (make-hash))
(hash-set! stats "join-void" 0)
(define (worker w)
  (let loop ((i 0))
    (if (< i 30)
        (begin
          (hash-set! stats (string-append "join-void") (+ 1 (hash-ref stats "join-void" 0)))
          (mutate :rebind (string-append "pol-" (number->string w)) "(lambda (x) (+ x 1))" "p")
          (eval-current)
          (loop (+ i 1)))
        w)))
(define fs (list (fiber:spawn (lambda () (worker 0)))
                 (fiber:spawn (lambda () (worker 1)))
                 (fiber:spawn (lambda () (worker 2)))
                 (fiber:spawn (lambda () (worker 3)))))
(map fiber:join fs)
(list (map string-length (hash-keys stats)) (hash-ref stats "join-void" -1))
"""


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


class Concurrency5476(unittest.TestCase):
    def test_lock_order_and_set_code_error_state(self) -> None:
        service = read("src/compiler/service.ixx")
        pos = service.find("std::size_t bind_define_value_in_env_at(")
        self.assertGreaterEqual(pos, 0, "bind_define_value_in_env_at present")
        win = service[pos : pos + 2600]
        self.assertIn("if (fresh)", win, "the cell lock is scoped before env.bind")
        self.assertIn("env.bind(name, types::make_cell(ci));", win)
        self.assertNotIn(
            "cells().push_back(value);\n        env.bind(",
            win,
            "env.bind must not run while the cells lock is held (#5476)",
        )

        prim = read("src/compiler/evaluator_primitives_eval.cpp")
        self.assertIn("take_set_code_error", prim)
        self.assertNotIn("last_set_code_error_kind_", prim, "set-code kind must go through the accessor")
        self.assertNotIn("last_set_code_error_msg_", prim, "set-code message must go through the accessor")
        ast = read("src/compiler/evaluator_primitives_ast.cpp")
        self.assertIn("clear_set_code_error", ast)
        self.assertNotIn("last_set_code_error_kind_", ast)

        flat = read("src/compiler/evaluator_eval_flat.cpp")
        self.assertIn("Evaluator::take_set_code_error()", flat)
        self.assertIn("Evaluator::set_set_code_error(", flat)

        tc = read("src/compiler/type_checker.ixx")
        self.assertIn("std::atomic<bool> last_linear_synth_hard_fail_", tc)
        self.assertIn("std::atomic<std::uint64_t> last_uncovered_bidirectional_tag_count_", tc)

        boundary = read("src/compiler/evaluator_mutation_boundary.cpp")
        snap = boundary.find("mark_dirty_upward_call_count()")
        self.assertGreaterEqual(snap, 0)
        before = boundary[max(0, snap - 900) : snap]
        self.assertIn(
            "lock_workspace_shared();",
            before,
            "the boundary-enter dirty snapshot must hold a workspace shared pin",
        )
        after = boundary[snap : snap + 900]
        self.assertIn("unlock_workspace_shared();", after)

        service_ixx = read("src/compiler/service.ixx")
        run_pos = service_ixx.find("void run_() {")
        self.assertGreaterEqual(run_pos, 0)
        self.assertIn(
            "stats = stats_;",
            service_ixx[run_pos : run_pos + 1200],
            "the tier worker must snapshot stats_ under the queue lock",
        )

        ast = read("src/core/ast.ixx")
        self.assertIn("core/atomic_column.hh", ast)
        self.assertIn("cow::AtomicTrackedColumn<std::uint16_t> node_gen_;", ast)
        atomic = read("src/core/atomic_column.hh")
        self.assertIn("std::deque<std::atomic<T>> mirror_;", atomic)
        self.assertIn("memory_order_acquire", atomic)

        mem = read("src/compiler/evaluator_primitives_memory.cpp")
        self.assertIn("heap_sizes_snapshot", mem)
        self.assertIn("sizes.strings", mem)

        ws = read("src/compiler/evaluator_workspace_tree.cpp")
        ck = ws.find("bool Evaluator::save_panic_checkpoint()")
        self.assertGreaterEqual(ck, 0)
        ck_win = ws[ck : ck + 2500]
        lock_at = ck_win.find("std::scoped_lock alloc_lock(alloc_storage_lock_)")
        size_at = ck_win.find("panic_safe_cells_size_ = cells_.size()")
        self.assertGreaterEqual(lock_at, 0)
        self.assertGreater(
            size_at,
            lock_at,
            "panic-checkpoint size snapshots must sit under the alloc lock",
        )

        arena = read("src/core/arena.ixx")
        cpos = arena.find("LiveCompactResult live_compact(LiveCompactMode mode) noexcept {")
        self.assertGreaterEqual(cpos, 0)
        self.assertIn(
            "std::unique_lock<std::shared_mutex> lock(arenas_mtx_)",
            arena[cpos : cpos + 900],
            "group live_compact must exclude stats readers",
        )

        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_TIER_JIT"] = "0"
        env["AURA_PIPELINE_STRICT"] = "0"
        env["AURA_SANDBOX"] = "off"
        with tempfile.TemporaryDirectory(prefix="aura-5476-") as td:
            script = Path(td) / "stress.aura"
            script.write_text(PROGRAM)
            for _attempt in range(2):
                proc = subprocess.run(
                    [str(AURA_BIN), str(script)],
                    cwd=td,
                    capture_output=True,
                    text=True,
                    timeout=90,
                    env=env,
                )
                self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
                self.assertNotIn("unbound variable", proc.stderr, proc.stderr[-800:])
                self.assertNotIn("AURA CRASH", proc.stderr, proc.stderr[-800:])
                rows = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip().startswith("((9) ")]
                self.assertTrue(rows, proc.stdout[-800:])
                value = int(rows[-1][5:-1])
                self.assertGreaterEqual(value, 1, rows[-1])
                self.assertLessEqual(value, 120, rows[-1])


if __name__ == "__main__":
    unittest.main()
