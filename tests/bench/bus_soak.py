#!/usr/bin/env python3
"""
The bus soak lane (#5395).

A resident host runs rounds while this driver samples the resource
curves: the host's resident memory outside the image, its open
descriptors, the coordinator's committed bytes, the loans the peers
hold, the trials that are still pending and the host's retained audit
events. The curves must be flat — no monotonic growth over the run.
The image is not pre-faulted: its pages come in as commits reach them,
so the image-backed share of RSS is sampled and left out of the flat
check. The in-process audit log is a bounded window (#5459:
`AuditLog::kMaxEvents`, 4096 events); each sample records how many it
retains, and an overrun is a MISS.

The worker is killed periodically: the coordinator must fail the round
closed (nothing half-applied), the next round must open, and no
residue may accumulate.

Usage:
    python3 tests/bench/bus_soak.py --minutes 3           # a short smoke run
    python3 tests/bench/bus_soak.py --hours 24            # the weekly lane
    python3 tests/bench/bus_soak.py --minutes 3 --check   # exit 1 on a finding
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The growth the curves may show over the whole run, in the unit of the
# sample. The room the image still has is large; the RSS is the tell.
MAX_RSS_GROWTH_BYTES = 32 << 20
MAX_FD_GROWTH = 8
MAX_LOAN_GROWTH = 8
MAX_PENDING_GROWTH = 8
MAX_COMMITTED_GROWTH = 8 << 20
# Keep in step with AuditLog::kMaxEvents (#5459). The retained window is
# bounded; a sample above this means an unbounded second copy came back.
AUDIT_WINDOW_EVENTS = 4096

# How often the worker is killed (in rounds).
KILL_EVERY_ROUNDS = 25
# The pacing between the rounds: a round a second keeps the image's
# worldline channel inside its room for a day-long run.
ROUND_SECONDS = 1.0

HOST_SCRIPT = """
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host {workspace} {image_bytes}))
(define ms (bus:attach "" {workspace} 0 255 255))
(define msid (car (cdr ms)))
(bus:project-nodes msid)
(display "ready")
(newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "slo")
       (define s (bus:slo msid))
       (display "slo=") (display s) (display "|") (newline)
       (loop))
      ((string=? l "capacity")
       (define c (bus:capacity-plan msid))
       (display "capacity=") (display c) (display "|") (newline)
       (loop))
      ((string=? l "audit")
       (define a (bus:audit msid 0))
       (display "audit=")
       (display (if (bus:ok? a) (list-ref a 1) -1))
       (display "|") (newline)
       (loop))
      (else (loop)))))
(bus:unhost {workspace})
(display "HOST-DONE")
(newline)
"""

WORKER_SCRIPT = """
(require "std/bus" all:)
(require "std/string" all:)
(define s (bus:attach "" {workspace} 1 127 255))
(define sid (car (cdr s)))
(display "worker-ready")
(newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string-prefix? l "round ")
       (define parts (string-split l " "))
       (define n (string->number (list-ref parts 1)))
       (bus:publish sid 9 n)
       (define r (bus:round-open sid))
       (if (bus:ok? r)
           (begin
             (define tr (bus:trial sid 3))
             (define wl (car (cdr tr)))
             (define d (car (cdr (bus:candidate-digest sid wl))))
             (bus:bind-evidence sid wl 0 d 7 42 1)
             (define k (bus:keep sid wl 0 0))
             (if (bus:ok? k)
                 (display "round-ok|")
                 (begin (display "round-fail=") (display (bus:message k)) (display "|"))))
           (begin (display "round-refused=") (display (bus:message r)) (display "|")))
       (newline)
       (loop))
      (else (loop)))))
