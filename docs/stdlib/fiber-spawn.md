# fiber:spawn denseness contract (Issue #2656)

## Return values

| Result | Meaning |
|--------|---------|
| **positive int** | Success — fiber id for `(fiber:join fid)` |
| **`#f`** | Failure (bad args) |
| **error object** | Capability denial under sandbox (`fiber` / `*`) |

Success is **never** `0` or `-1`. Pre-#2656 CLI thread-fallback used
negative ids starting at `-1`, which denseness probes mis-read as failure
even though `(fiber:join -1)` worked.

## Backends

| Backend | When | Probe |
|---------|------|--------|
| **scheduler** | `g_fiber_spawn` set (`--serve-async`, `--serve-async-bench`, serve modes) | `(fiber:spawn-backend)` → `1` |
| **thread** | CLI stdin denseness (`AURA_PIPELINE_STRICT=0` file/stdin runs) | `(fiber:spawn-backend)` → `2` |

Thread fallback is a **supported concurrent denseness backend** for
Hephaestus axis C (multi-worker rebind under load). Real stackful
ucontext fibers require serve-async (epoll).

## Minimal denseness CLI

```bash
# Thread-fallback concurrent path (default stdin / file eval):
AURA_PIPELINE_STRICT=0 ./build/aura -e '(fiber:join (fiber:spawn (lambda () 1)))'
# → 1

# Real scheduler fibers (Linux epoll):
./build/aura --serve-async-bench path/to/script.aura
```

## Sequential-yield surrogate

When spawn is capability-denied or a host deliberately avoids OS threads,
**cooperative sequential-yield fanout** (Aether examples 12 / 17 dual-mode)
remains a valid denseness PASS path for single-worker residual hunting.
It is not a substitute for multi-worker races once thread-fallback spawn
is available.

## Related

- Hephaestus `notes/host-residuals.md` **H9**
- Aether `examples/12-parallel-yield`, dual-mode residuals
- Implementation: `src/compiler/evaluator_primitives_messaging.cpp`
