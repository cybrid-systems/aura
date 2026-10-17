"""CPython twins of the class-1 checksums.

Reported beside the Aura column. Not a pass/fail gate. LuaJIT is not
invoked from here; bench/run.py skips that column when the interpreter
is absent.
"""

from __future__ import annotations


def fib(n: int = 18) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def int_loop(n: int = 6000) -> int:
    total = 0
    i = 0
    while i <= n:
        total += i
        i += 1
    return total


def float_loop(n: int = 6000) -> float:
    total = 0.0
    i = 0
    while i < n:
        total += 1.5
        i += 1
    return total


def cons_throughput(n: int = 4000) -> int:
    return n


def hash_lookup(n: int = 1500) -> int:
    return (n - 1) * n // 2


def string_build(n: int = 2000) -> int:
    return n


def quot(a: int, b: int) -> int:
    """Truncating division, matching Aura quotient for positive b."""
    return int(a / b)


def nbody(steps: int = 40, scale: int = 1000) -> int:
    n = 4
    x = [0, 40, 0, -30]
    y = [0, 0, 40, 10]
    vx = [0, 1, -1, 0]
    vy = [0, 0, 1, -1]
    for _ in range(steps):
        ax = [0] * n
        ay = [0] * n
        for i in range(n):
            for j in range(n):
                if i == j:
                    continue
                dx = x[j] - x[i]
                dy = y[j] - y[i]
                dist2 = dx * dx + dy * dy + 1
                ax[i] += quot(dx * scale, dist2)
                ay[i] += quot(dy * scale, dist2)
        for i in range(n):
            vx[i] += ax[i]
            vy[i] += ay[i]
            x[i] += vx[i]
            y[i] += vy[i]
    return sum(v * v for v in x) + sum(v * v for v in y)


def fannkuch(n: int = 5) -> int:
    perm1 = list(range(n))
    count = [0] * n
    r = n
    maxf = 0
    while True:
        while r != 1:
            count[r - 1] = r
            r -= 1
        perm = perm1[:]
        flips = 0
        k = perm[0]
        while k:
            a, b = 0, k
            while a < b:
                perm[a], perm[b] = perm[b], perm[a]
                a += 1
                b -= 1
            flips += 1
            k = perm[0]
        if flips > maxf:
            maxf = flips
        while True:
            if r == n:
                return maxf
            first = perm1[0]
            perm1[0:r] = perm1[1 : r + 1]
            perm1[r] = first
            count[r] -= 1
            if count[r] > 0:
                break
            r += 1


def evo_kv(n: int = 500) -> int:
    total = 0
    z = 0
    for i in range(n):
        total += i * 3
    for k in range(100, 150):
        z += k * 3
    return total + z


def control_plane(n: int = 400) -> int:
    return (n - 1) * n // 2


def multi_agent() -> int:
    one = 299 * 300 // 2
    return 8 * one + (7 * 8 // 2)


def fibers(spin: int = 500) -> int:
    one = spin * (spin + 1) // 2
    return 10 * 100_000_000 + 8 * one


REFS = {
    "fib": fib,
    "int-loop": int_loop,
    "float-loop": float_loop,
    "cons-throughput": cons_throughput,
    "hash-lookup": hash_lookup,
    "string-build": string_build,
    "nbody": nbody,
    "fannkuch": fannkuch,
    "evo-kv": evo_kv,
    "control-plane": control_plane,
    "multi-agent": multi_agent,
    "agent-fibers-8": fibers,
}
