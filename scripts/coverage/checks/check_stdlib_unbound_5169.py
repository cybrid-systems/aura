#!/usr/bin/env python3
"""Gate: a qualified call in lib/std names something that exists (Issue #5169).

A call head of the form name:name must be one of:

- a same-file define or defmacro (forward references count),
- an engine primitive (public catalog, prim_names.h, or add_hidden),
- a binding imported by (require ...). all: keeps the module's names.
  Without all:, the loader also binds basename:name.

register_stats_impl entries are stats keys, not callables. add_mutate
names are (mutate :op) dispatch, not (mutate:op) calls. Locals without a
colon are out of scope: a full unqualified resolver false-positives on
parameters.

Comments, strings, and '(...) forms are not calls.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts" / "tools"))

from prim_surface import scan_names  # noqa: E402

STD = ROOT / "lib" / "std"
SRC = ROOT / "src"

_DEFINE_HEAD_RE = re.compile(r"\(\s*(?:define|defmacro)\s+\(\s*([^\s()\[\]{},']+)")
_DEFINE_VALUE_RE = re.compile(r"\(\s*(?:define|defmacro)\s+([^\s()\[\]{},']+)")
_CALL_RE = re.compile(r"\(\s*([^\s()\[\]{},']+)")
_QUAL_RE = re.compile(r"^[^:]+:[^:]+$")
_HIDDEN_RE = re.compile(r'add_hidden\(\s*"([^"]+)"')
_MUTATE_RE = re.compile(r'add_mutate\(\s*"([^"]+)"')
_PRIM_CONST_RE = re.compile(r'inline constexpr const char\* \w+ = "([^"]+)"')
_ALL_RE = re.compile(r"(^|[\s()])all:([\s()]|$)")
_SRC_SUFFIXES = {".cpp", ".h", ".hh", ".hpp", ".ixx", ".ccm"}
# #\newline and friends are one token. A single-character literal (#\", #\;, #\()
# must not open a string, a comment, or a paren.
_CHAR_NAMES = ("newline", "return", "space", "tab")

_DEFINED: dict[Path, set[str]] = {}
_ENGINE: frozenset[str] | None = None


def _skip_char(chars: list[str], i: int) -> int:
    """Index after a #\\ literal at i, or i when chars[i] is not one."""
    n = len(chars)
    if i + 1 >= n or chars[i] != "#" or chars[i + 1] != "\\":
        return i
    j = i + 2
    if j >= n:
        return n
    if chars[j].isalpha():
        for name in _CHAR_NAMES:
            end = j + len(name)
            if "".join(chars[j:end]) == name and (end >= n or not (chars[end].isalnum() or chars[end] in "-_")):
                return end
        return j + 1
    return j + 1


def _read_string(chars: list[str], i: int) -> tuple[str, int]:
    """Read a string starting at the opening quote. Return (value, index after)."""
    j = i + 1
    n = len(chars)
    out: list[str] = []
    while j < n:
        ch = chars[j]
        if ch == "\\":
            j += 1
            if j < n:
                out.append(chars[j])
                j += 1
            continue
        if ch == '"':
            return "".join(out), j + 1
        out.append(ch)
        j += 1
    return "".join(out), j


def _form_end(chars: list[str], start: int) -> int:
    """Index just after the form whose opening paren is at start."""
    depth = 0
    j = start
    n = len(chars)
    while j < n:
        nxt = _skip_char(chars, j)
        if nxt != j:
            j = nxt
            continue
        ch = chars[j]
        if ch == ";":
            while j < n and chars[j] != "\n":
                j += 1
            continue
        if ch == '"':
            _, j = _read_string(chars, j)
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    return n


def _blank_comment(chars: list[str], i: int) -> int:
    n = len(chars)
    while i < n and chars[i] != "\n":
        chars[i] = " "
        i += 1
    return i


def _blank_string(chars: list[str], i: int) -> int:
    chars[i] = " "
    j = i + 1
    n = len(chars)
    while j < n:
        ch = chars[j]
        if ch == "\\":
            chars[j] = " "
            j += 1
            if j < n and chars[j] != "\n":
                chars[j] = " "
                j += 1
            elif j < n:
                j += 1
            continue
        if ch == '"':
            chars[j] = " "
            return j + 1
        if ch != "\n":
            chars[j] = " "
        j += 1
    return j


def _blank_form(chars: list[str], start: int) -> int:
    """Blank a form starting at '(' , preserving newlines. Return index after it."""
    depth = 0
    j = start
    n = len(chars)
    while j < n:
        nxt = _skip_char(chars, j)
        if nxt != j:
            j = nxt
            continue
        ch = chars[j]
        if ch == ";":
            j = _blank_comment(chars, j)
            continue
        if ch == '"':
            j = _blank_string(chars, j)
            continue
        if ch == "(":
            depth += 1
            chars[j] = " "
            j += 1
            continue
        if ch == ")":
            chars[j] = " "
            depth -= 1
            j += 1
            if depth == 0:
                return j
            continue
        if ch != "\n":
            chars[j] = " "
        j += 1
    return j


