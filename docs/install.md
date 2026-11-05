# Installing aura

Prebuilt aura for Linux on x86_64 and arm64 (aarch64) is published from every
push to `main` (the rolling `nightly` release) and from every `v*` tag.

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
aura new evolve demo && cd demo && aura run
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
them, so it does not depend on the host glibc. Every `main` push is verified on
Ubuntu 24.04 (glibc 2.39), on both x86_64 and arm64; that is the supported
host. It needs Linux 3.2 or newer. macOS and Windows are not supported; use the image.

## Container image

```sh
docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura:nightly main.aura
```

`ghcr.io/cybrid-systems/aura` is a multi-arch image (linux/amd64, linux/arm64)
holding only the bundle on `ubuntu:24.04`. Tags: `nightly` (newest `main`),
the 9-character commit id, and for each `vX.Y.Z` release `X.Y.Z`, `X.Y` and
`latest` (`latest` is only ever set by a release tag). The toolchain image
`ghcr.io/cybrid-systems/dev` is only needed to build aura from source.

## Building a bundle yourself

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build aura
scripts/tools/bundle.sh build            # → dist/aura-<commit>-linux-<arch>.tar.gz
```

The workflow is `.github/workflows/dist.yml`: build and test on native x86_64
and arm64 runners, install the bundle on the bare Ubuntu 24.04 runners and in a clean `ubuntu:24.04` container,
then publish the release assets and the image manifest.
