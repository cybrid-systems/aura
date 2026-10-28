#!/usr/bin/env python3
"""Issue #4665: registration-time mutate:* hygiene gate inventory gate.

The MacroIntroduced default-deny face (#3191 / #3344 / #3542) is enforced
INSIDE each mutate:* body, so a new prim that forgets the helper compiles,
registers, and passes its own suite: the regression class (#3815 / #4406 /
#4248 / #4435 / #3889) was closed one point fix at a time because the
REGISTRATION POINT could not tell a gated prim from an ungated one.

Issue #4665 closes that hole with ONE reviewable inventory
(kMutateHygieneInventory, in src/compiler/evaluator_primitives_mutate.cpp)
plus registration-time enforcement in add_mutate: a mutate:* prim with no
inventory row is REFUSED - the registration installs a deny stub that names
the offending prim, so an ungated prim can never become callable.

This gate is the static half of that contract. It re-derives the inventory
rows and the registrations from the source and cross-checks them:

  AC1  every mutate:* registration has exactly one inventory row; a
       Required row's body really calls a MacroIntroduced hygiene helper
       (reject_structural_macro_hygiene / enforce_macro_hygiene_mutate_hotpath
       / hygiene_protected_error / record_hygiene_violation_attempt); the
       add_mutate registration point really wires the fail-closed arm.
  AC2  every Exempt row records a reason, and an Exempt row's body does NOT
       call a hygiene helper (a stale exemption is a silent hole).
  AC3  the closed point fixes stay closed: the prims those issues gated
       (#3815 / #4406 / #4248 / #3889) are Required in the inventory, and
       the rollback surface (#3344 / #4435) stays an explicit reviewed row.
  AC4  the inventory is reviewable in ONE file.

Every failure message NAMES the offending prim.

Usage:
  python3 scripts/coverage/checks/check_mutate_hygiene_gate_4665.py
  python3 scripts/coverage/checks/check_mutate_hygiene_gate_4665.py --probe-file PATH
  python3 scripts/coverage/checks/check_mutate_hygiene_gate_4665.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TARGET_REL = "src/compiler/evaluator_primitives_mutate.cpp"
INVENTORY_SYM = "kMutateHygieneInventory"

HYG_HELPERS = (
    "reject_structural_macro_hygiene",
    "enforce_macro_hygiene_mutate_hotpath",
    "hygiene_protected_error",
    "record_hygiene_violation_attempt",
)

# The inventory row shape:
#   {"mutate:<name>", MutateHygienePolicy::Required|Exempt, "<reason>"}
ROW_RE = re.compile(r'\{\s*"(mutate:[^"]+)"\s*,\s*MutateHygienePolicy::(Required|Exempt)\s*,\s*"([^"]*)"\s*\}')
# mutate:* registrations (the wrapper and the raw metadata add).
ADD_MUTATE_RE = re.compile(r'add_mutate\(\s*"(mutate:[^"]+)"')
RAW_ADD_RE = re.compile(r'(?<![A-Za-z0-9_])add\(\s*"(mutate:[^"]+)"')
# The registration point itself.
ADD_MUTATE_DEF_RE = re.compile(r"auto\s+add_mutate\s*=")
# Adjacent string literals (clang-format may split a long literal) -> merge.
ADJACENT_LITERAL_RE = re.compile(r'"([^"]*)"(\s+)"')

# Prims the closed point fixes gated - they must stay Required.
CLOSED_POINT_PRIMS = (
    ("#3815", "mutate:move-node"),
    ("#4406", "mutate:set-body"),
    ("#4406", "mutate:rebind"),
    ("#4248", "mutate:atomic-batch"),
    ("#3889", "mutate:from-verification-feedback"),
)
# The rollback surface stays an explicitly reviewed inventory row.
ROLLBACK_PRIM = "mutate:rollback-macro-introduced"

SRC_SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".ixx")


def _skip_literal(text: str, i: int) -> int:
    """Index just past the string/char literal that starts at text[i]."""
    n = len(text)
    # Raw string: R"delim( ... )delim"
    if i > 0 and text[i - 1] in "Rr" and text[i] == '"':
        j = text.find("(", i + 1)
        if 0 < j <= i + 20:
            delim = text[i + 1 : j]
            if all(ch.isalnum() or ch == "_" for ch in delim):
                end = text.find(")" + delim + '"', j + 1)
                if end >= 0:
                    return end + len(delim) + 2
    quote = text[i]
    i += 1
    while i < n:
        if text[i] == "\\":
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        i += 1
    return n


def _blank_comments(text: str) -> str:
    """Same-length copy of text with comments replaced by spaces.

    String literals are preserved (the prim names live inside them) but
    comments are dropped, so a commented-out registration is never
    discovered as a real one.
    """
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            i = _skip_literal(text, i)
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def _merge_adjacent_literals(text: str) -> str:
    """Merge adjacent string literals so a clang-format split stays parseable."""
    prev = None
    while prev != text:
        prev = text
        text = ADJACENT_LITERAL_RE.sub(r'"\1"', text)
    return text


def _lambda_body(text: str, after: int) -> str:
    """Body of the lambda that starts at/after the given offset.

    Comments and string literals are skipped while the capture list and the
    braced body are matched.
    """
    n = len(text)
    i = after
    # capture list
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            i = n if i < 0 else i
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"' or c == "'":
            i = _skip_literal(text, i)
            continue
        if c == "[":
            break
        i += 1
    if i >= n:
        return ""
    depth = 0
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            i = n if i < 0 else i
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"' or c == "'":
            i = _skip_literal(text, i)
            continue
        if c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth == 0:
                i += 1
                break
        i += 1
    # body
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            i = n if i < 0 else i
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"' or c == "'":
            i = _skip_literal(text, i)
            continue
        if c == "{":
            break
        i += 1
    if i >= n:
        return ""
    depth = 0
    start = i
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            i = n if i < 0 else i
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"' or c == "'":
            i = _skip_literal(text, i)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    return text[start:]


def _inventory_files() -> list[str]:
    """Repo-relative files that declare the inventory (AC4: exactly one)."""
    hits: list[str] = []
    src = ROOT / "src"
    if not src.is_dir():
        return hits
    for path in sorted(src.rglob("*")):
        if not path.is_file() or path.suffix not in SRC_SUFFIXES:
            continue
        try:
            body = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if INVENTORY_SYM in body:
            hits.append(str(path.relative_to(ROOT)))
    return hits


def check_text(text: str, *, repo_scope: bool, extra_reg_text: str = "") -> list[str]:
    """Cross-check the inventory against the registrations. Returns failures."""
    fails: list[str] = []
    masked = _blank_comments(text)
    merged = _merge_adjacent_literals(masked)

    rows: dict[str, tuple[str, str]] = {}
    for m in ROW_RE.finditer(merged):
        name, policy, reason = m.group(1), m.group(2), m.group(3)
        if name in rows:
            fails.append(f"AC1: duplicate inventory row for {name} in {INVENTORY_SYM} (#4665)")
            continue
        rows[name] = (policy, reason)

    regs: dict[str, str] = {}
    order: list[str] = []
    for regex in (ADD_MUTATE_RE, RAW_ADD_RE):
        for m in regex.finditer(masked):
            name = m.group(1)
            if name not in regs:
                order.append(name)
                regs[name] = _lambda_body(text, m.end())
    # Issue #4742: a mutate:* prim may be registered in a sibling
    # evaluator_primitives_* partition (the inventory still lives here, AC4).
    # Such a row is resolved against those partitions' registrations.
    if extra_reg_text:
        extra = _merge_adjacent_literals(_blank_comments(extra_reg_text))
        # Only RESOLVE rows here (AC2 direction). AC1 (a registration without
        # a row) stays scoped to this file: sibling partitions carry their own
        # reviewed rows and are not this inventory's completeness target.
        for regex in (ADD_MUTATE_RE, RAW_ADD_RE):
            for m in regex.finditer(extra):
                name = m.group(1)
                if name not in regs:
                    regs[name] = _lambda_body(extra_reg_text, m.end())

    # The completeness rows are about the REAL inventory (a probe file is
    # deliberately small), so they only run in repo scope.
    if repo_scope:
        if len(rows) < 20:
            fails.append(f"AC1: {INVENTORY_SYM} has only {len(rows)} rows - the full mutate:* set is expected (#4665)")
        if len(regs) < 20:
            fails.append(f"AC1: discovered only {len(regs)} mutate:* registrations (#4665)")

    # AC1 - a registration without a row is the exact #4665 regression class.
    for name in order:
        if name not in rows:
            fails.append(
                f"AC1: {name} is registered without a hygiene gate policy row - add a Required "
                f"or Exempt(reason) row to {INVENTORY_SYM} (#4665)"
            )

    for name, (policy, reason) in rows.items():
        if name not in regs:
            fails.append(
                f"AC2: inventory row {name} has no registration in this file - drop the row or "
                f"register the prim (#4665)"
            )
            continue
        # The helper must be CALLED in code (comments are blanked first, so a
        # comment-only mention never counts as a gate).
        body = _blank_comments(regs[name])
        hits = [h for h in HYG_HELPERS if re.search(r"\b" + re.escape(h) + r"\s*\(", body)]
        if policy == "Required":
            if not hits:
                fails.append(
                    f"AC1: {name} declares Required but its body calls no MacroIntroduced hygiene "
                    f"helper ({' / '.join(HYG_HELPERS)}) (#4665)"
                )
        else:
            if not reason.strip():
                fails.append(
                    f"AC2: {name} is Exempt without a reason - record why the hygiene gate does not apply (#4665)"
                )
            if hits:
                fails.append(
                    f"AC2: {name} is Exempt but its body calls {hits[0]} - stale exemption, declare it Required (#4665)"
                )

    # AC1 - the registration point must really enforce the inventory.
    am = ADD_MUTATE_DEF_RE.search(masked)
    if not am:
        fails.append("AC1: the add_mutate registration point was not found (#4665)")
    else:
        am_body = _lambda_body(text, am.end())
        if "mutate_hygiene_gate_allow" not in am_body:
            fails.append(
                "AC1: the add_mutate registration point does not call "
                "mutate_hygiene_gate_allow - registration-time enforcement is missing (#4665)"
            )
        for needle in (
            "lookup_mutate_hygiene_policy",
            "hygiene-gate-undeclared",
            "HYGIENE_POLICY_REQUIRED",
            "HYGIENE_POLICY_EXEMPT",
        ):
            if needle not in text:
                fails.append(
                    f"AC1: the registration point does not wire {needle} - "
                    f"registration-time enforcement is missing (#4665)"
                )

    # AC3 - the closed point fixes stay closed (about the REAL inventory).
    if repo_scope:
        for issue, name in CLOSED_POINT_PRIMS:
            policy = rows.get(name, ("", ""))[0]
            if policy != "Required":
                fails.append(f"AC3: {name} was gated by {issue} - it must stay Required in {INVENTORY_SYM} (#4665)")
        rollback = rows.get(ROLLBACK_PRIM)
        if rollback is None:
            fails.append(f"AC3: {ROLLBACK_PRIM} (the #3344 / #4435 rollback surface) has no row (#4665)")
        elif "rollback" not in rollback[1].lower():
            fails.append(f"AC3: {ROLLBACK_PRIM} must record why the rollback surface is exempt (#4665)")

    # AC4 - the inventory is reviewable in ONE file.
    if repo_scope:
        files = _inventory_files()
        if len(files) != 1:
            fails.append(f"AC4: {INVENTORY_SYM} must live in exactly ONE file, found {len(files)}: {files} (#4665)")

    return fails


def _synthetic_source(*, ungated: str = "", no_row: str = "", no_reason: str = "") -> str:
    """Build a synthetic mutate registration source (self-test fixture)."""
    gated = [name for _, name in CLOSED_POINT_PRIMS]
    names = list(dict.fromkeys(gated + [ROLLBACK_PRIM] + [f"mutate:synthetic-{i}" for i in range(20)]))
    lines = [
        "enum class MutateHygienePolicy : std::uint8_t { Required, Exempt };",
        "struct MutateHygieneRow { const char* name; MutateHygienePolicy policy; const char* reason; };",
        f"inline constexpr MutateHygieneRow {INVENTORY_SYM}[] = {{",
    ]
    for name in names:
        if name == no_row:
            continue
        if name == ROLLBACK_PRIM:
            reason = "" if name == no_reason else "the rollback surface; default-deny would break it"
            lines.append(f'    {{"{name}", MutateHygienePolicy::Exempt, "{reason}"}},')
        else:
            lines.append(f'    {{"{name}", MutateHygienePolicy::Required, ""}},')
    lines += [
        "};",
        "void reg() {",
        "    auto add_mutate = [&](std::string name, auto fn) {",
        "        const auto* row = lookup_mutate_hygiene_policy(name);",
        "        if (!row) { /* hygiene-gate-undeclared */ }",
        "        /* HYGIENE_POLICY_REQUIRED HYGIENE_POLICY_EXEMPT */",
        "        mutate_hygiene_gate_allow(name);",
        "    };",
    ]
    for name in names:
        # The no_row case keeps the REGISTRATION but drops its inventory row:
        # that is the exact "registered without a policy row" regression.
        lines.append(f'    add_mutate("{name}", [&](const auto& a) -> int {{')
        if name != ROLLBACK_PRIM and name != ungated:
            lines.append("        if (auto err = reject_structural_macro_hygiene(a)) return err;")
        lines += ["        return 0;", "    });"]
    lines.append("}")
    return "\n".join(lines) + "\n"


def _self_test() -> int:
    """Prove the gate fails on an ungated prim and NAMES it."""
    fails = check_text(_synthetic_source(), repo_scope=False)
    if fails:
        print("FAIL #4665 self-test: the well-formed source should pass:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1

    cases = (
        ("ungated prim", _synthetic_source(ungated="mutate:move-node"), "mutate:move-node"),
        ("undeclared prim", _synthetic_source(no_row="mutate:move-node"), "mutate:move-node"),
        ("reason-less exemption", _synthetic_source(no_reason=ROLLBACK_PRIM), ROLLBACK_PRIM),
    )
    for label, text, name in cases:
        fails = check_text(text, repo_scope=False)
        if not any(name in f for f in fails):
            print(f"FAIL #4665 self-test: the {label} was not named", file=sys.stderr)
            for f in fails:
                print(f"  - {f}", file=sys.stderr)
            return 1

    print("OK #4665 self-test: the inventory gate names the offending prim")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Issue #4665 mutate hygiene gate inventory")
    ap.add_argument("--probe-file", help="lint an alternative mutate registration source")
    ap.add_argument("--self-test", action="store_true", help="run the built-in probes")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    target = ROOT / TARGET_REL
    repo_scope = True
    if args.probe_file:
        target = Path(args.probe_file)
        repo_scope = False
    if not target.is_file():
        print(f"FAIL #4665 mutate_hygiene_gate: missing {target}", file=sys.stderr)
        return 1

    text = target.read_text(encoding="utf-8", errors="replace")
    extra_reg_text = ""
    if repo_scope:
        parts = []
        for path in sorted((ROOT / "src" / "compiler").glob("evaluator_primitives_*.cpp")):
            if path.name == "evaluator_primitives_mutate.cpp":
                continue
            try:
                parts.append(path.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                continue
        extra_reg_text = chr(10).join(parts)
    fails = check_text(text, repo_scope=repo_scope, extra_reg_text=extra_reg_text)
    if fails:
        print(f"FAIL #4665 mutate_hygiene_gate: {target}", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1

    masked = _blank_comments(text)
    rows = len(ROW_RE.findall(_merge_adjacent_literals(masked)))
    regs = len(ADD_MUTATE_RE.findall(masked))
    print(
        f"OK #4665 mutate_hygiene_gate: {rows} inventory rows, {regs} add_mutate registrations, "
        f"registration-time enforcement wired"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
