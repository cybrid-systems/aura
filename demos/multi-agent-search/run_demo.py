#!/usr/bin/env python3
"""multi-agent-search demo — control-theoretic concurrent mutation search.

Topology (each wave):
  local catalog agents  ─┐
                         ├── proposals ──► dry-run rank (ΔV filter) ──► single commit
  MiniMax M3 (concurrent)┘

Control:
  V = 10·fail_count + 0.1·node_count + energy
  reject if ΔV > 0.5 (Lyapunov hard filter)
  S = fail_count==0 ∧ size bound

Usage:
  ./build.py build
  export AURA_PIPELINE_STRICT=0

  # Offline (local mutation search only — no API)
  python3 demos/multi-agent-search/run_demo.py --mode offline --waves 5

  # Hybrid / live MiniMax M3
  export AURA_LLM_API_KEY=...   # or MINIMAX_API_KEY
  export AURA_LLM_MODEL=MiniMax-M3
  python3 demos/multi-agent-search/run_demo.py --mode hybrid --waves 5 --n-llm 4
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from agents import fanout_proposals  # noqa: E402
from aura_host import (  # noqa: E402
    AuraRepl,
    fiber_local_fanout_probe,
    install,
    install_seed,
    measure,
    restore,
    snapshot,
)
from common import (  # noqa: E402
    FILTER_DELTA_V,
    SEED_CODE,
    Proposal,
    StepRecord,
    WaveStats,
    energy_of,
    in_S,
    llm_configured,
    save_trajectory,
)


def dry_run_rank(
    repl: AuraRepl,
    props: list[Proposal],
    *,
    old_v: float,
    old_nc: float,
    use_filter: bool,
) -> WaveStats:
    """Sequential dry-run each proposal under snapshot/restore; rank by V."""
    stats = WaveStats(proposals=list(props))
    snap = snapshot(repl)
    ranked: list[tuple[Proposal, float, float]] = []

    for p in props:
        try:
            install(repl, p.code)
        except Exception as exc:  # noqa: BLE001
            stats.install_fails += 1
            restore(repl, snap)
            ranked.append((p, float("inf"), float("inf")))
            p.note = f"{p.note};install-fail:{exc}"[:100]
            continue
        cand_nc = measure(repl, 0.0)[1]
        e = energy_of(old_nc, cand_nc)
        fc, nc, cand_v = measure(repl, e)
        delta = cand_v - old_v
        restore(repl, snap)
        if use_filter and delta > FILTER_DELTA_V:
            stats.rejected_by_filter += 1
            continue
        ranked.append((p, cand_v, delta))

    ranked.sort(key=lambda t: (t[1], t[2]))
    stats.ranked = ranked
    if ranked and ranked[0][1] < float("inf"):
        stats.picked = ranked[0][0]
        stats.delta_v = ranked[0][2]
    return stats


def commit_proposal(repl: AuraRepl, p: Proposal) -> None:
    install(repl, p.code)


def run_search(
    *,
    mode: str,
    waves: int,
    n_local: int,
    n_llm: int,
    max_workers: int,
    use_filter: bool,
    fiber_probe: bool,
    tag: str,
) -> list[StepRecord]:
    repl = AuraRepl()
    records: list[StepRecord] = []
    try:
        install_seed(repl, SEED_CODE)
        energy = 0.0
        fc, nc, v = measure(repl, energy)
        initial_nc = nc
        code = SEED_CODE
        records.append(
            StepRecord(
                step=0,
                V=v,
                fail_count=fc,
                node_count=nc,
                energy=energy,
                rejected=False,
                in_S=in_S(fc, nc, initial_nc),
                action="init",
                picked="seed-deny-all",
                extra=f"mode={mode}",
            )
        )

        if fiber_probe:
            try:
                fr = fiber_local_fanout_probe(repl, 3)
                records[-1].extra += f";fiber_sum={fr}"
            except Exception as exc:  # noqa: BLE001
                records[-1].extra += f";fiber_err={exc}"[:80]

        for wave in range(1, waves + 1):
            if in_S(fc, nc, initial_nc):
                break
            # Already correct on tests: stop even if size soft-miss (shouldn't).
            if fc == 0:
                break

            props = fanout_proposals(
                mode=mode,
                fail_count=fc,
                current_code=code,
                n_local=n_local,
                n_llm=n_llm,
                max_workers=max_workers,
            )
            n_llm_p = sum(1 for p in props if p.source == "llm")
            n_loc_p = sum(1 for p in props if p.source == "local")

            stats = dry_run_rank(repl, props, old_v=v, old_nc=nc, use_filter=use_filter)

            if stats.picked is None:
                records.append(
                    StepRecord(
                        step=wave,
                        V=v,
                        fail_count=fc,
                        node_count=nc,
                        energy=energy,
                        rejected=True,
                        in_S=in_S(fc, nc, initial_nc),
                        action="no-viable-proposal",
                        n_proposals=len(props),
                        n_llm=n_llm_p,
                        n_local=n_loc_p,
                        picked="",
                        extra=(f"rej_filter={stats.rejected_by_filter};install_fail={stats.install_fails}"),
                    )
                )
                continue

            # Single mutator — Lyapunov accept already checked in dry-run.
            old_v, old_nc = v, nc
            snap = snapshot(repl) if use_filter else ""
            try:
                commit_proposal(repl, stats.picked)
            except Exception as exc:  # noqa: BLE001
                if use_filter and snap:
                    restore(repl, snap)
                records.append(
                    StepRecord(
                        step=wave,
                        V=v,
                        fail_count=fc,
                        node_count=nc,
                        energy=energy,
                        rejected=True,
                        in_S=False,
                        action="commit-fail",
                        n_proposals=len(props),
                        n_llm=n_llm_p,
                        n_local=n_loc_p,
                        picked=stats.picked.label,
                        extra=str(exc)[:80],
                    )
                )
                continue

            e = energy_of(old_nc, measure(repl, 0.0)[1])
            fc, nc, v = measure(repl, e)
            # Post-commit safety net: if somehow worse, restore.
            if use_filter and (v - old_v) > FILTER_DELTA_V and snap:
                restore(repl, snap)
                fc, nc, v = measure(repl, energy)
                records.append(
                    StepRecord(
                        step=wave,
                        V=v,
                        fail_count=fc,
                        node_count=nc,
                        energy=energy,
                        rejected=True,
                        in_S=in_S(fc, nc, initial_nc),
                        action="post-commit-rollback",
                        n_proposals=len(props),
                        n_llm=n_llm_p,
                        n_local=n_loc_p,
                        picked=stats.picked.label,
                        extra=f"delta={v - old_v:.3f}",
                    )
                )
                continue

            energy = e
            code = stats.picked.code
            records.append(
                StepRecord(
                    step=wave,
                    V=v,
                    fail_count=fc,
                    node_count=nc,
                    energy=energy,
                    rejected=False,
                    in_S=in_S(fc, nc, initial_nc),
                    action="commit",
                    n_proposals=len(props),
                    n_llm=n_llm_p,
                    n_local=n_loc_p,
                    picked=f"{stats.picked.source}:{stats.picked.label}",
                    extra=(
                        f"delta={stats.delta_v:.3f};"
                        f"rej_filter={stats.rejected_by_filter};"
                        f"candidates={len(stats.ranked)}"
                    ),
                )
            )

        return records
    finally:
        repl.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--mode",
        choices=["offline", "hybrid", "live"],
        default="offline",
        help="offline=local only; hybrid/live add concurrent MiniMax M3",
    )
    ap.add_argument("--waves", type=int, default=5)
    ap.add_argument("--n-local", type=int, default=4)
    ap.add_argument("--n-llm", type=int, default=3)
    ap.add_argument("--max-workers", type=int, default=4)
    ap.add_argument("--filter", choices=["on", "off"], default="on")
    ap.add_argument("--fiber-probe", action="store_true", help="run fiber:spawn sum probe")
    ap.add_argument("--tag", default="", help="results CSV tag suffix")
    ap.add_argument("--trials", type=int, default=1)
    args = ap.parse_args()

    if args.mode in ("hybrid", "live") and not llm_configured():
        print(
            "!! no LLM key — falling back to offline local search (set AURA_LLM_API_KEY or MINIMAX_API_KEY)",
            flush=True,
        )
        args.mode = "offline"

    for trial in range(args.trials):
        tag = args.tag or f"mas_{args.mode}_filt-{args.filter}_t{trial:02d}"
        traj = run_search(
            mode=args.mode,
            waves=args.waves,
            n_local=args.n_local,
            n_llm=args.n_llm,
            max_workers=args.max_workers,
            use_filter=(args.filter == "on"),
            fiber_probe=args.fiber_probe,
            tag=tag,
        )
        path = save_trajectory(traj, tag)
        last = traj[-1] if traj else None
        print(
            f"[{tag}] steps={len(traj)} "
            f"last_V={last.V if last else float('nan'):.3f} "
            f"fail={last.fail_count if last else -1} "
            f"in_S={last.in_S if last else False} "
            f"picked={last.picked if last else ''} "
            f"→ {path}",
            flush=True,
        )
        if last and last.in_S:
            print(
                f"  ✓ reached S in {last.step} wave(s); control: V0={traj[0].V:.3f} → V∞={last.V:.3f}",
                flush=True,
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
