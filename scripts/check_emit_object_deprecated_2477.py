#!/usr/bin/env python3
"""Issue #2477: emit_object fail-closed deprecation.

Contract:
  AC1 returns false + stderr message
  AC2 no .ir fopen write in emit_object bodies
  AC3 header documents deprecation
  AC4 no production jit::emit_object callers
  AC5 gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    jit = _read("src/compiler/aura_jit.cpp")
    hdr = _read("src/compiler/aura_jit.h")
    test = _read("tests/compiler/test_emit_object_deprecated_2477.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    main_cpp = _read("src/main.cpp")

    # Both LLVM and no-LLVM emit_object bodies (stop at function end).
    bodies = list(re.finditer(r"bool emit_object\(const std::string[^)]*\)\s*\{", jit))
    if len(bodies) < 1:
        fails.append("AC1: emit_object definition not found")
    for m in bodies:
        # Brace-match to end of this function only (avoid emit_object_module).
        m.end() - 1  # points at '{'
        depth = 0
        end = m.end()
        for j in range(m.end() - 1, min(m.end() + 800, len(jit))):
            c = jit[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    end = j + 1
                    break
        body = jit[m.start() : end]
        must("Issue #2477", "AC1", body)
        must("return false", "AC1", body)
        must_not("return true", "AC1", body)
        must("emit_object: deprecated, use emit_native_object instead", "AC1", body)
        must_not('out_path + ".ir"', "AC2", body)
        must_not("fopen", "AC2", body)

    must("2477", "AC3", hdr)
    must("Deprecated", "AC3", hdr)
    must("emit_native_object", "AC3", hdr)

    must_not("jit::emit_object(", "AC4", bridge)
    must_not("jit::emit_object(", "AC4", main_cpp)
    must("emit_native_object", "AC4", bridge)

    must("check_emit_object_deprecated_2477", "gate", build)
    must("cmd_emit_object_deprecated_coverage", "gate", build)
    must("test_emit_object_deprecated_2477", "gate", cmake)
    must("2477 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: emit_object fail-closed deprecation #2477 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
