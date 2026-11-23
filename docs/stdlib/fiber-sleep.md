# fiber:sleep and std/schedule — timers for the personal runtime (#5414)

`(fiber:sleep ms)` is the kernel wait the personal runtime (`aura-agents#37`
/ `#39`) was missing. Until #5414 the only wait in `lib/std` was a yield
loop on the steady clock (`orch-wait-sec`). That is not a sleep — on the
thread backend `fiber:yield` is a no-op, so the loop spins its thread — and
nothing could keep an alarm across a restart. `std/schedule` is the timer
and the persisted alarm built on the primitive.

## The primitive

```aura
(fiber:sleep 250)          ; → #t after 250 ms
```

| Result | Meaning |
|--------|---------|
| `#t` | the whole interval elapsed |
| error | the argument is not an integer `0 <= ms <= 86400000`, or the `fiber-cancelled` condition |

Not capability-gated, like `fiber:yield` and `fiber:join`. The wait is
measured on the steady clock: a wall-clock change does not move it.

### What the wait is, per backend

| Called from | Wait | Wake |
|-------------|------|------|
| a scheduler fiber that may park: `--serve-async`, `--serve-async-bench`, an orch agent body under `--load`; not inside a live mutation boundary | a **timed park**: the fiber goes `Waiting` with a park deadline and leaves its worker. The IO loop shrinks its `epoll_wait` to the nearest deadline and re-queues the fiber when it passes | on the deadline: whole milliseconds plus the worker's dispatch |
| anything else: a CLI thread-backend body (`aura file`, `--load`, `--serve`), the host thread, a fiber inside a mutation boundary (a yield is refused there) | the **OS thread sleeps in slices of at most 20 ms** | on the deadline, to within the slice; the thread is blocked meanwhile |

The park is the mailbox-receive protocol (`set_park_deadline_ms` +
`Fiber::yield(Explicit)`), so it composes with the steal walk and the GC
safepoint the same way.

### Cancellation

`fiber:sleep` is a cancellation safepoint ([fiber-cancel.md](fiber-cancel.md)).
A request that is already pending answers before the wait starts; one that
lands during the wait raises the catchable condition in the sleeping body:

- a parked fiber is nudged through its eventfd at once (measured: 1 ms to
  the joined condition);
- a slice-sleeping thread answers at the end of its slice (20 ms at most).

A cancelled body never reaches what follows its sleep. A body inside a
mutation boundary blocks its worker for the whole interval: keep sleeps out
of a mutation scope.

### The scheduler no longer abandons a parked sleep

`Scheduler::run` stops itself after three idle ticks. A fiber parked on a
deadline is in no queue and wakes nobody, so it used to count as idle: a
sleep longer than about three seconds was abandoned once every other fiber
was done. A tick that saw an armed park deadline is not an idle tick any
more. This also covers a timed mailbox receive (`agent-recv` with a
timeout) on an otherwise idle scheduler.

## std/schedule

```aura
(require "std/schedule" all:)

; in memory only
(define s (schedule:make (hash)))
(define t (schedule:after s 60000 take-break))     ; once, 60 s from now
(schedule:at s (+ (current-time-ms) 3600000) report)
(schedule:cancel s t)                               ; → #t; take-break never runs
(schedule:pending s)                                ; armed timers, earliest first

; persisted across restarts
(define p (schedule:open "~/.aura-runtime/schedule"
                         (hash "on-due" (lambda (timer) ...))))
```

A timer is `(hash "id" "due" "payload" "state")`: `due` is epoch
milliseconds, `payload` is the optional fourth argument of
`schedule:after` / `schedule:at`, `state` is `armed`, `fired`, `done` or
`cancelled`. `(schedule:cancel s timer)` takes the timer or its id.

**A timer runs on its own fiber.** `(fiber:sleep ms)` then the thunk. A
cancel joins that fiber before it returns, so the thunk has either run or
never will. A thunk that throws is the timer fiber's result (visible to
nobody unless you keep the fiber); the timer stays `fired`.

### The alarm directory

`"dir"` makes a schedule persistent: **one JSON file per alarm**,
`<dir>/<id>.json`, holding `id`, `due`, `payload` and `state`.

| moment | file |
|--------|------|
| armed | written (`armed`) before the timer's fiber starts |
| firing | rewritten (`fired`) before the handler runs |
| ran | deleted after the handler returns |
| cancelled | deleted after the fiber was joined |

