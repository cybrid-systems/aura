#!/usr/bin/env python3
"""tests/run_serve.py — Run bench.aura via --serve-async with session:create.

One Aura process, N worker sessions (each with isolated evaluator).
Uses session:create + send/recv for communication.

Usage:
  LLM_API_KEY="***" python3 tests/run_serve.py [workers]
"""
import json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURA = os.environ.get("AURA", str(ROOT / "build" / "aura"))
WORKERS = int(sys.argv[1]) if len(sys.argv) > 1 else 4
TIMEOUT = 600

print(f"=== Serve-async bench ({WORKERS} workers) ===")

proc = subprocess.Popen(
    [AURA, "--serve-async"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True, bufsize=1,
)

def exec_aura(code, session="default"):
    proc.stdin.write(json.dumps({"cmd": "exec", "code": code, "session": session}) + "\n")
    proc.stdin.flush()
    # Read until we get a JSON response
    while True:
        line = proc.stdout.readline()
        if not line: break
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except: pass

# Create workers
for w in range(1, WORKERS + 1):
    r = exec_aura("", f"w{w}")
    exec_aura(f'(session:create "w{w}")')
print(f"Created {WORKERS} worker sessions")

# Load bench.aura in default session
exec_aura('(require "std/bench" all:)')

# Run a test task
r = exec_aura('(display "serve mode ready")(newline)')
print(f"Ready: {r}")

proc.terminate()
proc.wait()
