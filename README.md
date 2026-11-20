# Aura

AI-native Lisp — 代码自己进化：a running program proposes new source, a trial keeps or rolls it back, and the process does not restart.

## Watch a world change while the game keeps running

The hero runs on autopilot. Spoken commands switch the world: Garden, Beijing, San Francisco, Space (the runner morphs), Lava Canyon, Aurora Ice.

![Lumen Run on autopilot. Spoken commands switch worlds, and the corner note shows each evo trial KEEP or ROLLBACK.](docs/assets/aura-runner-edsl.gif)

Each world, morph, and phrase is one Aura form (the Biome EDSL), proposed live by a model or by an offline recipe.
`evo:trial` scores that form before the game can take it.
KEEP rebinds it into the running game. ROLLBACK leaves the current form in place. The corner note shows which.
The decision is appended to the audit log. The process does not restart.

The form, from [aura-runner](https://github.com/cybrid-systems/aura-runner) `lib/edsl.aura`:

```
; (biome ID
;   (label WORD)
;   (sky ZR ZG ZB HR HG HB)
;   (fog FR FG FB AMT)
;   (body sun|moon|stars|aurora|none)
;   (road R G B) (curb R G B) (ground R G B)
;   (surface grass|stone|dust|sand|ice|cloud|lava|reef|plank)
;   (light L) (mood M)
;   (particles fireflies|stardust|snow|embers|rain|sparkles|none DENSITY)
;   (props (KIND DENSITY SCALE) ...)
;   (recipe garden|city|space|coast|mix))
;
; KIND is tree, tower, arch, crystal, rock, coral, or ring.
; A proposal is kept only after evo:trial (edsl-gate).
```

[Lumen Run](https://github.com/cybrid-systems/aura-runner) (`aura-runner`). Quick start: `./run.sh`, then open the URL it prints.

## Install

Linux, x86_64 or arm64. The API is not stable. This installs the rolling nightly built from `main`.

```sh
curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
```

Put `~/.local/bin` on `PATH`. To reproduce a run, pin `ghcr.io/cybrid-systems/aura:<sha9>` or the image digest.

```sh
docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura:nightly main.aura
```

That image's entrypoint is `aura`, so the command runs `main.aura` in the current directory. Supported hosts: Ubuntu 24.04, Debian 12, Rocky Linux 9, and openEuler 24.03 LTS SP4, each on x86_64 and arm64. macOS, Windows, and other Linux distributions are not supported.

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
