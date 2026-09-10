#!/usr/bin/env python3
"""Issue #3632: negative row the manifest schema can't express —
load_mailbox_bp_recent (the admit source) must never read the sender
sketch: the receiver-capacity contract stays byte-identical to
pre-#3632. Substring rows live in scripts/coverage/manifests/3632.json
(run by run_checks.py --all via runner.py)."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    text = (ROOT / "src" / "orch" / "agent_spawn.h").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"load_mailbox_bp_recent\(std::string_view scope_id\) noexcept \{(.*?)\n\}", text, re.S)
    if not m:
        print("FAIL: load_mailbox_bp_recent not found", file=sys.stderr)
        return 1
    body = m.group(1)
    if "top_sender" in body:
        print(
            "FAIL: AC11 load_mailbox_bp_recent must not read the sender sketch (receiver-capacity contract preserved)",
            file=sys.stderr,
        )
        return 1
    print("check_scope_bp_sender_attribution_3632: AC11 admit-source purity OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
