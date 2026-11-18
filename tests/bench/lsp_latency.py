#!/usr/bin/env python3
"""LSP request latency over a synthetic file (#5351 / #5352).

Every data point is one fresh `aura lsp` process: initialize, didOpen, wait
for the first publishDiagnostics, then time one request and close.  The
reported number is the median over --runs processes, which is what the issue
asks for; a rebuild in the same process would hide the cold path.

    python3 tests/bench/lsp_latency.py            # print the table
    python3 tests/bench/lsp_latency.py --check    # fail on the issue targets

Targets (issue #5351/#5352): definition/hover <= 20 ms, completion <= 100 ms,
references <= 100 ms, documentSymbol <= 60 ms, foldingRange <= 30 ms at 20002
lines; every 4x-line step <= 5x for all of them.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python" / "process"))

from lsp_protocol import Client  # noqa: E402

SIZES = (312, 1250, 5000)  # 1250 / 5002 / 20002 lines
# Which issue owns each row's absolute target. The weekly CI lane runs
# --issue 5351: its four requests keep their absolute limits, while the
# #5352 rows (documentSymbol/foldingRange/selectionRange/documentHighlight)
# are still measured and printed but not enforced there -- the shared
# 4-core runner is ~10x slower than the box these targets were set on
# (completion 7 ms local vs 81 ms on the runner), and #5352 is closed.
ISSUE_OF = {
    "definition": 5351,
    "hover": 5351,
    "references": 5351,
    "completion": 5351,
    "documentSymbol": 5352,
    "foldingRange": 5352,
    "selectionRange": 5352,
    "documentHighlight": 5352,
}

TARGETS_MS = {
    "definition": 20.0,
    "hover": 20.0,
    "references": 100.0,
    "completion": 100.0,
    "documentSymbol": 60.0,
    "foldingRange": 30.0,
    "selectionRange": 60.0,
    "documentHighlight": 60.0,
}


def gen(n: int) -> str:
    lines = ['(require "std/string" all:)', ""]
    for i in range(n):
        prev = f"(syn-f{i - 1} (+ x 1))" if i else "x"
        lines += [
            f"; synthetic function {i}",
            f"(define (syn-f{i} x)",
            "  (let ((y (* x 2)))",
            f"    (if (> y 100) y {prev})))",
        ]
    return "\n".join(lines) + "\n"


def call_site(text: str, n: int) -> tuple[int, int]:
    lines = text.split("\n")
    needle = f"(syn-f{n - 2} "
    for idx in range(len(lines) - 1, -1, -1):
        col = lines[idx].find(needle)
        if col >= 0:
            return idx, col + 1
    raise AssertionError("call site not found")


def one(client_method: str, text: str, pos: tuple[int, int] | None, root: str) -> float:
    path = Path(root) / "syn.aura"
    path.write_text(text, encoding="utf-8")
    client = Client()
    client.initialize(caps={}, root=Path(root).as_uri())
    client.did_open(path.as_uri(), text)
    started = time.perf_counter()
    while time.perf_counter() - started < 30:
        if any(f.get("method") == "textDocument/publishDiagnostics" for f in client.pump(0.05)):
            break
    params: dict = {"textDocument": {"uri": path.as_uri()}}
    if pos is not None:
        params["position"] = {"line": pos[0], "character": pos[1]}
    if client_method == "textDocument/references":
        params["context"] = {"includeDeclaration": True}
    if client_method == "textDocument/completion":
        params["position"]["character"] += 3
    started = time.perf_counter()
    client.send({"jsonrpc": "2.0", "id": 99, "method": client_method, "params": params})
    reply = None
    while time.perf_counter() - started < 60 and reply is None:
        for frame in client.pump(0.001):
            if frame.get("id") == 99:
                reply = frame
    elapsed = (time.perf_counter() - started) * 1000.0
    client.close()
    return elapsed


def measure(method: str, text: str, pos: tuple[int, int] | None, runs: int) -> float:
    return statistics.median(one(method, text, pos, tempfile.mkdtemp(prefix="lsplat-")) for _ in range(runs))


REQUESTS = (
    ("textDocument/definition", "definition", True),
    ("textDocument/hover", "hover", True),
    ("textDocument/references", "references", True),
    ("textDocument/completion", "completion", True),
    ("textDocument/documentSymbol", "documentSymbol", False),
    ("textDocument/foldingRange", "foldingRange", False),
    ("textDocument/selectionRange", "selectionRange", True),
    ("textDocument/documentHighlight", "documentHighlight", True),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=5, help="processes per data point")
    parser.add_argument("--check", action="store_true", help="fail on the issue targets")
    parser.add_argument(
        "--issue",
        type=int,
        default=0,
        help="with --check, only enforce this issue's rows (5351 or 5352; 0 = both)",
    )
    args = parser.parse_args()

    failures: list[str] = []
    previous: dict[str, float] = {}
    for n in SIZES:
        text = gen(n)
        pos = call_site(text, n)
        row = [f"lines={text.count(chr(10)):5d}"]
        for method, name, needs_pos in REQUESTS:
            value = measure(method, text, pos if needs_pos else None, args.runs)
            row.append(f"{name}={value:6.1f}ms")
            checked = args.issue == 0 or ISSUE_OF.get(name) == args.issue
            if args.check and checked:
                if n == SIZES[-1] and value > TARGETS_MS[name]:
                    failures.append(f"{name} {value:.1f}ms > {TARGETS_MS[name]:.0f}ms at 20002 lines")
                if name in previous and value > previous[name] * 5.0:
                    failures.append(f"{name} grew {value / previous[name]:.1f}x in the last 4x lines")
            previous[name] = value
        print("  ".join(row), flush=True)

    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
