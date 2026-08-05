"""Local mutation catalog — structured search without LLM.

Each template is a complete (define (gate x) ...) form. Agents pick
templates (optionally guided by current fail pattern). This is the
Aura-side "mutation search" counterpart to MiniMax freeform.
"""

from __future__ import annotations

from common import Proposal

# Catalog of legal gate bodies. Labels are stable for arbiter / CSV.
GATE_CATALOG: dict[str, str] = {
    "deny-all": "(define (gate x) #f)",
    "allow-all": "(define (gate x) #t)",
    "ge-1": "(define (gate x) (>= x 1))",
    "ge-2": "(define (gate x) (>= x 2))",
    "ge-3": "(define (gate x) (>= x 3))",
    "ge-4": "(define (gate x) (>= x 4))",  # passes suite
    "ge-5": "(define (gate x) (>= x 5))",  # passes suite
    "ge-6": "(define (gate x) (>= x 6))",  # fails admit-5
    "ge-10": "(define (gate x) (>= x 10))",
    "gt-4": "(define (gate x) (> x 4))",  # passes (5,10) fails? 5>4 ok
    "lt-5": "(define (gate x) (< x 5))",  # inverted — poison
    "eq-5": "(define (gate x) (= x 5))",  # too narrow
}


def local_proposals(
    *,
    agent_prefix: str = "local",
    fail_count: int = 4,
    include_poison: bool = True,
) -> list[Proposal]:
    """Emit a fanout of local proposals.

    When fail_count is high, prefer widen-ish labels first (still returns
    a multi-hypothesis set — arbiter ranks by dry-run V).
    """
    order = [
        "ge-4",
        "ge-5",
        "ge-3",
        "ge-2",
        "gt-4",
        "ge-1",
        "ge-6",
        "allow-all",
        "deny-all",
        "ge-10",
        "eq-5",
    ]
    if include_poison:
        order.append("lt-5")

    # Rotate priority when already somewhat healthy (explore tighten).
    if fail_count <= 1:
        order = ["ge-5", "ge-4", "gt-4", "ge-6", "ge-3"] + [
            x for x in order if x not in {"ge-5", "ge-4", "gt-4", "ge-6", "ge-3"}
        ]

    props: list[Proposal] = []
    for i, label in enumerate(order):
        code = GATE_CATALOG[label]
        props.append(
            Proposal(
                agent_id=f"{agent_prefix}-{i:02d}",
                source="local",
                code=code,
                label=label,
                note=f"catalog fail_hint={fail_count}",
            )
        )
    return props


def local_beam(fail_count: int, k: int = 4) -> list[Proposal]:
    """Top-k local hypotheses for a beam-style wave."""
    return local_proposals(fail_count=fail_count, include_poison=True)[:k]
