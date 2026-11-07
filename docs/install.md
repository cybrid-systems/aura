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
source uses the toolchain image `ghcr.io/cybrid-systems/aura-ci:v0.1.0`.

## Building a bundle yourself

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build aura
scripts/tools/bundle.sh build            # → dist/aura-<commit>-linux-<arch>.tar.gz
```

The workflow is `.github/workflows/dist.yml`: build and test on native x86_64
and arm64 runners, install the bundle on the bare Ubuntu 24.04 runners and in
the clean containers listed above, then publish the release assets and the
image manifest.
