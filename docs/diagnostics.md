# Machine-readable diagnostics (#5240)

One diagnostic schema serves every face: a one-shot file/CLI run
(`--error-format=json`), `--serve`, `--serve-async`, the in-process
`aura::eval` ABI, and the language server. The human text stays the
default; it is rendered from the same structured `aura::diag::Diagnostic`
the JSON is built from.

## The object

One JSON object per line (NDJSON) on stderr, in stream order. A file run
reports the first error only (#4909).

```json
{"code":"UnboundVariable","severity":"error","file":"main.aura","span":{"start":{"line":3,"column":11},"end":{"line":3,"column":11}},"message":"memq","hint":"did you mean 'member'?","snippet":"  (memq x xs)"}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `code` | string | Stable identity from `kind_ident()` (`src/reflect/diag_error_kind_names.hh`), e.g. `ParseError`, `TypeError`, `EffectDenied`. Never an enum ordinal: the enum may be reordered, this string may not change. |
| `severity` | string | `"error"` today. |
| `file` | string | Source path, omitted when the diagnostic has no file. |
| `span` | object | `{start,end}`, each `{line,column}`. **Lines and columns are 1-based. Columns are UTF-8 byte offsets in the line.** `end` equals `start` until the AST records an end. |
| `message` | string | The producer's message without the `kind: ` prefix or location. |
| `hint` | string, optional | The suggestion ("did you mean ...?"), also shown after the human text. |
| `notes` | array of strings, optional | Context frames (`Diagnostic::context_stack`). |
| `snippet` | string, optional | The source line the span points at. |

The column unit is part of the contract: a consumer that needs UTF-16
(LSP) converts from UTF-8 bytes, and a consumer that needs 0-based
coordinates subtracts one. `diag_json_format.py` pins both the byte
column (multi-byte literals before the error) and the 1-based line.

## Faces

Every face carries `code`, `message`, `span`, `hint` and `notes` with
the same values for the same program. Shell fields differ:

| Face | How | Shell fields |
| --- | --- | --- |
| file / `-e` | `aura --error-format=json file.aura` (`--error-format json` also works) | none; the object is the whole line |
| `--serve` | one JSON reply per request | `status:"error"`, `kind` (the same stable ident as `code`), `msg` (the human line) |
| `--serve-async` | one status line per request | `session`, `status:"error"`, `msg`, `display` |
| `aura::eval` | JSON record through `char**` | `status:"error"`, `kind` (display name), `line`/`column` (kept for older hosts) |
| `aura lsp` | `textDocument/publishDiagnostics` | `range`, `severity`, `source`, `code` (the ident); `message` still carries the human head and the hint (#5262 moves it to `relatedInformation`) |

The serve line protocol framing (#5158) and the daemon's exit code (always
0; errors live in the reply) are unchanged. Serve used to print an enum
ordinal in `kind` and a meaningless `node_id:4294967295`; both are gone.

## Exit codes (one-shot runs)

`aura file.aura`, `aura -e EXPR` and the tool positionals that read a
program use this table (also printed by `aura help`):

| Code | Phase | Examples |
| --- | --- | --- |
| 0 | success | the program ran and produced no error |
| 1 | runtime / unclassified | `(car 5)`, a division by zero today, an uncaught exception |
| 2 | usage | no program, unknown option, unreadable path |
| 3 | parse | unbalanced list, unterminated string |
| 4 | static | unbound variable, effect denial, module error, stale workspace |

Notes:

- `--typecheck` is a static-analysis tool, not a run: it keeps 0 (clean) /
  1 (diagnostics found) and prints the type report, not NDJSON.
- `--serve` and `--serve-async` always exit 0 on program errors.
- A `TypeError` that is only discovered while executing stays in the
  runtime bucket (1): the interpreter cannot tell a static TypeError from
  a runtime one until #5241 types every producer.

## Migrating downstream

`aura-harness` and `aura-starter-kits` should:

1. Stop treating every non-zero exit as "program failed the same way".
   Branch on 2/3/4 for usage/parse/static and 1 for runtime; the buckets
   used to all be 1.
2. Prefer `--error-format=json` over scraping stderr. Read `code` for
   classification (it is stable), `span` for the location (1-based, byte
   columns), and `message`/`hint` for display.
3. If they speak `--serve`, read `kind` as a string now (it was the enum
   ordinal) and ignore `node_id` (no longer emitted). `code` and `span`
   are the portable fields.

No compatibility flag is kept for the old serve ordinal: the values moved
whenever `ErrorKind` gained a member, which is exactly why the schema now
carries the ident.
