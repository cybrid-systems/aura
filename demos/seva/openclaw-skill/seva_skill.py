#!/usr/bin/env python3
"""
demos/seva/openclaw-skill/seva_skill.py — Example OpenClaw
skill that drives the Aura SEVA demo using natural-language
goals.

This file is a *reference implementation* — it shows the
shape of a real OpenClaw skill that calls into Aura's
SEVA primitives. In production:

  - The `AuraServer` connection would use the real
    OpenClaw ↔ Aura transport (MCP / HTTP / gRPC, TBD).
  - The goal parser would call an LLM to extract intent.
  - The mutation driver would consult the strategy
    evolution controller (#444) for "what to mutate
    next".

For the MVP, the goal parser is a simple keyword matcher
and the AuraServer is a shell pipe.

Run with:
  ./build/aura demos/seva/seva_demo.aura &     # start Aura
  python3 demos/seva/openclaw-skill/seva_skill.py "Achieve 95% coverage on FIFO"
"""

from __future__ import annotations

import subprocess
import sys
import time
from dataclasses import dataclass


# ── AuraServer — minimal pipe-based connection ───────────────

@dataclass
class AuraServer:
    """Talks to Aura over stdin/stdout.

    For production: replace with a real transport (MCP /
    HTTP / gRPC) and the AuraServer keeps the same call() API.
    """
    proc: subprocess.Popen

    @classmethod
    def spawn(cls, aura_bin: str = "./build/aura") -> "AuraServer":
        # Aura reads expressions from stdin, prints results.
        # Each top-level expression returns a value; we feed
        # them one at a time.
        proc = subprocess.Popen(
            [aura_bin],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return cls(proc)

    def call(self, expr: str) -> str:
        """Evaluate `expr` in Aura. Returns the printed result."""
        if self.proc.stdin is None or self.proc.stdout is None:
            raise RuntimeError("AuraServer not initialized")
        self.proc.stdin.write(f"(display {expr})\n")
        self.proc.stdin.flush()
        # Aura writes one line per (display ...) call. Read it.
        line = self.proc.stdout.readline()
        return line.rstrip("\n")

    def close(self) -> None:
        if self.proc.stdin:
            self.proc.stdin.close()
        self.proc.wait(timeout=2)


# ── Goal parser ───────────────────────────────────────────────

def parse_goal(goal: str) -> tuple[str, dict]:
    """Parse a natural-language SEVA goal into a (kind, params)
    pair.

    Supports:
      - "Achieve X% coverage on DUT"
      - "Find and fix reset-related bugs"
      - "Generate regression script"
      - "Run verification loop"
    """
    g = goal.lower()
    if "coverage" in g and ("achieve" in g or "reach" in g):
        # Extract the target pct.
        import re
        m = re.search(r"(\d+)\s*%", g)
        target = int(m.group(1)) if m else 95
        # Extract the DUT name (everything after "on").
        m = re.search(r"on\s+([\w\-]+)", g)
        name = m.group(1) if m else "DUT"
        return ("achieve_coverage", {"name": name, "target": target})
    if "reset" in g and ("fix" in g or "find" in g):
        return ("fix_reset_bugs", {})
    if "regression" in g and "generate" in g:
        return ("generate_regression", {})
    if "verification loop" in g or "run" in g:
        return ("verify_loop", {})
    return ("unknown", {"raw": goal})


# ── Goal drivers ──────────────────────────────────────────────

def achieve_coverage(server: AuraServer, name: str, target: int) -> dict:
    """Drive the SEVA loop until coverage target is met."""
    audit_log = []
    for iter_n in range(5):  # bounded iterations
        # 1. Read current state
        gap_hash = server.call(
            f'(hash-ref (seva:achieve-coverage "{name}" {target}) (quote gap))'
        )
        try:
            gap = int(gap_hash)
        except ValueError:
            gap = -1
        audit_log.append(f"  iter {iter_n+1}: gap={gap}")

        # 2. Approve the next mutation (auto-approved in MVP)
        server.call('(seva:approve-mutation 0 "auto")')

        # 3. Apply a coverage-greedy strategy (#444)
        server.call('(strategy:set-strategy "coverage-greedy")')
        server.call('(strategy:report-success "coverage-greedy")')

        if gap == 0:
            audit_log.append(f"  ✓ target {target}% achieved")
            break

        # 4. Read audit log
        audit = server.call('(hash-ref (query:seva-audit-log) (quote mutations-total))')
        audit_log.append(f"  audit: mutations-total={audit}")

    return {
        "kind": "achieve_coverage",
        "achieved": gap == 0,
        "log": audit_log,
    }


def fix_reset_bugs(server: AuraServer) -> dict:
    """Identify reset-related verification holes and target them."""
    # Read the reset-holes count
    reset_holes = server.call(
        '(hash-ref (seva:fix-reset-bugs) (quote reset-holes))'
    )
    return {
        "kind": "fix_reset_bugs",
        "reset_holes": reset_holes,
        "approved": server.call('(seva:approve-mutation 0 "auto")'),
    }


def generate_regression(server: AuraServer) -> dict:
    """Emit a regression script and return it as text."""
    script = server.call('(seva:generate-regression)')
    return {
        "kind": "generate_regression",
        "script": script,
    }


def verify_loop(server: AuraServer) -> dict:
    """Run one pass of the verification loop."""
    audit = server.call('(query:seva-audit-log)')
    readiness = server.call('(hash-ref (query:edsl-readiness) (quote dirty-block-rate))')
    return {
        "kind": "verify_loop",
        "audit": audit,
        "dirty_block_rate": readiness,
    }


# ── Main ──────────────────────────────────────────────────────

def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: seva_skill.py <natural-language goal>")
        print('  Example: seva_skill.py "Achieve 95% coverage on FIFO"')
        return 1
    goal = " ".join(sys.argv[1:])
    print(f"Goal: {goal}")
    kind, params = parse_goal(goal)
    print(f"  parsed kind: {kind}")
    print(f"  params: {params}")

    server = AuraServer.spawn()
    try:
        # Make sure the workspace is loaded.
        server.call('(set-code "(define (f x) (+ x 1))")')
        server.call('(eval-current)')

        if kind == "achieve_coverage":
            result = achieve_coverage(server, **params)
        elif kind == "fix_reset_bugs":
            result = fix_reset_bugs(server)
        elif kind == "generate_regression":
            result = generate_regression(server)
        elif kind == "verify_loop":
            result = verify_loop(server)
        else:
            print(f"  unknown goal kind: {kind}")
            return 1

        print("\nResult:")
        for k, v in result.items():
            print(f"  {k}: {v}")
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())