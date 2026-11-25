# std/secret — minimal-disclosure credential store (#5417)

A secret should not live in an environment variable that every child, log
line and crash dump can read. `std/secret` stores named values in a local
locked-down directory and hands the plaintext only to an explicit
callback.

## Store

- Default path: `$AURA_SECRET_STORE`, else `$HOME/.aura/secrets`. With
  neither set there is no fallback: `secret:default-store` returns
  `bad-store-path` (and `std/llm` falls back to `LLM_API_KEY`).
- The directory is `0700`; each value is a `0600` file named
  `<name>.secret` and holds the value base64-encoded. A read also checks
  that both are owned by the current uid; a wrong mode or owner is
  `secret-unavailable`, never a value. The check uses `stat`, so an
  exec-denied sandbox also fails closed as unavailable.
- A name is `[A-Za-z0-9._-]+` and may not start with `.`; a store path is
  restricted to `[A-Za-z0-9/._-]+`, so neither can inject a shell word.
- No exported handle contains the value. Printing a handle prints its
  name and path only.

```aura
(require "std/secret" all:)

(define store (secret:default-store))
(secret:put! store "llm-api-key" key)        ; admin: needs exec + write

(secret:use (secret:get store "llm-api-key")
  (lambda (k) (http-post url body k)))  ; the response, not the key
```

`secret:use` returns the callback's result, masking a string result with
`***` so an accidental return cannot print the value. A module that must
hand the value to a transport uses `secret:use-raw` explicitly. A missing
value, a sandbox without `io-read` (or without the exec needed for the
`stat` ownership check), an unreadable file, a directory that is not
`0700`, or a file that is not `0600` returns
`(hash "ok" #f "error" "secret-unavailable")`; the file's contents are
never part of an error. `secret:list`, `secret:exists?` and
`secret:audit` are read-only queries; audit records hold the name, the
action, success and the time, never the value.

## Redaction

Every value seen through `put!`, `import-file!`, `import-env!` or `use`
is registered in-process. `(secret:redact s)` replaces each occurrence
with `***`, and `(secret:contains-known? s)` answers whether `s` still
carries one. Use `redact` before logging a callback result.

```aura
(secret:redact (string-append "key=" key))  ; "key=***"
```

## Migration

`LLM_API_KEY` still works as a compatibility import source, but it is no
longer the recommended long-term store:

```aura
(secret:import-env! store "llm-api-key" "LLM_API_KEY")
(secret:import-file! store "llm-api-key" "/path/to/key-file")
```

`std/llm` reads `llm-api-key` from the store first and falls back to
`LLM_API_KEY` only when the store has no value. `AURA_LLM_SECRET_NAME`
overrides the name.

## Sandbox

Reading a secret is a file read plus a metadata check: it needs
`io-read` and `exec` (for `stat`). Provisioning (`put!`, `import-*`) also
creates the directory and fixes its mode, so it needs `exec` and `write`;
this is an admin operation, not something a request handler does. Denial
yields `secret-unavailable`, not the value.

## Non-goals

- No cloud KMS or enterprise IAM; the backend is a locked local file.
- No value is returned by the module itself except through the callback
  or an explicit `secret:use-raw`; do not write it back to source.
