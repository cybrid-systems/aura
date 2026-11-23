# External observability (Issue #5409)

Aura can be observed from outside the process with stock Linux system tools;
no business code, metrics endpoint or logging hook is required.

## Build shape

Release, RelWithDebInfo and MinSizeRel builds add
`-fno-omit-frame-pointer` and (on x86) `-mno-omit-leaf-frame-pointer`.
`aura` is linked with `-rdynamic`, so the ELF symbol table contains the
interpreter/runtime entry points even when LLVM is not found at configure
time. Disable the frame-pointer policy with `-DAURA_FRAME_POINTERS=OFF` if a
build must keep the old prologues.

## JIT names: `/tmp/perf-<pid>.map`

```sh
AURA_PERF_MAP=1 perf record -F 99 -g -- aura --jit program.aura
perf report
```

With `AURA_PERF_MAP=1`, every JIT-compiled Aura function is appended to the
standard perf map as `Lisp:<name>`:

```
7f3c2a001000 1000 Lisp:fib
7f3c2a002000 1000 Lisp:loop
```

Perf and FlameGraph resolve those addresses directly; without the map, JIT
frames appear as anonymous executable pages. The map is opt-in and never
written on a normal run.

## USDT probes

The binary carries SystemTap-compatible probes in `.note.stapsdt`:

| Probe | Fires |
| --- | --- |
| `aura:fiber__spawn` | a fiber is created |
| `aura:fiber__yield` | a fiber yields |
| `aura:fiber__resume` | a fiber is resumed |
| `aura:channel__wait_enter` / `channel__wait_exit` | a channel send/receive parks |
| `aura:gc__start` / `gc__end` | a full GC cycle |
| `aura:http_post__start` / `http_post__end` | an `http-post` call |
| `aura:require_eval__start` / `require_eval__end` | one module load / require |

Attach with bpftrace, for example:

```sh
bpftrace -e 'usdt:/path/to/aura:aura:gc__start { @gc = count(); }'
readelf -n /path/to/aura | grep -A4 stapsdt
```

Without a tracer the probe site is one NOP. On non-Linux targets the macros
compile to nothing, so the same sources keep building.

## Regression

`tests/python/process/test_observability_5409.py` checks the perf-map format
and opt-in behavior, the probe notes in the linked binary, and the exported
runtime symbols.
