#!/usr/bin/env python3
"""
The bus under history (#5460).

A resident host accumulates a long task board and a long metrics channel,
then the lane re-measures the per-frame path and the attach path:

    task cycle   post + claim + complete, microseconds per task
    task status  bus:task-status on a board with 20k finished tasks
    attach       bus:attach + detach after each 100k committed records

The target is flatness: the late cycle cost must not scale with the
number of finished tasks, and an attach must not rehash/re-walk the whole
committed image every time. The floors are generous because the runner
is shared; the ratios are the regression signal.

Usage:
    python3 tests/bench/bus_history.py
    python3 tests/bench/bus_history.py --check
    python3 tests/bench/bus_history.py --json
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The per-task cycle (three control frames) after the board holds 20k
# finished tasks. The issue's pre-fix number was ~1.7 ms at 60k; a
# bounded board should stay in the tens of microseconds.
TARGET_TASK_CYCLE_US = 500
# bus:task-status must not copy the board; one read should be cheap.
TARGET_TASK_STATUS_US = 100
# An attach after another 100k records should cost about the same as
# the previous one (only the appended tail is new). The issue's curve
# grew linearly with the whole committed image.
TARGET_ATTACH_GROWTH = 2.5
TARGET_ATTACH_AVG_MS = 50.0

IMAGE_BYTES = 64 << 20
PUBLISH_COHORTS = 5
PUBLISHES_PER_COHORT = 50_000
ATTACHES_PER_SAMPLE = 10

HOST_SCRIPT = """
(require "std/bus" all:)
(define port 8995)
(define (bench-ms n fn)
  (let ((t0 (monotonic-ms)))
    (let loop ((i 0)) (if (< i n) (begin (fn) (loop (+ i 1))) #f))
    (- (monotonic-ms) t0)))
(define h (bus:host port {image_bytes}))
(define s (car (cdr (bus:attach "" port 0 255 255))))
(define last 0)
(define (cycle)
  (define p (bus:post-task s 0 1))
  (define id (car (cdr p)))
  (bus:claim s 5000 id)
  (bus:complete s id 1)
  (set! last id))
(define (seed n)
  (let loop ((i 0)) (if (< i n) (begin (cycle) (loop (+ i 1))) #f)))
(define (publish-many n)
  (let loop ((i 0) (ok 0))
    (if (< i n)
        (let ((r (bus:publish s 9 i)))
          (loop (+ i 1) (+ ok (if (bus:ok? r) 1 0))))
        ok)))
(define (attach-sample n)
  (let ((t0 (monotonic-ms)))
    (let loop ((i 0))
      (if (< i n)
          (begin
            (define a (bus:attach "" port 0 255 255))
            (bus:detach (car (cdr a)))
            (loop (+ i 1)))
          #f))
    (- (monotonic-ms) t0)))
(display "ready")
(newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "tasks")
       (define early (bench-ms 1000 cycle))
       (seed 4000)
       (define mid (bench-ms 1000 cycle))
       (seed 14000)
       (define late (bench-ms 1000 cycle))
       (define st (bench-ms 2000 (lambda () (bus:task-status s last))))
       (display "task=") (display early) (display ",") (display mid)
       (display ",") (display late) (display ",") (display st) (display "|") (newline)
       (loop))
      ((string=? l "publish")
       (define empty (attach-sample {attaches}))
       (display "attach=") (display empty) (display ",0") (display "|") (newline)
       (let cohorts ((k 0) (total 0))
         (if (< k {cohorts})
             (begin
               (define published (publish-many {publishes}))
               (define ms (attach-sample {attaches}))
               (display "attach=") (display ms) (display ",")
               (display (+ total published)) (display "|") (newline)
               (cohorts (+ k 1) (+ total published)))
             (begin
               (display "published=") (display total) (display "|") (newline)
               (loop))))
      ((string=? l "quit") (bus:unhost port))
      (else (loop)))))
(bus:unhost port)
(display "HOST-DONE")
(newline)
"""


def _record(text: str, name: str) -> list[int]:
    start = text.find(name + "=")
    if start < 0:
        raise SystemExit(f"bus_history: the host did not report {name}")
    body = text[start + len(name) + 1 :]
    body = body[: body.find("|")] if "|" in body else body
    values = []
    for item in body.strip().split(","):
        values.append(int(item))
    return values


def _wait_for(proc: subprocess.Popen, marker: str, timeout: float) -> str:
    seen = ""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        seen += line
        if marker in line:
            return seen
    return seen


def run(workspace: int, timeout: float) -> dict:
    with tempfile.TemporaryDirectory(prefix="aura-bus-history-") as tmp:
        script = Path(tmp) / "host.aura"
        script.write_text(
            HOST_SCRIPT.format(
                image_bytes=IMAGE_BYTES,
                attaches=ATTACHES_PER_SAMPLE,
                cohorts=PUBLISH_COHORTS,
                publishes=PUBLISHES_PER_COHORT,
            )
        )
        env = dict(os.environ)
        env["XDG_RUNTIME_DIR"] = tmp
        env["AURA_SANDBOX"] = "off"
        proc = subprocess.Popen(
            [str(ROOT / "build" / "aura"), str(script)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )
        try:
            startup = _wait_for(proc, "ready", timeout)
            if "ready" not in startup:
                raise SystemExit(f"bus_history: the host did not come up: {startup.strip()}")
            proc.stdin.write("tasks\n")
            proc.stdin.flush()
            task_sample = _record(_wait_for(proc, "task=", timeout), "task")
            proc.stdin.write("publish\n")
            proc.stdin.flush()
            attaches = []
            published_total = 0
            for _ in range(PUBLISH_COHORTS + 1):
                sample = _record(_wait_for(proc, "attach=", timeout), "attach")
                attaches.append(sample[0])
                published_total = sample[1]
            published = _record(_wait_for(proc, "published=", timeout), "published")[0]
            if published != published_total:
                raise SystemExit("bus_history: the publish count moved during the run")
            proc.stdin.write("quit\n")
            proc.stdin.flush()
            try:
                out, _ = proc.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate()
            if "HOST-DONE" not in out:
                raise SystemExit("bus_history: the host did not shut down cleanly")
        finally:
            if proc.poll() is None:
                proc.kill()
        early, mid, late, status_ms = task_sample
        attach_avg_ms = [ms / ATTACHES_PER_SAMPLE for ms in attaches]
        growth = attach_avg_ms[-1] / attach_avg_ms[0] if attach_avg_ms[0] > 0 else 0.0
        return {
            "task_cycle_us": [early * 1000 / 1000, mid * 1000 / 1000, late * 1000 / 1000],
            "task_cycle_growth": late / early if early > 0 else 0.0,
            "task_status_us": status_ms * 1000 / 2000,
            "attach_avg_ms": attach_avg_ms,
            "attach_growth": growth,
            "finished_tasks": 21000,
            "published_records": published,
        }


def check(report: dict) -> list[str]:
    problems = []
    if report["task_cycle_us"][2] > TARGET_TASK_CYCLE_US:
        problems.append(f"late task cycle {report['task_cycle_us'][2]:.0f} us > {TARGET_TASK_CYCLE_US} us")
    if report["task_cycle_growth"] > 4.0:
        problems.append(f"task cycle grew {report['task_cycle_growth'] * 100:.0f}% over the board > 400%")
    if report["task_status_us"] > TARGET_TASK_STATUS_US:
        problems.append(f"task-status {report['task_status_us']:.1f} us > {TARGET_TASK_STATUS_US} us")
    if report["published_records"] < PUBLISH_COHORTS * PUBLISHES_PER_COHORT:
        problems.append(
            f"only {report['published_records']} records committed < {PUBLISH_COHORTS * PUBLISHES_PER_COHORT}"
        )
    if report["attach_avg_ms"][-1] > TARGET_ATTACH_AVG_MS:
        problems.append(f"attach {report['attach_avg_ms'][-1]:.1f} ms > {TARGET_ATTACH_AVG_MS} ms")
    if report["attach_growth"] > TARGET_ATTACH_GROWTH:
        problems.append(f"attach grew {report['attach_growth']:.1f}x over history > {TARGET_ATTACH_GROWTH}x")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="exit 1 when a target is missed")
    parser.add_argument("--json", action="store_true", help="machine-readable report")
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()

    report = run(os.getpid() % 100000, args.timeout)
    problems = check(report)

    if args.json:
        report["problems"] = problems
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print("bus history (#5460)")
        print(
            "  task cycle      "
            f"{report['task_cycle_us'][0]:.0f} -> {report['task_cycle_us'][1]:.0f} -> "
            f"{report['task_cycle_us'][2]:.0f} us/task "
            f"(growth {report['task_cycle_growth']:.2f}x)"
        )
        print(f"  task status     {report['task_status_us']:.1f} us")
        print(
            "  attach          "
            + ", ".join(f"{ms:.1f}" for ms in report["attach_avg_ms"])
            + f" ms (growth {report['attach_growth']:.2f}x)"
        )
        print(
            f"  history         {report['finished_tasks']} finished tasks, "
            f"{report['published_records']} committed records"
        )
        for problem in problems:
            print(f"  MISS: {problem}")
        if not problems:
            print("  all targets met")

    return 1 if (args.check and problems) else 0


if __name__ == "__main__":
    sys.exit(main())
