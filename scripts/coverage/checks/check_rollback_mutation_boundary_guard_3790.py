#!/usr/bin/env python3
"""#3790: rollback / rollback-since must acquire MutationBoundaryGuard."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "src/compiler/evaluator_primitives_mutation.cpp"


def main() -> int:
    text = SRC.read_text(encoding="utf-8", errors="replace")
    errors: list[str] = []
    if "mutate_dispatch.hh" not in text:
        errors.append("mutation.cpp missing mutate_dispatch.hh include")
    if "Issue #3790" not in text:
        errors.append("missing #3790 cite")

    cases = [
        ('add("rollback"', "workspace_flat_->rollback(", "rollback"),
        ('add("rollback-since"', "workspace_flat_->rollback_since(", "rollback-since"),
    ]
    for needle, write_tok, label in cases:
        i = text.find(needle)
        if i < 0:
            errors.append(f"missing {needle}")
            continue
        j = text.find("\n    add(", i + len(needle))
        win = text[i : j if j > i else i + 3000]
        if "mutate_dispatch_try_acquire" not in win:
            errors.append(f"{label}: missing mutate_dispatch_try_acquire")
        if "require_effect" not in win:
            errors.append(f"{label}: missing require_effect gate (#3722)")
        acq = win.find("mutate_dispatch_try_acquire")
        wr = win.find(write_tok)
        if acq < 0 or wr < 0 or not (acq < wr):
            errors.append(f"{label}: acquire must precede {write_tok}")

    if errors:
        for e in errors:
            print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print("OK: #3790 rollback MutationBoundaryGuard present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
