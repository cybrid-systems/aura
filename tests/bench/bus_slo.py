#!/usr/bin/env python3
"""
The bus's own SLO (#5395).

The coordinator measures its own cost in CPU time (CLOCK_THREAD_CPUTIME_ID),
never in wall clock: on a shared runner the wall clock measures the
scheduler and not the work (#5324). This bench drives a resident host
through a fixed traffic pattern and checks the counters against the
versioned targets below.

Targets (SLO version 1):
    frame p99        the 99th percentile of the per-frame CPU cost
    publish rate     the records published per CPU second
    catchup size     the largest catchup increment handed out
    attach cost      the CPU time of one handshake
    capacity         the rounds the image can still hold

Usage:
    python3 tests/bench/bus_slo.py            # print the report
    python3 tests/bench/bus_slo.py --check    # exit 1 when a target is missed
    python3 tests/bench/bus_slo.py --json     # machine-readable report
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

# The versioned SLO targets. Bump SLO_VERSION when a target moves.
SLO_VERSION = 1

# The per-frame CPU cost: a control frame is a small fixed-size
# exchange; a millisecond of coordinator CPU per frame is a lot.
TARGET_FRAME_P99_NS = 1_000_000
# The publish rate, in records per CPU second.
TARGET_PUBLISH_RATE = 1_000
# The largest catchup increment the coordinator may hand out.
TARGET_CATCHUP_MAX_BYTES = 1 << 20
# The CPU cost of one handshake.
TARGET_ATTACH_CPU_NS = 100_000_000
# The rounds the image must still be able to hold.
TARGET_CAPACITY_ROUNDS = 8

# The traffic pattern: the peers, the metrics per peer, the catchups.
PEERS = 4
# The image the host serves. A modest one: the attach cost is
# dominated by the mapping, not by the protocol.
IMAGE_BYTES = 64 << 20
METRICS_PER_PEER = 200
CATCHUPS_PER_PEER = 20

HOST_SCRIPT = """
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host {workspace} {image_bytes}))
(define sids (list))
(let loop ((i 0))
  (if (< i {peers})
      (begin
        (define a (bus:attach "" {workspace} 0 127 255))
        (set! sids (cons (car (cdr a)) sids))
        (loop (+ i 1)))))
(define (publish-many sid n)
  (if (> n 0)
      (begin
        (bus:publish sid 9 n)
        (publish-many sid (- n 1)))))
(display "ready")
(newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "traffic")
       (for-each (lambda (sid) (publish-many sid {metrics})) sids)
       (display "traffic-done")
       (newline)
       (loop))
      ((string=? l "catchup")
       (for-each (lambda (sid) (bus:query sid 1 0 0)) sids)
       (display "catchup-done")
       (newline)
       (loop))
      ((string=? l "slo")
       (define s (bus:slo (car sids)))
       (display "slo=") (display s) (display "|") (newline)
       (loop))
      ((string=? l "capacity")
       (define c (bus:capacity-plan (car sids)))
       (display "capacity=") (display c) (display "|") (newline)
       (loop))
      ((string=? l "quit") (bus:unhost {workspace}))
      (else (loop)))))
