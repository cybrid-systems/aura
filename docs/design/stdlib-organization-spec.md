# Stdlib Organization Specification (Issue #565)

> **Authoritative reference** for new stdlib modules. Companion to
> [`primitive-vs-stdlib-decision-framework.md`](primitive-vs-stdlib-decision-framework.md).
> This spec was established in Issue #565 to prevent stdlib sprawl
> as primitives demote at scale (Issues #558-#564).

---

## 1. Directory structure

```
lib/std/
├── <topic>.aura          # main module (required)
├── <topic>.aura-type     # type signatures (required for stdlib landers)
├── INDEX.aura             # discoverability index (this PR)
├── INDEX.aura-type       # type signatures for INDEX
├── list/                  # vertical: domain-specific utilities
│   ├── algorithm.aura
│   └── algorithm.aura-type
└── <vertical>/            # vertical-specific modules
    ├── <vtopic>.aura
    └── <vtopic>.aura-type
```

**Naming rules:**
- File name = lowercase kebab-case, no spaces, no underscores.
- Topic = single word OR short kebab phrase.
- Type file = `<topic>.aura-type` (exact suffix).

---

## 2. Export conventions

Every stdlib `.aura` file MUST begin with:

```aura
; lib/std/<topic>.aura — <one-line description>
;
; Issue #N: <why this module exists>
;
; Import with: (import "std/<topic>")
; Or:        (require "std/<topic>" all:)
;
; <Longer description if needed>

(export <name1> <name2> ...)
```

**Naming rules for exports:**
- `<topic>:<verb-noun>` for functions (e.g. `query:find-by-name`, `ast:summary-formatted`).
- `<topic>:<adjective>` for predicates (e.g. `query:contains?`).
- `<topic>:<noun>` for data (e.g. `query:templates`, `query:categories`).
- One export per line in the `(export ...)` statement (for grep-ability).
- All exports listed in BOTH `<topic>.aura` and `<topic>.aura-type`.

---

## 3. Required vs optional sections

**Required:**
- (export ...) line — top of file, ALL public functions listed.
- One-liner description above the export.
- Type file `<topic>.aura-type` with matching signatures.

**Optional (recommended for new modules):**
- "Recommended Agent pattern" block (4-line example).
- "Tier" annotation if the module demotes specific primitives (per
  decision-framework document).
- "Future follow-up" block listing Tier 2/3 candidates.

---

## 4. Naming prefixes for stdlib landers

| Stdlib lander | Pattern | Examples |
|---|---|---|
| `:find` family | `<topic>:find[-by-x]` | `query:find-by-name`, `query:nodes-with-marker` |
| `:list` family | `<topic>:list` | `query:list`, `query:list-categories`, `query:list-templates` |
| `:help` family | `<topic>:help` | `query:help`, `synthesize:list-help` |
| `:summary` family | `<topic>:summary[-formatted]` | `ast:summary-formatted`, `ws:current-stats` |
| `:validate` family | `<topic>:validate[-x]` | `ast:validate-summary` |
| `:pressure` family | `<topic>:memory-pressure`, `<topic>:memory-stats` | `ast:memory-pressure`, `ws:memory-pressure` |
| `:rollback` family | `<topic>:rollback[-latest]` | `ws:rollback-latest` |
| `:clamp` / `:lerp` | numeric helpers | `core:clamp`, `core:lerp` |
| `:safe-*` | defensive wrappers | `core:safe-div` |

---

## 5. Test template (per the issue's "标准测试模板")

Every stdlib demotion PR should ship a test file. Use this template:

```cpp
// tests/test_stdlib_<topic>_demotion.cpp — Issue #NNN:
// <one-line description of what this stdlib module demotes>
//
// Non-duplicative with #558-#563. This binary focuses on the
// stdlib surface introduced in this PR:
//   - AC1: lib/std/<topic>.aura present + exports N functions
//   - AC2: lib/std/<topic>.aura-type present (N signatures)
//   - AC3: >= M engine primitives wrapped (≥K acceptance met)
//   - AC4: docs/design/<topic>-decision.md present (Tier table)
//   - AC5: regression — core <topic>:* primitives still work

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

import std;

// Layer 1: stdlib file + structure checks (file I/O only)
// Layer 2: engine primitive regression (use Aura eval)

namespace test_issue_NNN_detail {

static int count_occurrences(const std::string& h, const std::string& n) {
    int c = 0;
    std::size_t p = 0;
    while ((p = h.find(n, p + 1)) != std::string::npos) ++c;
    return c;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::string c;
    if (f.good()) c.assign((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    return c;
}

// AC1, AC2, AC3, AC5: file-level checks
// AC4: documentation check
}  // namespace

int main() { /* RUN_ALL_TESTS(); */ }
```

---

## 6. Discoverability via INDEX.aura

The `lib/std/INDEX.aura` file lists every module with a one-line
description. This is the primary discoverability surface for AI
Agents. See Issue #565 ship for the format.

---

## 7. CI gate enhancement

`./build.py docs` already runs `gen_docs.py` which generates
`docs/generated/stdlib-index.md` from the stdlib files. This PR adds:

- A new `check-stdlib-consistency` sub-step in `gen_docs.py` that
  verifies every `<topic>.aura-type` matches its `<topic>.aura`
  exports exactly.
- Updated `INDEX.aura` is referenced from `lib/std/INDEX.aura`
  (no change to discoverability surface).
- `lib/std/INDEX.aura` is added to the stdlib catalog.

---

## 8. Migration path (for future cycles)

For each new stdlib module added in a future PR:

1. Place the `.aura` + `.aura-type` files in `lib/std/`.
2. Add a one-line entry to `lib/std/INDEX.aura` (the discoverability
   index).
3. Ship a test binary following the template above (§5).
4. Run `./build.py docs` to regenerate `docs/generated/stdlib-index.md`.
5. Run `./build.py gate` to verify consistency.

---

_Last updated: 2026-06-28 (Issue #565)._