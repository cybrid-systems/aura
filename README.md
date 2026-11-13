# Aura

AI-native Lisp. Your running code evolves itself — every change is trialed, gated, and kept or rolled back.

![sum-to is rewritten in one process: a wrong fast body is rolled back, a correct fast body is kept](docs/assets/sum-to.gif)

[recording](docs/assets/sum-to.cast) · [re-record](scripts/record_demo.sh)

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
```

## Try

```sh
aura --preset dev examples/sum-to.aura
```

[Getting started](docs/getting-started.md) · [Install](docs/install.md) · [Editor](docs/editor.md)

<sub>Nightly builds from `main`; the API is not stable. Linux x86_64 and arm64 — Ubuntu 24.04, Debian 12, Rocky Linux 9, openEuler 24.03 LTS SP4. macOS and Windows are not supported.</sub>
