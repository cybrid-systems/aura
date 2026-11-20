# Aura

AI-native Lisp — 代码自己进化: running code proposes new source, `evo:trial` keeps or rolls it back, no restart.

Live showcase: [Lumen Run](https://github.com/cybrid-systems/aura-runner).

## Install

Linux, x86_64 or arm64. The API is not stable. This installs the rolling nightly built from `main`.

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
```

Put `~/.local/bin` on `PATH`. To reproduce a run, pin `ghcr.io/cybrid-systems/aura:<sha9>` or the image digest.

```sh
docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura:nightly main.aura
```

Supported hosts: Ubuntu 24.04, Debian 12, Rocky Linux 9, and openEuler 24.03 LTS SP4, each on x86_64 and arm64. macOS, Windows, and other Linux distributions are not supported.

## Quick start

```sh
aura demo
```

Offline, no model: four fixed candidates. Each step prints KEEP or ROLLBACK and the live source. It ends with `(guess) => 42`.

The same program as a project:

```sh
aura new evolve demo
cd demo
aura run
aura test
```

`aura demo agent` is the two-role version (a proposer and a checker). `aura new` also scaffolds `app`, `evolve-moo`, and `agent`.

## Learn more

- [Getting started](docs/getting-started.md)
- [Install details](docs/install.md)
- [Editor and the REPL](docs/editor.md)
- [Evolution log](docs/evo.md)
- [Starter kits](https://github.com/cybrid-systems/aura-starter-kits)
- [HTTP live-tune example](examples/live-tune)
