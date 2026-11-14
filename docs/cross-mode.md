# Cross-mode consistency gate

A program gives the same result however aura runs it. The gate
`tests/python/process/xmode_corpus.py` enforces that (#4908). It
runs in the CI `build-test` job, in the step "Cross-mode gate + DX
contracts" (`python3 tests/run.py process --tier fast`).

## What it compares

Every program in `tests/cross_mode/*.aura` runs on six faces: the three
pipelines `AURA_PIPELINE_STRICT` unset (default), `0` and `force-soa`,
each both directly (`aura prog.aura`) and with `--load`. Every face runs
with `AURA_SANDBOX=off`.

For each face the gate compares three things against the default/direct
face:

- stdout
- the exit code
- the `line:col` of the first `error:` line on stderr

Any difference fails the gate and is printed as a unified diff.

The corpus is the minimal repro of each split users have hit:

| Programs | Issue |
|---|---|
| `i4899_*`, `i4899f_*` | #4899 |
| `i4901_*` | #4901 |
| `i4903_*` | #4903 |
| `i4904_*` | #4904 |

### Kit leg

Set `AURA_STARTER_KITS=<aura-starter-kits checkout>` to also run
`kits/evolve-strategy` (`KIT_STEPS`, default 20) on the same six faces
and compare the `summary.json` files. CI clones the kits' latest `main`
for this, not the `AURA_PIN` the kits use.

## Run it locally

```sh
python3 tests/python/process/xmode_corpus.py
AURA_STARTER_KITS=../aura-starter-kits python3 tests/python/process/xmode_corpus.py
```

## Allowlist: intended differences

`tests/cross_mode/allowlist.txt` lists the differences that are known
and accepted, one per line:

```
<program glob> <face glob> <field> <reason...>
```

- `face` is one of `default|strict0|force-soa`, then `/direct` or
  `/load`. `*` matches any face.
- `field` is `stdout`, `rc`, `loc` or `*`.

Current entries:

| Programs | Faces | Field | Why |
|---|---|---|---|
| `i4899f_*.aura` | `force-soa/*` | `loc` | An error raised inside a procedure body that force-soa runs on the IR is located at the top-level call site, not at the failing expression in the body. stdout and rc still match. The IR instruction's source node id belongs to the defining form's flat, but the producer lookup resolves it against the calling form's flat. (#4909 follow-up) |

Sandbox denials are not part of this gate, because the corpus runs with
the sandbox off. The sandbox faces (off / default Restricted / soft) are
compared by the starter kits' `scripts/mode-matrix.sh` (15 cells; `aura
soft` has no `--load`). On the published image
`ghcr.io/cybrid-systems/aura:7002d30ab` (main `7002d30ab`, which contains
the #4928 fix and does not yet contain the later #4937 restores), that
matrix matched the sandbox-off / default-pipeline summary in every cell,
including `unset-force-soa-direct`, `unset-force-soa-load`, and
`soft-force-soa-direct`. The published kits workflow still sets
`MATRIX_KNOWN_RED` to those three cells. A local starter-kits commit
(`2d2eb7ff`, pin `7002d30ab64234738ab88ebe7822e83279c53e99`, allowlist
removed) was not pushed: this bot gets 403 on
`cybrid-systems/aura-starter-kits`. `continue-on-error` is already absent
from that workflow. The same image still aborts the #4937
rebind/snapshot/`member` repro under `preset=dev`. Current main has those
restores; no newer runtime image tag was published at the time of this note.

To add an entry, give the reason and the issue that will remove it. A
new difference with no entry fails CI.
