# Getting started: projects and capabilities (#4912, #4913)

## The first run

    aura demo

prints the signature record: a definition is written into the live AST, kept
or rolled back, and the live source after each decision.

    1 ROLLBACK  (define (guess) 10)   live (define (guess) 0)
    2 KEEP      (define (guess) 7)    live (define (guess) 7)
    3 ROLLBACK  (define (guess) 2)    live (define (guess) 7)
    4 KEEP      (define (guess) 42)   live (define (guess) 42)
    (guess) => 42

`aura demo agent` prints the two-role dialogue. The same evolve program is a
project you can edit:

    aura new evolve demo      # or: app (modules + tests), evolve-moo
                              # (multi-objective + QD), agent (two roles)
    cd demo
    aura run

`aura new` writes `aura.toml`, the program, `tests/` with golden files, a
`.github/workflows/ci.yml` and a README. In a project:

| command | what it does |
|---|---|
| `aura run [entry.aura]` | runs `entry` from `aura.toml` (default `main.aura`) with the project root as cwd and the project's `preset` |
| `aura test` | runs every `tests/*.aura` and compares stdout with the `.golden` beside it; `--update` rewrites the goldens |
| `(require "lib/x")` | resolves from the project root; `"./x"` stays relative to the requiring file |

`aura.toml` keys read today: `entry`, `preset`, `out`. Its `[dependencies]`
table records `aura = "<channel>"`: the aura release the project was made with
(`nightly` from a nightly or local build, `X.Y.Z` from a release). The generated
CI runs `ghcr.io/cybrid-systems/aura:<channel>`, so it works before any stable
release exists; bump both lines together to move the project to a release.
Modules are files in the project (`require` resolves from the root) and `std/*`
ships with aura; there is no package registry yet.

## Capability presets

The default sandbox is Restricted: a program can read, but it cannot rewrite
its live AST (`mutate`) or write files. Pick a preset instead of turning the
sandbox off:

| scenario | run with |
|---|---|
| a self-evolving program on your machine (`set-code`, `mutate:rebind`, `ast:restore`, `write-file`) | `--preset dev` |
| the same program in CI | `--preset ci` (like dev, but files may be written only under `out/`) |
| a service / untrusted code | `--preset prod` (the Restricted default; takes no `--grant`) |
| only one extra capability | `--grant write` (or `mutate`, `ffi`, `exec`, `network`) |
| write only under some directories | `--grant write:out/` (repeatable: `--grant write:out/,write:data/`) |
| dev minus something | `--preset dev --deny write` |

The preset can also come from `AURA_PRESET=dev` or `preset = "dev"` in
`aura.toml` (the flag wins, then the env var, then the file). Every run with a
preset prints one line naming the effective capabilities, for example

    aura: preset=dev sandbox=restricted caps=mutate,read,write (tenant=0, ws=0)

With `--preset ci` or `--grant write:DIR/`, `write-file`, `make-directory`,
`file-copy` (destination), `file-delete`, `serialize-workspace` and
`generate-type-sigs` must stay under the listed directories (for `ci`: the
`out` key of `aura.toml`, default `out/`, relative to the directory the run
starts in; `aura run` starts in the project root). The path is resolved first,
so `out/../x` and a symlink inside `out/` that points elsewhere are refused:

    effect-denied: write-path not granted for x.txt (writable: out/) op=write-file

and the banner shows the scope, e.g. `caps=mutate,read,write:out/`. A bare
`--grant write` (or `--preset dev`) lifts the scope.

`mutate:rebind` replaces a live workspace define by name. A name that is
not in the workspace is **added**: a new top-level define is linked in and
the call returns `#t`, the same as a successful replace. It does not mean
"not found". Defines that arrived only from a file, `-e`, or `--load` are
not in the live AST until `(set-code ...)` and `(eval-current)`; rebind
then returns `no-workspace` rather than a usage error.

`--quiet` drops it. `macro-self-evo`, `tenant-admin` and `syscall` are never
granted from the command line; they remain host-side grants (#4575).
`AURA_SANDBOX=off` still turns every check off, for local debugging only.