A restart therefore finds exactly the alarms that did not run.
`(schedule:open DIR (hash "on-due" fn))` reads the directory and, **before
it returns**, hands every overdue `armed` alarm and every `fired` alarm to
`fn` (the timer hash, payload included); a future alarm is re-armed to call
`fn` at its time. Without an `on-due` nothing is delivered and nothing is
lost: the alarms wait for `(schedule:on-due! s fn)`.

**Delivery is at-least-once.** A crash or a throwing handler between
"fired" and "ran" leaves a `fired` file, and the next `schedule:open` hands
it to `on-due` again. A timer keeps its id, so a handler can dedupe.

**A file that is not an alarm is a refusal.** `schedule:open` raises and
names the file — a torn write, a foreign file, or an id that does not match
its file name (recovery rewrites and deletes by id) — instead of dropping
it or guessing.

**Why one file per alarm.** Nothing is written from two threads: the
caller's thread writes when it arms, the timer's own fiber rewrites and
deletes when it fires, and cancel joins that fiber first
([fiber-spawn.md](fiber-spawn.md)). The in-memory state is an atomic
counter.

### Shutdown

A running fiber costs the `~Evaluator` drain (about two seconds, then a
warning). `(schedule:shutdown s)` cancels and joins every armed timer's
fiber and **keeps the alarm files**, so a process with pending alarms exits
at once and the next `schedule:open` finds them. An in-memory timer is lost
with the process; there is nothing to recover it from.

### Grants

Persistence uses `write-file`, `file-delete` and `make-directory`, which
need the write effect. Under a sandbox without it `schedule:after` /
`schedule:at` answer the refusal message (the string
`effect-denied: write not granted ...`) instead of a timer and arm nothing:
pass `--grant write:DIR`. A directory is read with the read effect. An
in-memory schedule needs no grant beyond what `hash-set!` needs.

### For the downstream runtime (aura-agents#39)

The runtime owns the loop and the handlers; this module owns timers and
alarm files. The contract:

- arm with `schedule:after` / `schedule:at` and a JSON payload; the handler
  for a recovered alarm is `on-due`, keyed by the payload (the thunk is gone
  after a restart);
- call `schedule:shutdown` before the process exits, and `schedule:open`
  with the same directory and an `on-due` when it starts again;
- treat a `schedule:open` refusal as a broken alarm directory: nothing was
  delivered.

## What moved onto fiber:sleep (#5414)

| Site | Before | Now |
|------|--------|-----|
| `std/orch` `orch-wait-sec` | yield loop on the monotonic clock | `(fiber:sleep ms)` |
| `std/orch` `orch:ask` reply poll | `channel:try-recv` in a `fiber:yield` loop | the same poll, 1 ms of sleep between tries |
| `std/portfolio` member watchdog | yield loop against the caller's clock | the same deadline, sleeps of at most 20 ms of the caller's clock |
| `std/race` default `"sleep"` | yield loop | `(fiber:sleep ms)` |

The scheduler files that back the park: `src/serve/fiber.h`
(`set_park_deadline_ms`), `src/serve/scheduler.cpp` (the deadline scan and
the idle rule); the primitive: `src/compiler/evaluator_primitives_messaging.cpp`.

## Acceptance evidence

- `tests/suite/issue_5064_fiber_cancel_text.aura` (thread backend): the
  interval and `#t`, the cancel condition inside a minute-long sleep, the
  argument refusals.
- `tests/suite/fiber_sleep_park.aura` (`;; suite-mode: serve-async`,
  scheduler backend): a parked sleep wakes and leaves the worker to a
  shorter sleeper, a parked sleep is cancelled at once, a 3.5 s sleep
  outlives the idle window.
- `tests/suite/schedule.aura`: fires once; cancel never fires; `at`; the
  alarm file written and removed; a restart hands overdue and `fired`
  alarms over exactly once and re-arms the rest; a throwing handler's alarm
  is handed over again; a torn file and a mismatched id are refusals;
  `shutdown` keeps the files.

## Limits

- Whole milliseconds; no sub-ms timer, and the park's wake is only as
  precise as the IO loop's tick.
- A timer is a sleeping fiber: one fiber per pending alarm.
- `schedule:open` delivers overdue alarms on the calling thread; a slow
  `on-due` slows the open.
- The alarm directory is for one process. Two processes arming into the
  same directory at the same millisecond can pick the same id.
