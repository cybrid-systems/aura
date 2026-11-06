# Changelog

## 0.1.0

First stable channel. The API is not stable. This version installs and runs the shipped examples.

- Linux x86_64 and arm64. `AURA_VERSION=v0.1.0` selects the GitHub Release asset `aura-0.1.0-linux-<arch>.tar.gz`. The default install remains nightly.
- Images: `ghcr.io/cybrid-systems/aura:0.1.0`, `:0.1`, and `:latest`. `:nightly` stays the rolling build from `main`.
- A binary whose version is `X.Y.Z` makes `aura new` pin that channel. `0.1.0` writes `ghcr.io/cybrid-systems/aura:0.1.0`. `dev` and `nightly` write `:nightly`. `:latest` is never written into a new project.
- Install and smoke coverage is Ubuntu 24.04, Ubuntu 26.10, Debian 12, Rocky Linux 9, and openEuler 24.03 LTS SP4, on x86_64 and arm64.
- A tag release runs the chaos SOAK (300s, 8 workers, 256 fibers) and the production concurrency gate before assets are uploaded.

Not in this version:

- C++ Core Guidelines remediation is unfinished (#4936). These rules are still soft, not hard: R4 (special members), T7 (initialize locals), F2 (parameter passing), F6 (pointer-keyed iteration order), P2 (needless copies), P3 (`noexcept` move and `swap`), P6 (`std::scoped_lock`).
- macOS, Windows, and other Linux distributions.
- The evolve-strategy kit is not pinned to this tag yet.
