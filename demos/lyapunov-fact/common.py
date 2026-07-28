"""Common types + V definition for the lyapunov-fact demo.

V (Lyapunov-like) is identical on both sides:

    V = 10 * test_fail + 2 * recursive_residual + 0.1 * node_count + energy

where
    energy = 0.05 * |new_node_count − old_node_count|

(energy is a soft size-penalty so a correct recursive→iterative rewrite
can still decrease V; a raw |Δnodes| term made every good rewrite look
like a drawdown and stuck the ΔV filter.)

S (target set):
    test_fail == 0 AND recursive_residual == 0 AND node_count <= 1.3 * initial
"""

from __future__ import annotations

import csv
import json
import os
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path

RESULTS = Path(__file__).resolve().parent / "results"
RESULTS.mkdir(parents=True, exist_ok=True)

# Shared numerical constants (both engines, both controllers).
TEST_VALUE = 3628800  # fact(10)
FILTER_DELTA_V = 0.5  # hard reject if candidate ΔV exceeds this
ENERGY_SCALE = 0.05  # soft size-change penalty in V
# Aura FlatAST counts fewer nodes than CPython's ast for the recursive
# seed (~17 vs ~25) but the iterative rewrite is denser in Aura (~24),
# so 1.3× rejects a correct rewrite. 1.5× admits both engines' GOOD-ITER.
S_SIZE_RATIO = 1.5


@dataclass
class StepRecord:
    step: int
    V: float
    test_fail: int
    recursive_residual: int
    node_count: float
    energy: float
    rejected: bool
    in_S: bool
    action: str
    extra: str = ""


def energy_of(old_nc: float, new_nc: float) -> float:
    return ENERGY_SCALE * abs(float(new_nc) - float(old_nc))


def save_trajectory(records: list[StepRecord], tag: str) -> Path:
    """Write trajectory to a CSV. Returns the output path."""
    out = RESULTS / f"{tag}.csv"
    fieldnames = [
        "step",
        "V",
        "test_fail",
        "recursive_residual",
        "node_count",
        "energy",
        "rejected",
        "in_S",
        "action",
        "extra",
    ]
    with open(out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in records:
            writer.writerow(asdict(r))
    return out


def V_value(test_fail: int, recursive_residual: int, node_count: float, energy: float) -> float:
    return 10.0 * test_fail + 2.0 * recursive_residual + 0.1 * float(node_count) + float(energy)


def in_S(
    test_fail: int,
    recursive_residual: int,
    node_count: float,
    initial_size: float,
    *,
    size_ratio: float = S_SIZE_RATIO,
) -> bool:
    return test_fail == 0 and recursive_residual == 0 and float(node_count) <= size_ratio * float(initial_size)


# --- shared LLM client (exp2) -----------------------------------------


def _resolve_api_key() -> str:
    for var in ("AURA_LLM_API_KEY", "OPENAI_API_KEY", "GROK_API_KEY", "DEEPSEEK_API_KEY"):
        v = os.environ.get(var)
        if v:
            return v
    raise RuntimeError("No LLM API key found in env (set AURA_LLM_API_KEY)")


def _resolve_base_url() -> str:
    return os.environ.get("AURA_LLM_BASE_URL") or "https://api.minimax.chat/v1"


def _resolve_model() -> str:
    return os.environ.get("AURA_LLM_MODEL") or "grok-4"


def strip_llm_fences(text: str) -> str:
    """Drop markdown fences and model 'thinking' blocks."""
    import re

    text = text.strip()
    text = re.sub(r"<think>[\s\S]*?</think>", "", text, flags=re.I)
    text = re.sub(r"<thinking>[\s\S]*?</thinking>", "", text, flags=re.I)
    text = re.sub(r"^```(?:python|aura|lisp|scheme)?\s*\n", "", text)
    text = re.sub(r"\n```\s*$", "", text)
    return text.strip()


def llm_chat(
    prompt: str,
    *,
    system: str = "You are a code refactorer.",
    max_retries: int = 3,
    timeout_s: float = 60.0,
) -> str:
    """OpenAI-compatible chat completion. Returns assistant text."""
    api_key = _resolve_api_key()
    base_url = _resolve_base_url().rstrip("/")
    model = _resolve_model()
    body = json.dumps(
        {
            "model": model,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": prompt},
            ],
            "temperature": 0.0,
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
        except (urllib.error.URLError, urllib.error.HTTPError, KeyError, TimeoutError, json.JSONDecodeError) as exc:
            last_err = exc
            time.sleep(0.5 * (2**attempt))
    raise RuntimeError(f"LLM call failed after {max_retries} attempts: {last_err}")