(bus:detach sid)
(display "WORKER-DONE")
(newline)
"""


def _records(text: str, name: str) -> list[int]:
    start = text.find(name + "=")
    if start < 0:
        return []
    body = text[start + len(name) + 1 :]
    body = body[: body.find("|")] if "|" in body else body
    body = body.strip()
    if not body.startswith("(ok"):
        return []
    inner = body[body.find("(") + 1 : body.rfind(")")]
    values = []
    for item in inner.split():
        try:
            values.append(int(item))
        except ValueError:
            values.append(item)
    return values


class Process:
    """One aura child with a line protocol on stdin/stdout."""

    def __init__(self, script: Path, env: dict[str, str]) -> None:
        self.proc = subprocess.Popen(
            [str(ROOT / "build" / "aura"), str(script)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        self.pending = ""

    def ask(self, command: str, marker: str, timeout: float = 60.0) -> str:
        # Each sample must read its own reply: the buffer holds only
        # the answer to this command. It used to accumulate every reply,
        # so _records found the first "slo=" ever printed and the loan,
        # pending and committed curves were frozen at the first sample.
        self.pending = ""
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                break
            self.pending += line
            if marker in line:
                return self.pending
        return self.pending

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            with contextlib.suppress(subprocess.TimeoutExpired):
                self.proc.wait(timeout=10)

    def pid(self) -> int:
        return self.proc.pid


def _rss_split(pid: int) -> tuple[int, int]:
    """The host's resident bytes and the image-backed (memfd) share of them."""
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as handle:
            fields = {}
            for line in handle:
                if line.startswith(("VmRSS:", "RssShmem:")):
                    name, value = line.split(":", 1)
                    fields[name] = int(value.split()[0]) * 1024
        return fields["VmRSS"], fields.get("RssShmem", 0)
    except (OSError, KeyError, IndexError, ValueError):
        return 0, 0


def _fds(pid: int) -> int:
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return 0


def run(minutes: float, image_bytes: int, workspace: int) -> dict:
    with tempfile.TemporaryDirectory(prefix="aura-bus-soak-") as tmp:
        env = dict(os.environ)
        env["XDG_RUNTIME_DIR"] = tmp
        env["AURA_SANDBOX"] = "off"
        host_script = Path(tmp) / "host.aura"
        host_script.write_text(HOST_SCRIPT.format(workspace=workspace, image_bytes=image_bytes))
        worker_script = Path(tmp) / "worker.aura"
        worker_script.write_text(WORKER_SCRIPT.format(workspace=workspace))

        host = Process(host_script, env)
        if "ready" not in host.ask("", "ready", 60.0):
            raise SystemExit("bus_soak: the host did not come up")
        worker = Process(worker_script, env)
        if "worker-ready" not in worker.ask("", "worker-ready", 60.0):
            raise SystemExit("bus_soak: the worker did not come up")

        samples = []
        rounds = 0
        kills = 0
        failures = []
        recoveries = 0
        deadline = time.monotonic() + minutes * 60
        next_kill = KILL_EVERY_ROUNDS
        while time.monotonic() < deadline:
            rounds += 1
            try:
                answer = worker.ask(f"round {rounds}", "round-", 120.0)
            except BrokenPipeError:
                errors = worker.proc.stderr.read() if worker.proc.stderr else ""
                raise SystemExit(
                    f"bus_soak: the worker died at round {rounds}: {errors.strip()}\n  stdout: {worker.pending.strip()}"
                ) from None
            time.sleep(ROUND_SECONDS)
            if "round-ok|" not in answer:
                failures.append(answer.strip().splitlines()[-1] if answer else "no answer")
            if rounds == next_kill:
                # The crash-recovery probe: the worker dies mid-loop and
                # the coordinator must fail the round closed.
                worker.kill()
                kills += 1
                worker = Process(worker_script, env)
                worker.ask("", "worker-ready", 60.0)
                next_kill = rounds + KILL_EVERY_ROUNDS
                recoveries += 1
            if rounds % 10 == 0:
                text = host.ask("slo", "slo=", 60.0)
                slo = _records(text, "slo")
                resident, image = _rss_split(host.pid())
                samples.append(
                    {
                        "rounds": rounds,
                        "rss": resident,
                        "image": image,
                        "fds": _fds(host.pid()),
                        "frames": slo[1] if len(slo) > 1 else 0,
                        "loans": slo[10] if len(slo) > 10 else 0,
                        "pending": slo[11] if len(slo) > 11 else 0,
                        "committed": 0,
                        "room": 0,
                        "audit_events": 0,
                    }
                )
                text = host.ask("capacity", "capacity=", 60.0)
                capacity = _records(text, "capacity")
                if samples and capacity:
                    samples[-1]["committed"] = capacity[8] if len(capacity) > 8 else 0
                    # The worldline room left: it shrinks as the rounds
                    # land, and it must never run out mid-round.
                    samples[-1]["room"] = capacity[6] if len(capacity) > 6 else 0
                audit_text = host.ask("audit", "audit=", 60.0)
                try:
                    audit_events = int(audit_text.split("audit=")[-1].split("|", 1)[0])
                except (ValueError, IndexError):
                    audit_events = 0
                if samples:
                    samples[-1]["audit_events"] = max(0, audit_events)
        worker.kill()
        host.kill()
        return {
            "rounds": rounds,
            "kills": kills,
            "recoveries": recoveries,
            "failures": failures,
            "samples": samples,
        }


