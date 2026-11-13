# Aura

AI-native Lisp. Your running code evolves itself — every change is trialed, gated, and kept or rolled back.

![A definition is proposed, trialed, then KEEP or ROLLBACK, and the live source changes without restarting](docs/assets/sum-evolve.gif)

[cast](docs/assets/sum-evolve.cast)

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
```

## Try

```sh
aura --preset dev examples/sum-evolve.aura
```

[Getting started](docs/getting-started.md) · [Install](docs/install.md) · [Editor](docs/editor.md)

<sub>The API is not stable. Nightly is the rolling build from `main`. Supported hosts: Ubuntu 24.04, Debian 12, Rocky Linux 9, and openEuler 24.03 LTS SP4, each on x86_64 and arm64.</sub>
