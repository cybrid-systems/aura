# Aura

AI-native Lisp — 代码自己进化。

The API is not stable. These commands install the rolling nightly built from `main`.

## Install

Linux, x86_64 or arm64:

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
```

Put `~/.local/bin` on `PATH`. Nightly is replaced on each push to `main`. To reproduce a run, pin `ghcr.io/cybrid-systems/aura:<sha9>` or the image digest. More detail: [docs/install.md](docs/install.md).

```sh
docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura:nightly main.aura
```

Supported hosts are Ubuntu 24.04, Debian 12, Rocky Linux 9, and openEuler 24.03 LTS SP4, each on x86_64 and arm64. macOS, Windows, and other Linux distributions are not supported.

## Quick start

```sh
aura demo
```

One definition, four fixed candidates, and the live source after each KEEP or
ROLLBACK:

```
1 ROLLBACK  (define (guess) 10)   live (define (guess) 0)
2 KEEP      (define (guess) 7)    live (define (guess) 7)
3 ROLLBACK  (define (guess) 2)    live (define (guess) 7)
4 KEEP      (define (guess) 42)   live (define (guess) 42)
(guess) => 42
```

The same program is a project you can edit:

```sh
aura new evolve demo   # the same program as a project
cd demo
aura run
aura test
```

`aura demo agent` shows the two-role version (a proposer and a checker).
`aura new` (`app`, `evolve`, or `agent`) writes a template from this repository. The evolve-strategy kit is a separate project: [aura-starter-kits](https://github.com/cybrid-systems/aura-starter-kits).

[Getting started](docs/getting-started.md) · [Editor support and the REPL](docs/editor.md)