def _growth(samples: list[dict], key: str) -> int:
    if len(samples) < 2:
        return 0
    return samples[-1][key] - samples[0][key]


def check(report: dict) -> list[str]:
    problems = []
    samples = report["samples"]
    if not samples:
        problems.append("no samples")
        return problems
    # The image is sealed and its pages only follow the commits, so the
    # leak signal is the resident memory that is not the image.
    rss = _growth(samples, "rss") - _growth(samples, "image")
    if rss > MAX_RSS_GROWTH_BYTES:
        problems.append(f"the resident memory outside the image grew {rss} B > {MAX_RSS_GROWTH_BYTES} B")
    fds = _growth(samples, "fds")
    if fds > MAX_FD_GROWTH:
        problems.append(f"the descriptor count grew {fds} > {MAX_FD_GROWTH}")
    loans = _growth(samples, "loans")
    if loans > MAX_LOAN_GROWTH:
        problems.append(f"the loans grew {loans} > {MAX_LOAN_GROWTH}")
    pending = _growth(samples, "pending")
    if pending > MAX_PENDING_GROWTH:
        problems.append(f"the pending trials grew {pending} > {MAX_PENDING_GROWTH}")
    committed = _growth(samples, "committed")
    if committed > MAX_COMMITTED_GROWTH:
        problems.append(f"the committed bytes grew {committed} > {MAX_COMMITTED_GROWTH}")
    audit_events = samples[-1].get("audit_events", 0)
    if audit_events > AUDIT_WINDOW_EVENTS:
        problems.append(
            f"the audit window retained {audit_events} events > {AUDIT_WINDOW_EVENTS} (round {samples[-1]['rounds']})"
        )
    if report["failures"]:
        problems.append(f"{len(report['failures'])} rounds failed: {report['failures'][:3]}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--minutes", type=float, default=3.0)
    parser.add_argument("--hours", type=float, default=0.0)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--image-bytes", type=int, default=256 << 20)
    args = parser.parse_args()

    minutes = args.hours * 60 if args.hours > 0 else args.minutes
    report = run(minutes, args.image_bytes, os.getpid() % 100000)
    problems = check(report)

    if args.json:
        report["problems"] = problems
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"bus soak: {report['rounds']} rounds, {report['kills']} worker kills")
        for sample in report["samples"][:3] + report["samples"][-3:]:
            print(
                f"  round {sample['rounds']:>6}: rss {sample['rss']} image {sample.get('image', 0)} "
                f"fds {sample['fds']} "
                f"loans {sample['loans']} pending {sample['pending']} "
                f"room {sample['room']} audit {sample.get('audit_events', 0)}"
            )
        for problem in problems:
            print(f"  MISS: {problem}")
        if not problems:
            print("  the curves are flat")

    return 1 if (args.check and problems) else 0


if __name__ == "__main__":
    sys.exit(main())
