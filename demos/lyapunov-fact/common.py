"""Common types + V definition for the lyapunov-fact demo.

V (Lyapunov-like) is identical on both sides per spec:
    V = 10 * test_fail + 2 * recursive_residual + 0.1 * node_count + energy

S (target set) per spec:
    test_fail == 0 AND recursive_residual == 0 AND node_count <= 1.3 * initial
"""

import csv
import json
import os
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path

# Results are written to <repo>/demos/lyapunov-fact/results/.
# Resolve relative to this file so the demo runs the same way regardless
# of CWD (callers usually invoke from the repo root).
RESULTS = Path(__file__).resolve().parent / "results"
RESULTS.mkdir(parents=True, exist_ok=True)


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
    extra: str = ""  # free-form tag (e.g. LLM model, prompt size)


def save_trajectory(records: list[StepRecord], tag: str) -> Path:
    """Write trajectory to a CSV. Returns the output path."""
    out = RESULTS / f"{tag}.csv"
    if not records:
        # Always write a header so analyze.py can glob over empty results.
        with open(out, "w", newline="") as f:
            f.write("step,V,test_fail,recursive_residual,node_count,energy,rejected,in_S,action,extra\n")
        return out
    with open(out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(asdict(records[0]).keys()))
        writer.writeheader()
        for r in records:
            writer.writerow(asdict(r))
    return out


def V_value(test_fail: int, recursive_residual: int, node_count: float, energy: float) -> float:
    """Proxy V — identical on both sides."""
    return 10.0 * test_fail + 2.0 * recursive_residual + 0.1 * node_count + energy


def in_S(test_fail: int, recursive_residual: int, node_count: float, initial_size: float) -> bool:
    """S — identical on both sides."""
    return test_fail == 0 and recursive_residual == 0 and node_count <= 1.3 * initial_size


# --- shared LLM client (used by exp2_llm/) -----------------------------
# Env-var lookup order per spec: AURA_LLM_API_KEY → OPENAI_API_KEY →
# GROK_API_KEY → DEEPSEEK_API_KEY (or any first non-empty). Default base
# URL is OpenAI-compatible; the model is whatever AURA_LLM_MODEL says
# (or "grok-4" if unset). We use the stdlib `urllib` to keep the demo
# zero-dependency.


def _resolve_api_key() -> str:
    for var in ("AURA_LLM_API_KEY", "OPENAI_API_KEY", "GROK_API_KEY", "DEEPSEEK_API_KEY"):
        v = os.environ.get(var)
        if v:
            return v
    raise RuntimeError("No LLM API key found in env (set AURA_LLM_API_KEY)")


def _resolve_base_url() -> str:
    v = os.environ.get("AURA_LLM_BASE_URL")
    if v:
        return v
    # default: OpenAI-compatible public endpoint (used by minimax-m3
    # at ~/code/keys/minimax per the user's instruction).
    return "https://api.minimax.chat/v1"


def _resolve_model() -> str:
    v = os.environ.get("AURA_LLM_MODEL")
    if v:
        return v
    return "grok-4"


def llm_chat(
    prompt: str, *, system: str = "You are a code refactorer.", max_retries: int = 3, timeout_s: float = 30.0
) -> str:
    """OpenAI-compatible chat completion. Returns the assistant text.

    The minimal demo client uses urllib + a short retry loop. It does
    not stream. A real production client would also pin the TLS
    fingerprint and use a longer timeout, but for the demo we keep the
    surface area small.
    """
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
            return data["choices"][0]["message"]["content"]
        except (urllib.error.URLError, urllib.error.HTTPError, KeyError, TimeoutError) as exc:
            last_err = exc
            time.sleep(0.5 * (2**attempt))
    raise RuntimeError(f"LLM call failed after {max_retries} attempts: {last_err}")