(bus:unhost {workspace})
(display "HOST-DONE")
(newline)
"""


def _record(text: str, name: str) -> list[int]:
    """Parse one `(ok a b c ...)` record."""
    start = text.find(name + "=")
    if start < 0:
        raise SystemExit(f"bus_slo: the host did not report {name}")
    body = text[start + len(name) + 1 :]
    body = body[: body.find("|")] if "|" in body else body
    body = body.strip()
    if not body.startswith("(ok"):
        raise SystemExit(f"bus_slo: {name} is not an ok record: {body}")
    inner = body[body.find("(") + 1 : body.rfind(")")]
    values = []
    for item in inner.split():
        try:
            values.append(int(item))
        except ValueError:
            values.append(item)
    return values


def run(workspace: int, timeout: float) -> dict:
    with tempfile.TemporaryDirectory(prefix="aura-bus-slo-") as tmp:
        script = Path(tmp) / "host.aura"
        script.write_text(
            HOST_SCRIPT.format(
                workspace=workspace,
                peers=PEERS,
                metrics=METRICS_PER_PEER,
                image_bytes=IMAGE_BYTES,
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
            if "ready" not in _wait_for(proc, "ready", timeout):
                raise SystemExit("bus_slo: the host did not come up")
            proc.stdin.write("traffic\n")
            proc.stdin.flush()
            if "traffic-done" not in _wait_for(proc, "traffic-done", timeout):
                raise SystemExit("bus_slo: the traffic did not finish")
            proc.stdin.write("catchup\n")
            proc.stdin.flush()
            if "catchup-done" not in _wait_for(proc, "catchup-done", timeout):
                raise SystemExit("bus_slo: the catchups did not finish")
            proc.stdin.write("slo\n")
            proc.stdin.flush()
            proc.stdin.write("capacity\n")
            proc.stdin.flush()
            proc.stdin.write("quit\n")
            proc.stdin.flush()
            try:
                out, _ = proc.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate()
            slo = _record(out, "slo")
            capacity = _record(out, "capacity")
        finally:
            if proc.poll() is None:
                proc.kill()
        frames, frame_cpu_ns, p99_cpu_ns = slo[1], slo[2], slo[3]
        publishes, publish_cpu_ns = slo[4], slo[5]
        attaches, attach_cpu_ns = slo[6], slo[7]
        catchups, catchup_max = slo[8], slo[9]
        rounds, low = capacity[1], capacity[2]
        publish_rate = 0.0
        if publish_cpu_ns > 0:
            publish_rate = publishes * 1e9 / publish_cpu_ns
        frame_avg_ns = 0.0
        if frames > 0:
            frame_avg_ns = frame_cpu_ns / frames
        return {
            "slo_version": SLO_VERSION,
            "frames": frames,
            "frame_cpu_ns": frame_cpu_ns,
            "frame_avg_ns": int(frame_avg_ns),
            "frame_p99_ns": p99_cpu_ns,
            "publishes": publishes,
            "publish_cpu_ns": publish_cpu_ns,
            "publish_rate": publish_rate,
            "attaches": attaches,
            "attach_cpu_ns": attach_cpu_ns,
            "catchups": catchups,
            "catchup_max_bytes": catchup_max,
            "capacity_rounds": rounds,
            "capacity_low": low,
        }


def _wait_for(proc: subprocess.Popen, marker: str, timeout: float) -> str:
    """Read until the marker shows up (the host writes line by line)."""
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


def check(report: dict) -> list[str]:
    """The targets the report must meet."""
    problems = []
    if report["frame_p99_ns"] > TARGET_FRAME_P99_NS:
        problems.append(f"frame p99 {report['frame_p99_ns']} ns > {TARGET_FRAME_P99_NS} ns")
    if report["publish_rate"] < TARGET_PUBLISH_RATE:
        problems.append(f"publish rate {report['publish_rate']:.0f}/CPU-s < {TARGET_PUBLISH_RATE}/CPU-s")
    if report["catchup_max_bytes"] > TARGET_CATCHUP_MAX_BYTES:
        problems.append(f"catchup increment {report['catchup_max_bytes']} B > {TARGET_CATCHUP_MAX_BYTES} B")
    if report["attach_cpu_ns"] > TARGET_ATTACH_CPU_NS:
        problems.append(f"attach cost {report['attach_cpu_ns']} ns > {TARGET_ATTACH_CPU_NS} ns")
    if report["capacity_rounds"] < TARGET_CAPACITY_ROUNDS:
        problems.append(f"capacity {report['capacity_rounds']} rounds < {TARGET_CAPACITY_ROUNDS} rounds")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="exit 1 when a target is missed")
    parser.add_argument("--json", action="store_true", help="machine-readable report")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    report = run(os.getpid() % 100000, args.timeout)
    problems = check(report)

    if args.json:
        report["problems"] = problems
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"bus SLO (version {report['slo_version']})")
        print(f"  frames          {report['frames']}")
        print(f"  frame cpu       {report['frame_cpu_ns']} ns total, {report['frame_avg_ns']} ns avg")
        print(f"  frame p99       {report['frame_p99_ns']} ns (target {TARGET_FRAME_P99_NS})")
        print(f"  publishes       {report['publishes']} in {report['publish_cpu_ns']} ns")
        print(f"  publish rate    {report['publish_rate']:.0f} records/CPU-s (target {TARGET_PUBLISH_RATE})")
        print(f"  attaches        {report['attaches']} in {report['attach_cpu_ns']} ns")
        print(f"  catchup max     {report['catchup_max_bytes']} B (target {TARGET_CATCHUP_MAX_BYTES})")
        print(f"  capacity        {report['capacity_rounds']} rounds left (target {TARGET_CAPACITY_ROUNDS})")
        for problem in problems:
            print(f"  MISS: {problem}")
        if not problems:
            print("  all targets met")

    return 1 if (args.check and problems) else 0


if __name__ == "__main__":
    sys.exit(main())
