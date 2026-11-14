#!/usr/bin/env python3
"""Real HTTP load for the demo: N clients POST sign-up email lists to /dedupe.

Prints one line per second: requests/s, p50/p99 latency, failures, and
whether the server process is still the same PID (restarts). On SIGTERM
(or after --seconds) prints a before/after summary split at the moment
the server wrote out/keep_at.
"""

import argparse
import json
import os
import random
import signal
import socket
import sys
import threading
import time

NAMES = [
    "alice",
    "bob",
    "carol",
    "dave",
    "erin",
    "frank",
    "grace",
    "heidi",
    "ivan",
    "judy",
    "mallory",
    "niaj",
    "olivia",
    "peggy",
    "rupert",
    "sybil",
    "trent",
    "victor",
    "walter",
    "wendy",
    "yuki",
    "zoe",
    "liam",
    "emma",
    "noah",
    "ava",
    "mia",
    "leo",
    "lucas",
    "maya",
    "omar",
    "nora",
    "ryan",
    "sara",
    "theo",
    "uma",
]
DOMAINS = ["gmail.com", "example.com", "mail.cn", "outlook.com"]


def make_bodies(n, items, seed=7):
    """Sign-up lists with real-looking duplicates (people sign up twice)."""
    rnd = random.Random(seed)
    pool = [f"{a}.{b}@{d}" for a in NAMES for b in NAMES[:6] for d in DOMAINS[:2]]
    bodies = []
    for _ in range(n):
        people = rnd.sample(pool, items // 2)
        lst = [rnd.choice(people) for _ in range(items)]
        bodies.append("\n".join(lst))
    return bodies


def post(host, port, body, timeout):
    data = body.encode()
    req = (
        f"POST /dedupe HTTP/1.1\r\nHost: {host}\r\nContent-Type: text/plain\r\n"
        f"Content-Length: {len(data)}\r\nConnection: close\r\n\r\n"
    ).encode() + data
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(req)
        chunks = []
        while True:
            b = s.recv(65536)
            if not b:
                break
            chunks.append(b)
    resp = b"".join(chunks)
    expected = "\n".join(dict.fromkeys(body.split("\n"))) + "\n"
    return resp.startswith(b"HTTP/1.1 200") and resp.partition(b"\r\n\r\n")[2] == expected.encode()


def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100.0 * len(xs)))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--clients", type=int, default=1)
    ap.add_argument("--items", type=int, default=400)
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--pidfile", default="out/server.pid")
    ap.add_argument("--keepfile", default="out/keep_at")
    ap.add_argument("--redraw", type=int, default=0, help="redraw title+last N rows each second")
    a = ap.parse_args()
    bodies = make_bodies(40, a.items)
    with open("out/request-bodies.json", "w") as f:
        json.dump(bodies, f)
    lock = threading.Lock()
    rows = []  # (t_end, latency_ms, ok)
    stop = threading.Event()

    def client(i):
        k = i
        while not stop.is_set():
            t0 = time.time()
            try:
                ok = post("127.0.0.1", a.port, bodies[k % len(bodies)], 30)
            except OSError:
                ok = False
            t1 = time.time()
            with lock:
                rows.append((t1, (t1 - t0) * 1000.0, ok))
            k += a.clients
            if not ok:
                time.sleep(0.2)

    def pid():
        try:
            with open(a.pidfile) as f:
                p = f.read().strip()
            os.kill(int(p), 0)
            with open(f"/proc/{p}/stat") as f:
                startticks = f.read().split()[21]
            return p + ":" + startticks
        except (OSError, ValueError):
            return "missing"

    with open("out/load.pid", "w") as f:
        f.write(str(os.getpid()))
    sys.stdout.write("\033[?25l")
    pid0 = pid()
    assert pid0 != "missing", "Aura server PID must be tracked, not guessed"
    original_pid = pid0
    restarts = 0

    def on_term(*_):
        stop.set()

    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)
    ths = [threading.Thread(target=client, args=(i,), daemon=True) for i in range(a.clients)]
    for t in ths:
        t.start()
    plural = "s" if a.clients > 1 else ""
    title = f"📊 real HTTP traffic: {a.clients} client{plural}, POST /dedupe, {a.items} emails per request"
    head = f"{'time':>5} {'req/s':>6} {'p50':>8} {'p99':>8} {'failed':>7} {'restarts':>9}"
    hist = []
    base_p50 = None
    start = time.time()
    last = start
    total_fail = 0

    def color(v):
        if base_p50 is None or v <= 0:
            return f"{v:6.0f}ms" if v > 0 else "     —  "
        c = "31" if v >= 0.7 * base_p50 else ("32" if v <= 0.3 * base_p50 else "33")
        return f"\033[{c}m{v:6.0f}ms\033[0m"

    while not stop.is_set() and time.time() - start < a.seconds:
        time.sleep(1.0 - ((time.time() - start) % 1.0))
        now = time.time()
        with lock:
            win = [r for r in rows if last < r[0] <= now]
        last = now
        p = pid()
        if p != pid0:
            restarts += 1
            pid0 = p
        fails = sum(1 for r in win if not r[2])
        total_fail += fails
        lat = [r[1] for r in win if r[2]]
        p50, p99 = pct(lat, 50), pct(lat, 99)
        if base_p50 is None and len(hist) >= 3 and p50 > 0:
            base_p50 = p50
        hist.append(f"{now - start:4.0f}s {len(lat):6d} {color(p50)} {color(p99)} {total_fail:7d} {restarts:9d}")
        if a.redraw:
            sys.stdout.write("\033[H\033[2J" + title + "\n" + head + "\n" + "\n".join(hist[-a.redraw :]) + "\n")
            sys.stdout.flush()
        else:
            print(hist[-1], flush=True)
    stop.set()
    try:
        with open(a.keepfile) as f:
            keep = int(f.read().strip()) / 1000.0
    except (OSError, ValueError):
        keep = None
    with lock:
        allr = list(rows)
    ok_n = sum(1 for r in allr if r[2])
    fail_n = len(allr) - ok_n
    with open("out/load-raw.json", "w") as f:
        json.dump(
            {
                "rows": allr,
                "start": start,
                "end": time.time(),
                "keep": keep,
                "restarts": restarts,
                "server-pid-start": original_pid,
                "server-pid-end": pid0,
            },
            f,
        )
    if a.redraw:
        sys.stdout.write("\033[H\033[2J" + title + "\n")
    print("─" * 60)
    if keep:
        bef = [r for r in allr if r[0] < keep and r[2]]
        aft = [r for r in allr if r[0] > keep + 3 and r[2]]

        def rate(xs):
            if len(xs) < 2:
                return 0.0
            return len(xs) / max(0.001, xs[-1][0] - xs[0][0])

        print(
            f"before the swap: {rate(bef):5.1f} req/s   p50 {pct([r[1] for r in bef], 50):4.0f} ms   p99 {pct([r[1] for r in bef], 99):4.0f} ms"
        )
        print(
            f"after the swap:  {rate(aft):5.1f} req/s   p50 {pct([r[1] for r in aft], 50):4.0f} ms   p99 {pct([r[1] for r in aft], 99):4.0f} ms"
        )
    print(f"\033[1mrequests: {len(allr)}   failed: {fail_n}   server restarts: {restarts}\033[0m\033[?25h", flush=True)


if __name__ == "__main__":
    main()
