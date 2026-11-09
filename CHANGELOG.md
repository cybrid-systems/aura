# Changelog

## 0.1.2

Patch release on the 0.1 channel. The API is still not stable; this version installs and runs the shipped examples.

- The REPL is an editing surface. `:focus` names the definition being edited; `:src` prints its live source; `:propose EXPR` rewrites it behind a keep-or-rollback `:gate` with a four-line before/after diff; `:undo`, `:redo` and `:at N` walk that definition's worldline. `:context` reports the focused source, the last error and the worldline, and `:ask` feeds a model answer into `:propose` when a model is configured. A proposal budget (`:fuel`, default 100000) stops a runaway loop, and Ctrl-C rolls the current proposal back.
- The REPL records one evaluation per form on a pipe: one record line per form with `status/value/type/wrote/focus`, a refused form is a record instead of an empty line, and an open phrase continues on a `..` prompt on a TTY. `:doc`, `:type` and the grey hint answer from the primitive table; Tab completes the word at the cursor; readline emacs keys and Ctrl-R history search work over the extracted edit core; an error value prints its cause and marks the failing column with a caret.
- `tcp-listen` accepts an optional IPv4 host: `(tcp-listen port)` still binds 127.0.0.1, and `(tcp-listen port host)` takes a literal such as `"0.0.0.0"`.
- An exhausted EnvFrame arena is a catchable `OutOfMemory` diagnostic instead of `abort()`: a program holding more than 1,048,576 live capture frames gets `error: out of memory: EnvFrame arena exhausted (...)`, and an Aura `(try ... (catch (e) ...))` can handle it. File mode reclaims dead capture frames from a live EnvId root set (including every parked fiber's Envs and its cap-snapshot chain), so the arena budget tracks live frames. `(engine:metrics)` exposes live frames, high water, freelist hits/misses and reclaimed frames.
- A cell store that landed while another body grew the cell vector could be lost, leaving a named-let binding unreadable (`cannot call: loop`). The store now serializes with the growth.
- Type checker: `(if (and ...))` and `(format ...)` stop producing false Bool/String warnings in the soft REPL; a Dynamic argument is a checked boundary, not a type error; the check-mode pass keeps an `if` condition; a type-var callee keeps the gradual Dynamic return (#4316); a recursive definition is installable at runtime again (#5052).
- Parser: a malformed character literal in an `if`/lambda operand no longer hangs the reader (#5051), and a formatted parse error keeps its message.
- Orchestration: an agent body waits for the spawn to publish its handle (#5053), and the orchestration scheduler counters are observable outside `--serve-async` (#5054).
- Install and smoke coverage is unchanged: Ubuntu 24.04, Ubuntu 26.10, Debian 12, Rocky Linux 9, and openEuler 24.03 LTS SP4, on x86_64 and arm64.
- `AURA_VERSION=v0.1.2` selects the GitHub Release asset `aura-0.1.2-linux-<arch>.tar.gz`; the default install remains nightly. Images: `ghcr.io/cybrid-systems/aura:0.1.2`, `:0.1`, and `:latest`. `:nightly` stays the rolling build from `main`.
- A binary whose version is `X.Y.Z` makes `aura new` pin that channel. `0.1.2` writes `ghcr.io/cybrid-systems/aura:0.1.2`. `dev` and `nightly` write `:nightly`. `:latest` is never written into a new project.
- A tag release runs the chaos SOAK (300s, 8 workers, 256 fibers) and the production concurrency gate before assets are uploaded.

Not in this version:

- macOS, Windows, and other Linux distributions.
- The evolve-strategy kit is not pinned to this tag yet.

## 0.1.1

Patch release on the 0.1 channel. The API is still not stable; this version installs and runs the shipped examples.

- `aura demo` runs the evolve example in one command: one definition, four fixed candidates, and the live source after each KEEP or ROLLBACK. `aura demo agent` is the two-role version (a proposer and a checker); `aura new evolve demo` is the same program as an editable project.
- The `aura repl` banner prints the binary's own version and the product tagline instead of a hard-coded `Aura v0.2`: the same `AURA_BUILD_VERSION` token `aura --version` reports and the same `an AI-native Lisp` line `aura --help` prints. Piped `aura repl` still prints no banner.
- The REPL prints the cause of an error value instead of a bare `<error>`: `(car 5)` prints `car: not a pair`, and an ungranted `set-code` prints `effect-denied: mutate not granted tenant=0 op=set-code`. It is still one stdout line per form with nothing on stderr.
- Bare `aura` on a TTY is the same interactive face as `aura repl`: the same soft session and grant line, the same std prelude, the same `dev` preset default. `(set-code ...)` succeeds there and `std/list` names resolve. A piped bare `aura` keeps the oneshot face.
- C++26 / Core Guidelines modernization (#4936): a single mutex is now a hard `std::scoped_lock` rule (P6), address-sized FFI/JIT casts go through `ffi_addr.hh`, and the remaining R4 / T7 / F2 / F6 / P2 / P3 items stay review-level guidance in `docs/cpp-style.md`.
- Install and smoke coverage is unchanged: Ubuntu 24.04, Ubuntu 26.10, Debian 12, Rocky Linux 9, and openEuler 24.03 LTS SP4, on x86_64 and arm64.
- `AURA_VERSION=v0.1.1` selects the GitHub Release asset `aura-0.1.1-linux-<arch>.tar.gz`; the default install remains nightly. Images: `ghcr.io/cybrid-systems/aura:0.1.1`, `:0.1`, and `:latest`. `:nightly` stays the rolling build from `main`.
- A binary whose version is `X.Y.Z` makes `aura new` pin that channel. `0.1.1` writes `ghcr.io/cybrid-systems/aura:0.1.1`. `dev` and `nightly` write `:nightly`. `:latest` is never written into a new project.
- A tag release runs the chaos SOAK (300s, 8 workers, 256 fibers) and the production concurrency gate before assets are uploaded.

Not in this version:

- The REPL answers each form with one stdout line and still represents a refused form with an empty line; the per-form record is #5043.
- macOS, Windows, and other Linux distributions.
- The evolve-strategy kit is not pinned to this tag yet.

## 0.1.0

First stable channel. The API is not stable. This version installs and runs the shipped examples.

- Linux x86_64 and arm64. `AURA_VERSION=v0.1.0` selects the GitHub Release asset `aura-0.1.0-linux-<arch>.tar.gz`. The default install remains nightly.
- Images: `ghcr.io/cybrid-systems/aura:0.1.0`, `:0.1`, and `:latest`. `:nightly` stays the rolling build from `main`.
- A binary whose version is `X.Y.Z` makes `aura new` pin that channel. `0.1.0` writes `ghcr.io/cybrid-systems/aura:0.1.0`. `dev` and `nightly` write `:nightly`. `:latest` is never written into a new project.
- Install and smoke coverage is Ubuntu 24.04, Ubuntu 26.10, Debian 12, Rocky Linux 9, and openEuler 24.03 LTS SP4, on x86_64 and arm64.
- A tag release runs the chaos SOAK (300s, 8 workers, 256 fibers) and the production concurrency gate before assets are uploaded.

Not in this version:

- C++ Core Guidelines remediation is unfinished (#4936). These rules are still soft, not hard: R4 (special members), T7 (initialize locals), F2 (parameter passing), F6 (pointer-keyed iteration order), P2 (needless copies), P3 (`noexcept` move and `swap`), P6 (`std::scoped_lock`).
- macOS, Windows, and other Linux distributions.
- The evolve-strategy kit is not pinned to this tag yet.
