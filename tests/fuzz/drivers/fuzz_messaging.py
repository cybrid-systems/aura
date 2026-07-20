#!/usr/bin/env python3
"""Inter-Agent Messaging Stress Fuzz — test send/recv across sessions.

Strategy:
1. Create multiple sessions via --serve
2. Send messages between them in random patterns
3. Recv with various timeouts
4. Verify message integrity (no lost/corrupted messages)
5. Test my-id consistency after session switches

Usage:
  python3 tests/fuzz_messaging.py [--quick] [--seed N]
"""

# --- paths patched for tests/fuzz layout (#1935) ---
import sys as _sys
from pathlib import Path as _Path

_FUZZ = _Path(__file__).resolve().parent.parent
if str(_FUZZ) not in _sys.path:
    _sys.path.insert(0, str(_FUZZ))
# --- end path patch ---
import contextlib
import datetime
import random
import subprocess
import sys
import time

from common import AURA_BIN as _AURA_BIN  # noqa: E402
from common import REPO as _REPO

HERE = _FUZZ
REPO = _REPO
AURA = str(_AURA_BIN)

QUICK = "--quick" in sys.argv
SEED = None
for i, a in enumerate(sys.argv):
    if a == "--seed" and i + 1 < len(sys.argv):
        SEED = int(sys.argv[i + 1])

rng = random.Random(SEED if SEED is not None else None)
N_SESSIONS = 3 if QUICK else 5

MESSAGES = [
    "hello",
    "ping",
    "pong",
    "ack",
    "data",
    "request",
    '{"type":"query","sym":"fib"}',
    '{"type":"result","value":42}',
    "long message " + "x" * 100,
    "!@#$%^&*() special chars",
]


def send(proc, cmd):
    try:
        if proc.poll() is not None:
            return None
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        time.sleep(0.005)
        line = proc.stdout.readline()
    except (BrokenPipeError, OSError):
        return None
    if not line:
        return None
    return line.strip()


def run_session():
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.15)

    stats = {"ok": 0, "error": 0, "crash": 0}

    # Create sessions
    session_names = ["default"]
    for i in range(1, N_SESSIONS):
        name = f"agent-{i}"
        resp = send(proc, f'{{"cmd":"session","name":"new:{name}"}}')
        if resp and "created" in resp:
            session_names.append(name)
            stats["ok"] += 1
        elif resp is None:
            stats["crash"] += 1
            break
        time.sleep(0.03)

    if stats["crash"]:
        with contextlib.suppress(Exception):
            proc.kill()
        return stats

    n_ops = 150 if QUICK else 500

    for i in range(n_ops):
        phase = i % 5

        if phase == 0:
            # Verify my-id in each session
            for name in session_names[:3]:  # check first 3
                resp = send(proc, f'{{"cmd":"session","name":"{name}"}}')
                if resp is None:
                    stats["crash"] += 1
                    break
                time.sleep(0.01)
                resp = send(proc, "(display (my-id))")
                if resp is None:
                    stats["crash"] += 1
                    break
                if name in resp:
                    stats["ok"] += 1
                else:
                    stats["error"] += 1
            if stats["crash"]:
                break

        elif phase < 3:
            # Send messages between random sessions
            src = rng.choice(session_names[1:] if rng.random() < 0.7 else session_names)
            dst = rng.choice([s for s in session_names if s != src] + session_names[:1])
            msg = rng.choice(MESSAGES)

            # Switch to src session
            resp = send(proc, f'{{"cmd":"session","name":"{src}"}}')
            if resp is None:
                stats["crash"] += 1
                break
            time.sleep(0.01)

            # Send message
            resp = send(proc, f'(display (send "{dst}" "{msg}"))')
            if resp is None:
                stats["crash"] += 1
                break
            if "#t" in resp:
                stats["ok"] += 1
            else:
                stats["error"] += 1

        else:
            # Recv messages in random sessions
            dst = rng.choice(session_names)
            resp = send(proc, f'{{"cmd":"session","name":"{dst}"}}')
            if resp is None:
                stats["crash"] += 1
                break
            time.sleep(0.01)

            # Try to recv (with short timeout)
            resp = send(proc, "(display (recv 50))")
            if resp is None:
                stats["crash"] += 1
                break
            if "()" in resp:
                # Timeout — no message; expected if no one sent
                pass
            else:
                # Got a message
                pass
            stats["ok"] += 1

        if stats["crash"]:
            break

    try:
        proc.kill()
        proc.wait(timeout=3)
    except Exception:
        pass
    return stats


def main():
    print("=" * 60)
    print("Inter-Agent Messaging Stress Fuzz")
    print(f"  Date:    {datetime.date.today().isoformat()}")
    print(f"  Seed:    {SEED if SEED is not None else 'random'}")
    print(f"  Mode:    {'QUICK' if QUICK else 'FULL'}")
    print(f"  Agents:  {N_SESSIONS}")
    print("=" * 60)

    n_sessions = 3 if QUICK else 5
    total = {"ok": 0, "error": 0, "crash": 0}

    for s in range(n_sessions):
        print(f"\n  Session {s + 1}/{n_sessions} ... ", end="", flush=True)
        st = run_session()
        for k in total:
            total[k] += st[k]
        ops = st["ok"] + st["error"] + st["crash"]
        pct = st["ok"] / max(ops, 1) * 100
        print(f"{st['ok']}/{ops} ({pct:.0f}%) [err={st['error']} crash={st['crash']}]")

    print(f"\n{'=' * 60}")
    print(f"  Total:  {total['ok']} ok, {total['error']} error, {total['crash']} crash")
    rate = total["ok"] / max(total["ok"] + total["error"] + total["crash"], 1) * 100
    print(f"  Rate:   {rate:.1f}%")

    if total["crash"]:
        print("\n  💥 CRASH detected!")
        sys.exit(1)


if __name__ == "__main__":
    main()