def prepare(text: str) -> tuple[str, list[tuple[str, bool]]]:
    """Blank comments, strings, and '(...) forms. Collect (require spec, all?)."""
    chars = list(text)
    reqs: list[tuple[str, bool]] = []
    i = 0
    n = len(chars)
    while i < n:
        nxt = _skip_char(chars, i)
        if nxt != i:
            i = nxt
            continue
        ch = chars[i]
        if ch == ";":
            i = _blank_comment(chars, i)
            continue
        if ch == '"':
            i = _blank_string(chars, i)
            continue
        if ch == "'":
            j = i + 1
            while j < n and chars[j] in " \t\r\n":
                j += 1
            if j < n and chars[j] == "(":
                k = i
                while k < j:
                    if chars[k] != "\n":
                        chars[k] = " "
                    k += 1
                i = _blank_form(chars, j)
                continue
            i += 1
            continue
        if ch == "(":
            j = i + 1
            while j < n and chars[j] in " \t\r\n":
                j += 1
            if j < n and chars[j] == "r" and "".join(chars[j : j + 7]) == "require":
                nxt = j + 7
                if nxt >= n or chars[nxt] in " \t\r\n()":
                    k = nxt
                    while k < n and chars[k] in " \t\r\n":
                        k += 1
                    if k < n and chars[k] == '"':
                        spec, _after = _read_string(chars, k)
                        end = _form_end(chars, i)
                        form = "".join(chars[i:end])
                        reqs.append((spec, _ALL_RE.search(form) is not None))
            i += 1
            continue
        i += 1
    return "".join(chars), reqs


def names_in(blank: str) -> set[str]:
    names = {m.group(1) for m in _DEFINE_HEAD_RE.finditer(blank)}
    names.update(m.group(1) for m in _DEFINE_VALUE_RE.finditer(blank))
    return names


def unbound_qualified_calls(text: str, bound: set[str] | None = None) -> list[tuple[int, str]]:
    """Qualified call heads in text that `bound` plus same-file defines miss.

    Line numbers are 1-based. Define heads are not calls. Used by the
    regression test to show the gate fails closed on a missing name.
    """
    blank, _reqs = prepare(text)
    known = set(bound or ())
    known |= names_in(blank)
    return _calls_missing(blank, known)


def _calls_missing(blank: str, bound: set[str]) -> list[tuple[int, str]]:
    heads = {m.start(1) for m in _DEFINE_HEAD_RE.finditer(blank)}
    out: list[tuple[int, str]] = []
    for m in _CALL_RE.finditer(blank):
        if m.start(1) in heads:
            continue
        name = m.group(1)
        if _QUAL_RE.fullmatch(name) is None or name in bound:
            continue
        out.append((blank.count("\n", 0, m.start(1)) + 1, name))
    return out


def module_file(spec: str) -> Path | None:
    spec = spec.strip()
    if not spec or any(part == ".." for part in spec.split("/")):
        return None
    if spec.endswith(".aura"):
        spec = spec[: -len(".aura")]
    if not spec:
        return None
    path = ROOT / "lib" / f"{spec}.aura"
    return path if path.is_file() else None


def prefix_of(spec: str) -> str:
    base = spec.strip().split("/")[-1]
    if base.endswith(".aura"):
        base = base[: -len(".aura")]
    return base + ":"


def defined_names(path: Path) -> set[str]:
    path = path.resolve()
    hit = _DEFINED.get(path)
    if hit is not None:
        return hit
    text = path.read_text(encoding="utf-8", errors="replace")
    blank, _reqs = prepare(text)
    names = names_in(blank)
    _DEFINED[path] = names
    return names


def engine_names() -> frozenset[str]:
    """Callable names. Stats-catalog getters and mutate: dispatch stay out."""
    global _ENGINE
    if _ENGINE is not None:
        return _ENGINE
    hidden: set[str] = set()
    mutate: set[str] = set()
    for path in SRC.rglob("*"):
        if path.suffix not in _SRC_SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        hidden.update(_HIDDEN_RE.findall(text))
        mutate.update(_MUTATE_RE.findall(text))
    names = set(scan_names())
    prim_h = ROOT / "src" / "compiler" / "prim_names.h"
    names.update(_PRIM_CONST_RE.findall(prim_h.read_text(encoding="utf-8")))
    names.update(hidden - mutate)
    _ENGINE = frozenset(names)
    return _ENGINE


def scan_stdlib(engine: frozenset[str]) -> list[str]:
    findings: list[str] = []
    for path in sorted(STD.rglob("*.aura")):
        text = path.read_text(encoding="utf-8", errors="replace")
        blank, reqs = prepare(text)
        bound = set(engine)
        bound |= names_in(blank)
        for spec, all_p in reqs:
            mod = module_file(spec)
            if mod is None:
                continue
            exported = defined_names(mod)
            bound |= exported
            if not all_p:
                pre = prefix_of(spec)
                bound.update(pre + name for name in exported)
        rel = path.relative_to(ROOT).as_posix()
        for line, name in _calls_missing(blank, bound):
            findings.append(f"{rel}:{line}: {name}")
    return findings


def main() -> int:
    findings = scan_stdlib(engine_names())
    if findings:
        for item in findings:
            print(f"FAIL: {item}", file=sys.stderr)
        print(f"\n{len(findings)} unbound qualified call(s) in lib/std", file=sys.stderr)
        return 1
    print("OK: lib/std qualified calls resolve to a primitive, a define, or a required export")
    return 0


if __name__ == "__main__":
    sys.exit(main())
