# Installing aura

Prebuilt aura for Linux on x86_64 and arm64 (aarch64) is published from every
push to `main` (the rolling `nightly` release) and from every `v*` tag.

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
aura demo

The first command prints the signature record; each line is one rewrite of the
live definition:

    1 ROLLBACK  (define (guess) 10)   live (define (guess) 0)
    2 KEEP      (define (guess) 7)    live (define (guess) 7)
    3 ROLLBACK  (define (guess) 2)    live (define (guess) 7)
    4 KEEP      (define (guess) 42)   live (define (guess) 42)
    (guess) => 42
```

`install.sh` picks the tarball for `uname -m`, checks its sha256, unpacks it
under `~/.aura/<version>` and links `~/.local/bin/aura`. Settings:

| variable | default | meaning |
|---|---|---|
| `AURA_VERSION` | `nightly` | release tag to install (`nightly` or `vX.Y.Z`) |
| `AURA_PREFIX` | `~/.aura` | where the bundle is unpacked |
| `AURA_BIN_DIR` | `~/.local/bin` | where the `aura` link goes |
| `AURA_DOWNLOAD_BASE` | the GitHub release URL | a mirror, or a local directory holding the tarballs |

## From the install line to a report

The v0.2 search demo is the end-to-end path: install, scaffold `evolve-moo`,
run 240 mock proposals (no LLM, no network) and write the report.

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
export PATH="$HOME/.local/bin:$PATH"
aura new evolve-moo demo && cd demo
aura run                      # evaluated 240  front 30  qd coverage ...
aura evo report --html out/report
```

Measured on 2026-11-09 with a fresh `$HOME` under `mktemp -d`, the public
install line and the `nightly` tarball (40 MB), twice:

| step | command | run 1 | run 2 |
|---|---|---|---|
| download + install | `curl … \| sh` | 53.9 s | 22.6 s |
| scaffold | `aura new evolve-moo demo` | 0.01 s | 0.01 s |
| search | `aura run` | 0.8 s | 0.8 s |
| report | `aura evo report --html out/report` | 0.02 s | 0.01 s |
| **install → report** | | **54.7 s** | **23.4 s** |

Host: Ubuntu 26.10, aarch64. The download dominates and the two runs differ
only there (cold vs warm network); the demo itself is under a second. The
budget is 10 minutes (#4948).

`aura run` printed `evaluated 240  front 30  qd coverage 0.953125`, and the
report wrote `index.html`, `pareto.svg`, `hv-curve.svg`, `qd-heatmap.svg`
and `lineage.svg`.

**Platform coverage.** Ubuntu 24.04 on x86_64 and on arm64 is checked by
`.github/workflows/dist.yml` (`verify` job) on every push: it installs the
bundle on both bare runners, then scaffolds `evolve-moo`, runs it, writes the
report and fails if install → report exceeds 600 s. That cell has not been
observed on this host (arm64, no container engine): the Ubuntu 24.04/x86_64
number comes from that CI job once it runs, not from a local measurement.

**Host packages.** None beyond a stock Ubuntu 24.04 image: `install.sh` uses
`sh`, `tar`, `gzip`, `sha256sum` and `curl` (or `wget`), plus
`ca-certificates` for the HTTPS download. The bundle carries its own glibc, so
no compiler, Python or C++ runtime is needed.

## What runs where

Each tarball carries its own dynamic loader, glibc and libstdc++ (`rt/`), and
`bin/aura` is a small launcher that starts the real binary (`libexec/aura`) on
them, so it does not depend on the host glibc. On every `main` push and
release tag, the dist workflow runs each image below on the x86_64 runner and
again on the arm64 runner (the bare host is Ubuntu 24.04; these are clean
stock containers):

| distro | image |
|---|---|
| Ubuntu 24.04 | `ubuntu:24.04` |
| Ubuntu 26.10 | `ubuntu:26.10` |
| Debian 12 | `debian:12` |
| Rocky Linux 9 | `rockylinux/rockylinux:9` |
| openEuler 24.03 LTS (SP4) | `openeuler/openeuler:24.03-lts-sp4` |

Ubuntu 24.04 passing does not by itself mean Ubuntu 26.10 works. macOS,
Windows, and any distro not in the table are not supported.

`install.sh` needs only `sh`, `tar`/`gzip`, `sha256sum` and `curl` or `wget`. It needs Linux 3.2 or newer. macOS and Windows are not supported; use the image.

## Container image

```sh
docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura:nightly main.aura
```

`ghcr.io/cybrid-systems/aura` is a multi-arch image (linux/amd64, linux/arm64)
holding only the bundle on `ubuntu:24.04`. Tags: `nightly` (newest `main`),
the 9-character commit id, and for each `vX.Y.Z` release `X.Y.Z`, `X.Y` and
`latest` (`latest` is only ever set by a release tag). Compiling aura from
source uses the toolchain image `ghcr.io/cybrid-systems/aura-ci:v0.1.1`.

## Building a bundle yourself

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build aura
scripts/tools/bundle.sh build            # → dist/aura-<commit>-linux-<arch>.tar.gz
```

The workflow is `.github/workflows/dist.yml`: build and test on native x86_64
and arm64 runners, install the bundle on the bare Ubuntu 24.04 runners and in
the clean containers listed above, then publish the release assets and the
image manifest.
