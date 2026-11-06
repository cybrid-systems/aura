# Aura

AI-native Lisp — 代码自己进化。

v0.1 is the first version meant to install and run a complete example. It does
not promise a stable API. There is no `v0.1.0` tag and no stable image tag
(`:0.1`, `:0.1.0`, or `:latest`) yet. The default install is the rolling
nightly built from `main`.

## Install

Linux only, x86_64 or arm64. The installer default is nightly; do not set
`AURA_VERSION` to a release tag that has not been published.

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
```

`~/.local/bin` must be on `PATH`. Details, mirrors, and a from-source bundle:
[docs/install.md](docs/install.md).

Nightly tarballs and `ghcr.io/cybrid-systems/aura:nightly` move on every push
to `main`. To reproduce a run, pin the image tag that is the 9-character
commit id (`ghcr.io/cybrid-systems/aura:<sha9>`) or the image digest. The
acceptance baseline recorded for this tree is commit `b988527a0`.

```sh
docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura:nightly main.aura
```

## Quick start

```sh
aura new evolve demo
cd demo
aura run
aura test
```

`aura new` writes a small project (`app`, `evolve`, or `agent`) in this
repository's templates. That is not the
[aura-starter-kits](https://github.com/cybrid-systems/aura-starter-kits)
evolve-strategy kit. The kit lives in that repository and pins an aura
commit; `aura new` does not.

Presets, capabilities, and what `aura run` / `aura test` compare:
[docs/getting-started.md](docs/getting-started.md).

## Supported hosts

x86_64 and arm64, on:

| distro | image used in dist verify |
|---|---|
| Ubuntu 24.04 | `ubuntu:24.04` |
| Debian 12 | `debian:12` |
| Rocky Linux 9 | `rockylinux/rockylinux:9` |
| openEuler 24.03 LTS (SP4) | `openeuler/openeuler:24.03-lts-sp4` |

macOS, Windows, and other Linux distributions are not supported. A green CI
run is not a claim that every host or every program is safe.

## Where things are

| | |
|---|---|
| CI | [`.github/workflows/ci.yml`](.github/workflows/ci.yml) |
| Nightly bundles and images | [`.github/workflows/dist.yml`](.github/workflows/dist.yml) |
| Install | [docs/install.md](docs/install.md) |
| Projects and capabilities | [docs/getting-started.md](docs/getting-started.md) |
| Editor support and the REPL | [docs/editor.md](docs/editor.md) |
| C++ style | [docs/cpp-style.md](docs/cpp-style.md) |
| Contributing | [docs/contributing.md](docs/contributing.md) |
| Starter kits | [cybrid-systems/aura-starter-kits](https://github.com/cybrid-systems/aura-starter-kits) |

## Release channels

| channel | what it is now |
|---|---|
| `nightly` | Replaced on each push to `main`: GitHub Release `nightly`, and `ghcr.io/cybrid-systems/aura:nightly` plus a 9-character commit tag. |
| stable | Not published. A `vX.Y.Z` tag is what would publish `:X.Y.Z`, `:X.Y`, and `:latest`. None of those tags exist yet. |

## Limits

These are limits, not a list of open defects.

- Installing `nightly` does not pin the commit above. A later `main` push
  replaces that release.
- A read interrupted by a signal is retried in the chunk I/O path. Tests
  observe `SIGALRM` and a successful read. They do not prove that `read`
  returned `EINTR`. That is missing test coverage, not a known I/O bug.
- C++ style work tracked in
  [#4936](https://github.com/cybrid-systems/aura/issues/4936) still has
  review and soft items (casts at the JIT/FFI boundary, some `lock_guard`s,
  broader concepts and contracts). Those are not new correctness bugs.
  `Evaluator::prim_registrar()` returns an owning `std::function` because the
  name and primitive are stored; that return type is intentional.
- Behavior outside the four distributions and two architectures above is
  unsupported.
