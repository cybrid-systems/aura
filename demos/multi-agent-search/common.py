"""Control-theoretic primitives for multi-agent-search.

Domain: budget gate — (gate x) should deny low x and admit mid/high x.

    V = 10 * fail_count + 0.1 * node_count + energy
    energy = 0.05 * |Δnodes|
    S = fail_count == 0  ∧  node_count ≤ 1.5 · initial

fail_count ∈ {0..4} from the fixed verify suite (not a single boolean).
This makes V a real multi-objective residual, not a one-bit flag.
"""

from __future__ import annotations

import csv
import json
import os
import re
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass, field
from pathlib import Path

RESULTS = Path(__file__).resolve().parent / "results"
RESULTS.mkdir(parents=True, exist_ok=True)

FILTER_DELTA_V = 0.5
ENERGY_SCALE = 0.05
# Seed (define (gate x) #f) is tiny on FlatAST (~3 nodes); correct
# (>= x T) forms are denser (~6). Use a floor so S is reachable.
S_SIZE_RATIO = 2.0
S_SIZE_FLOOR = 12.0

# Verify suite for gate (Aura truthiness: #f / 0 = deny, else admit).
# Spec: deny 0,3 ; admit 5,10  → threshold in {4,5}.
GATE_CASES: list[tuple[int, bool]] = [
    (0, False),
    (3, False),
    (5, True),
    (10, True),
]

SEED_CODE = "(define (gate x) #f)"


@dataclass
class Proposal:
    agent_id: str
    source: str  # local | llm | skip | deny
    code: str
    label: str
    note: str = ""


@dataclass
class StepRecord:
    step: int
    V: float
    fail_count: int
    node_count: float
    energy: float
    rejected: bool
    in_S: bool
    action: str
    n_proposals: int = 0
    n_llm: int = 0
    n_local: int = 0
    picked: str = ""
    extra: str = ""


@dataclass
class WaveStats:
    proposals: list[Proposal] = field(default_factory=list)
    ranked: list[tuple[Proposal, float, float]] = field(default_factory=list)
    picked: Proposal | None = None
    delta_v: float = 0.0
    rejected_by_filter: int = 0
    install_fails: int = 0


def energy_of(old_nc: float, new_nc: float) -> float:
    return ENERGY_SCALE * abs(float(new_nc) - float(old_nc))


def V_value(fail_count: int, node_count: float, energy: float) -> float:
    return 10.0 * int(fail_count) + 0.1 * float(node_count) + float(energy)


def in_S(
    fail_count: int,
    node_count: float,
    initial_size: float,
    *,
    size_ratio: float = S_SIZE_RATIO,
    size_floor: float = S_SIZE_FLOOR,
) -> bool:
    bound = max(size_ratio * float(initial_size), float(size_floor))
    return int(fail_count) == 0 and float(node_count) <= bound


def save_trajectory(records: list[StepRecord], tag: str) -> Path:
    out = RESULTS / f"{tag}.csv"
    fields = (
        list(asdict(records[0]).keys())
        if records
        else [
            "step",
            "V",
            "fail_count",
            "node_count",
            "energy",
            "rejected",
            "in_S",
            "action",
            "n_proposals",
            "n_llm",
            "n_local",
            "picked",
            "extra",
        ]
    )
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in records:
            w.writerow(asdict(r))
    return out


# ── MiniMax / OpenAI-compatible LLM ──────────────────────────────────


def _resolve_api_key() -> str | None:
    for var in (
        "AURA_LLM_API_KEY",
        "MINIMAX_API_KEY",
        "OPENAI_API_KEY",
        "GROK_API_KEY",
        "DEEPSEEK_API_KEY",
    ):
        v = os.environ.get(var)
        if v:
            return v
    return None


def llm_configured() -> bool:
    return _resolve_api_key() is not None


def _resolve_base_url() -> str:
    return os.environ.get("AURA_LLM_BASE_URL") or "https://api.minimax.chat/v1"


def _resolve_model() -> str:
    # Default MiniMax M3 for this demo (override with AURA_LLM_MODEL).
    return os.environ.get("AURA_LLM_MODEL") or "MiniMax-M3"


def strip_llm_fences(text: str) -> str:
    text = text.strip()
    text = re.sub(r"<think>[\s\S]*?</think>", "", text, flags=re.I)
    text = re.sub(r"<thinking>[\s\S]*?</thinking>", "", text, flags=re.I)
    text = re.sub(r"^```(?:python|aura|lisp|scheme|text)?\s*\n", "", text)
    text = re.sub(r"\n```\s*$", "", text)
    return text.strip()


def llm_chat(
    prompt: str,
    *,
    system: str = "You propose short structured mutations.",
    temperature: float = 0.2,
    max_retries: int = 3,
    timeout_s: float = 60.0,
) -> str:
    api_key = _resolve_api_key()
    if not api_key:
        raise RuntimeError("No LLM API key (set AURA_LLM_API_KEY or MINIMAX_API_KEY)")
    base_url = _resolve_base_url().rstrip("/")
    model = _resolve_model()
    body = json.dumps(
        {
            "model": model,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": prompt},
            ],
            "temperature": temperature,
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        f"{base_url}/chat/completions",
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
    )
    last_err: Exception | None = None
    for attempt in range(max_retries):
        try:
            with urllib.request.urlopen(req, timeout=timeout_s) as resp:
                data = json.loads(resp.read())
            content = data["choices"][0]["message"]["content"]
            return strip_llm_fences(content)
        except (
            urllib.error.URLError,
            urllib.error.HTTPError,
            KeyError,
            TimeoutError,
            json.JSONDecodeError,
        ) as exc:
            last_err = exc
            time.sleep(0.4 * (2**attempt))
    raise RuntimeError(f"LLM call failed after {max_retries}: {last_err}")


def llm_chat_many(
    prompts: list[tuple[str, str, str]],
    *,
    system: str,
    max_workers: int = 4,
) -> list[tuple[str, str | None, str]]:
    """Concurrent MiniMax calls.

    prompts: list of (agent_id, user_prompt, note)
    returns: list of (agent_id, response_or_None, error_or_ok)
    """
    if not prompts:
        return []

    def one(item: tuple[str, str, str]) -> tuple[str, str | None, str]:
        aid, prompt, _note = item
        try:
            return aid, llm_chat(prompt, system=system), "ok"
        except Exception as exc:  # noqa: BLE001 — surface to arbiter
            return aid, None, f"llm-err:{exc}"[:120]

    out: list[tuple[str, str | None, str]] = []
    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futs = {pool.submit(one, p): p[0] for p in prompts}
        for fut in as_completed(futs):
            out.append(fut.result())
    # stable order by agent_id
    order = {p[0]: i for i, p in enumerate(prompts)}
    out.sort(key=lambda t: order.get(t[0], 999))
    return out
