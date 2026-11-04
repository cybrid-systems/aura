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
them. The host glibc therefore does not matter: the bundle runs on Ubuntu 22.04
(glibc 2.35), 24.04 (2.39), Debian 12 and newer, on both architectures. It
needs Linux 3.2 or newer. macOS and Windows are not supported; use the image.

## Container image

```sh
docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura main.aura
```

`ghcr.io/cybrid-systems/aura` is a multi-arch image (linux/amd64, linux/arm64)
holding only the bundle on `ubuntu:24.04`. Tags: `nightly` / `latest` (newest
`main`) and the 9-character commit id. The toolchain image
`ghcr.io/cybrid-systems/dev` is only needed to build aura from source.

## Building a bundle yourself

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build aura
scripts/tools/bundle.sh build            # → dist/aura-<commit>-linux-<arch>.tar.gz
```

The workflow is `.github/workflows/dist.yml`: build and test on native x86_64
and arm64 runners, install the bundle on the bare runners and in `ubuntu:22.04`,
then publish the release assets and the image manifest.
