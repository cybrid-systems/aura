# Evolution log (`aura evo`)

Every self-modification step can be seen, compared and reported with one command (#4910).

## Recording

The runtime appends one JSON object per line to the file named by `AURA_EVO_LOG`:

| source | kind | decision |
|---|---|---|
| any `mutate:*` (outermost call) | the op name, e.g. `mutate:rebind` | `KEEP` when applied, `ROLLBACK` with the refusal (`kind: message`) as reason |
| `ast:restore` | `ast:restore` | `ROLLBACK` (`restored` / `restore failed`) |
| `(evo:record! candidate decision reason evidence [new-src dur-ms parent])` | `record` | your gate's verdict (`keep`/`rollback` are normalized) |

`aura run` inside a project defaults `AURA_EVO_LOG` to `<project>/.aura/evo/log.jsonl`;
set `AURA_EVO_LOG=` (empty) to turn it off. Outside a project nothing is recorded unless
`AURA_EVO_LOG` is set, and `evo:record!` returns `#f`. `(evo:log-path)` returns the
active file. The path is operator configuration only; programs cannot redirect it.

Record fields (`v` = 1): `id`, `parent`, `run`, `seq`, `ts_ms`, `kind`, `target`,
`decision`, `reason`, `evidence` (any JSON value, e.g. `(hash "score" 0.71 "threshold" 0.7)`),
`old` / `new` (define text before/after; `rebind` fills both), `dur_ms`.
A candidate's `parent` is the worldline tip when it was recorded: the last `KEEP`
in the same run (or the explicit `parent` argument). `AURA_EVO_RUN` names the run;
the default is a UTC timestamp plus the pid.

```scheme
(define r (mutate:rebind "strategy" new-src "widen stop"))
(evo:record! "strategy-v7" (if (>= holdout 0.70) "keep" "rollback")
             (format "holdout ~a vs 0.70" holdout)
             (hash "score" holdout "threshold" 0.70 "train" train)
             new-src eval-ms)
```

## Reading

```
aura evo log [--json] [--all] [--run ID|last] [--log FILE]
aura evo report --html DIR [--all] [--run ID|last] [--log FILE]
```

When a run has `evo:record!` entries, those are its candidates: `evo log` and `evo report`
show only them (pass `new-src` so each candidate has a diff); the automatic `mutate:*` /
`ast:restore` records stay in the file and in `--json`, and `--all` shows them too.
`evo log` prints each candidate with its decision, reason, gate evidence, line diff size,
duration and parent; `--json` prints the records as one JSON array.
`evo report` writes `DIR/index.html`, a self-contained page (no network) with the
timeline, the score curve (`evidence.score`, else the first numeric evidence field; the
green line follows the KEEP worldline) and every candidate's source diff. When a record
has no `old`, the diff is taken against the last KEEP'd `new` of the same target.
