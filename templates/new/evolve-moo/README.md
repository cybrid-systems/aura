# {{name}}

A multi-objective + quality-diversity search, ready to run with no LLM and no
network (the proposer is deterministic from the seed).

    aura run                       # 240 mock proposals, Pareto front + QD map
    aura evo report --html out/report
    aura test

`aura run` prints the evaluated count, the Pareto front size and the QD
coverage, and writes `out/pareto.svg` and `out/qd.svg`. The evo log records
every candidate's objectives and behaviour, so `aura evo report` adds the
Pareto scatter, the hypervolume curve, the QD heat map and the lineage tree.
Replace `lib/moo.aura`'s evaluator or the proposer in `main.aura` to make it
your own problem.
