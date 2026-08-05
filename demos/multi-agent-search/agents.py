"""Multi-agent proposers: local catalog + concurrent MiniMax M3.

Topology (per wave):
  local agents  ──┐
                  ├──► proposals[] ──► arbiter (elsewhere)
  llm agents    ──┘   (ThreadPool concurrent API)

LLM is constrained to emit a single integer threshold T for:
  (define (gate x) (>= x T))
so MiniMax does not need full Aura/Scheme fluency.
"""

from __future__ import annotations

import re

from common import Proposal, llm_chat_many, llm_configured
from local_search import GATE_CATALOG, local_beam

SYSTEM_LLM = (
    "You help tune a budget gate for Aura Lisp. "
    "The gate is (define (gate x) (>= x T)). "
    "Spec: deny x=0 and x=3; admit x=5 and x=10. "
    "Reply with ONLY one integer T in 0..12. No words, no code."
)


def _parse_threshold(text: str) -> int | None:
    text = text.strip()
    m = re.search(r"-?\d+", text)
    if not m:
        return None
    t = int(m.group(0))
    if t < 0 or t > 20:
        return None
    return t


def threshold_to_code(t: int) -> str:
    return f"(define (gate x) (>= x {t}))"


def llm_proposals(
    *,
    n_agents: int = 3,
    fail_count: int = 4,
    current_code: str = "",
    max_workers: int = 4,
) -> list[Proposal]:
    """Concurrent MiniMax calls → threshold proposals."""
    if not llm_configured() or n_agents <= 0:
        return []

    prompts: list[tuple[str, str, str]] = []
    for i in range(n_agents):
        aid = f"llm-{i:02d}"
        # Light diversity in prompt so temperature=0 models can still differ
        # when combined with agent index hint (some providers ignore temp).
        user = (
            f"Agent {i}/{n_agents}. fail_count={fail_count}.\n"
            f"Current code:\n{current_code}\n"
            f"Prefer T around 3..6. Agent bias: T ≈ {3 + i}.\n"
            "Reply with one integer T only."
        )
        prompts.append((aid, user, f"bias={3 + i}"))

    results = llm_chat_many(prompts, system=SYSTEM_LLM, max_workers=max_workers)
    props: list[Proposal] = []
    for aid, text, status in results:
        if text is None:
            props.append(
                Proposal(
                    agent_id=aid,
                    source="llm",
                    code="",
                    label="llm-fail",
                    note=status,
                )
            )
            continue
        t = _parse_threshold(text)
        if t is None:
            props.append(
                Proposal(
                    agent_id=aid,
                    source="llm",
                    code="",
                    label="llm-parse-fail",
                    note=text[:80],
                )
            )
            continue
        props.append(
            Proposal(
                agent_id=aid,
                source="llm",
                code=threshold_to_code(t),
                label=f"llm-T{t}",
                note=f"raw={text[:40]}",
            )
        )
    return props


def fanout_proposals(
    *,
    mode: str,
    fail_count: int,
    current_code: str,
    n_local: int = 4,
    n_llm: int = 3,
    max_workers: int = 4,
) -> list[Proposal]:
    """Collect multi-hypothesis set for one search wave.

    mode:
      offline — local only
      hybrid  — local + llm (if key present; else local)
      live    — prefer llm, always keep local beam as baseline
    """
    props: list[Proposal] = []
    if mode in ("offline", "hybrid", "live"):
        props.extend(local_beam(fail_count, k=n_local))
    if mode in ("hybrid", "live"):
        props.extend(
            llm_proposals(
                n_agents=n_llm,
                fail_count=fail_count,
                current_code=current_code,
                max_workers=max_workers,
            )
        )
    # Drop empty-code proposals (llm fail) but keep them out of dry-run.
    return [p for p in props if p.code.strip()]


def catalog_labels() -> list[str]:
    return list(GATE_CATALOG.keys())
