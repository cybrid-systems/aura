# std/webhook — an inbound HTTP server for callbacks (#5415)

A personal runtime has to receive callbacks: a webhook from a service, a
POST from a script. Aura had two things that look like it and are not:

| Listener | What it is | Why it is not a webhook server |
|----------|-----------|-------------------------------|
| `--health-port` / `AURA_HTTP_PORT` (`src/serve/http_health.h`, #677) | the ops listener: `/healthz`, `/readyz`, `/metrics` on **every interface** (`INADDR_ANY`), no authentication, one 2048-byte read per request | it answers three fixed paths; it has no routes, no authentication and no body |
| `--serve` / `--serve-async` | the stdin line protocol for evaluation (`embedding.md`) | not HTTP |
| `std/socket` | raw `tcp-*` transport | the application had to write HTTP by hand (`examples/live-tune/server.aura`) |

`std/webhook` is the application-facing server: a stdlib module over the
`tcp-*` transport with routes, a body limit, an authentication hook and an
audit trail.

```aura
(require "std/webhook" all:)

(define s (webhook:make (hash "auth" (webhook:token-auth token))))  ; token from the secret store (#5417)
(webhook:route! s "/hook" (list "POST")
  (lambda (req)
    (hash "status" 200 "body" "ok" "headers" (hash "Content-Type" "text/plain"))))
(webhook:start! s)
(webhook:port s)             ; the bound port (0 asked for an ephemeral one)
(webhook:stats s)            ; counters
(webhook:audit s 10)         ; the last 10 request records
(webhook:stop! s)
```

A request is `(hash "method" "path" "query" "headers" "body" "content-length")`:
`path` has no query string, `query` is the raw query, `headers` is keyed by
lower-case name. A handler answers a string (200, `text/plain`) or a hash
with `status`, `body` and `headers`; an error it raises is answered 500.

## Boundary with the health server

| | health listener | `std/webhook` |
|---|---|---|
| bind | every interface | `127.0.0.1` unless `"host"` says otherwise |
| port | `--health-port` / `AURA_HTTP_PORT` | its own; `webhook:make` refuses the health port (`"health-port"`, default `AURA_HTTP_PORT`) |
| paths | `/healthz` `/readyz` `/metrics` | those three are refused as routes (`webhook:route!` raises) and answered 404, never handed to a handler |
| authentication | none | an explicit hook; with no hook, **reads pass and every write is refused 401** |

The ops port stays an ops port. Nothing in this module reads or writes it,
and a webhook server cannot be talked into sharing it.

## Authentication

`"auth"` on the server or on a route is `(lambda (req) ...)`:

| the hook answers | the request gets |
|------------------|------------------|
| `#t` | the handler |
| `#f` | `401` + `WWW-Authenticate: Bearer` |
| a response hash | that response (a `429` with `Retry-After`, a `403`, ...) |
| anything else, or an error | `403` / `401` |

Order: a route's own hook, then `"public" #t` on the route (anonymous
requests, hook or not), then the server's hook, then — with no hook
anywhere — `GET` / `HEAD` / `OPTIONS` pass and `POST` / `PUT` / `PATCH` /
`DELETE` get `401`. A route that must take anonymous writes says so:
`(webhook:route! s "/incoming" (list "POST") handler (hash "public" #t))`.

`(webhook:token-auth token)` builds the hook for `Authorization: Bearer
<token>`: it compares every code point before the verdict, so the answer
time does not reveal where two tokens differ. The token itself belongs in a
secret store, not in source or environment (#5417).

## Request handling

- **One request per connection** (`Connection: close`), HTTP/1.1 request
  line + headers + a body with `Content-Length`. A route that takes `GET`
  takes `HEAD` too (same handler, no body on the wire).
- **Each request is handled on its own fiber.** The server fiber only
  accepts and accounts; the accept loop keeps serving while a handler
  works, up to `"max-inflight"` (16) at a time; beyond that a connection is
  answered `503` at once. The accept loop drains every ready connection
  before its 1 ms idle tick (#5451), so a burst does not pay one tick per
  connection; the listener's backlog is the kernel maximum (`SOMAXCONN`).
- **Cancellable.** The server fiber sleeps between polls and every handler
  sleeps in its own waits, so `fiber:cancel` reaches them at the next
  safepoint. `(webhook:stop! s)` is the orderly form: it closes the
  listener first (no new request), lets the requests in flight finish
  (`"drain-ms"`), cancels the handlers still running and waits for them to
  reach their safepoint (`"cancel-ms"`), stops the server fiber, and keeps
  the audit trail for `webhook:audit`. It answers `#t` only once no
  handler runs; a handler that never reaches a safepoint (a CPU-bound body)
  cannot be cancelled, and `stop!` then raises
  `webhook:stop!: N handler(s) still running` instead of answering `#t`
  with it alive (Issue #5436). A request cancelled this way is closed
  without a response. If the server fiber is already gone (cancelled by
  hand or crashed), `stop!` closes the listener itself after a 500 ms close
  wait, skips the stop handshake, releases `running`, and lets the same
  port be listened on again (#5452); the normal path never closes the
  listener twice.
- **Limits** (`webhook:make`): `"max-body"` bytes (65536) — `413` as soon
  as the head announces more; `"max-header"` bytes (16384) — `431`;
  `"max-inflight"` (16); `"read-timeout-ms"` (2000) — the default total
  budget for each of the request head and the body, with `"header-timeout-ms"`
  / `"body-timeout-ms"` overriding it per phase; `"close-drain-ms"` (250)
  — the shutdown/drain window after a refusal; `"drain-ms"` (15000) and
  `"cancel-ms"` (5000), the two windows of `webhook:stop!`.
- **Read deadlines.** A peer that sends a partial head or body and stalls is
  answered `408 Request Timeout` when its phase budget (`"read-timeout-ms"`,
  default 2 s; `"header-timeout-ms"` / `"body-timeout-ms"` override it)
  expires, so it cannot hold an in-flight slot indefinitely. The
  budget is total for the phase, not per `recv`. A refusal written before the
  request body was consumed (`408`, `413`, `431`, `501`, `503`) shuts the
  write side down (`shutdown(SHUT_WR)`), drains/discards the peer's remaining
  bytes for up to `"close-drain-ms"`, and only then closes, so the client can
  read the status line and body instead of getting `ECONNRESET` from a close
  with unread data (#5450).
- **Not supported:** `Transfer-Encoding: chunked` (`501`), multipart or
  form decoding, TLS, keep-alive. `Expect: 100-continue` gets its go-ahead.

A body is UTF-8 **text**: the transport decodes bytes into a string, so
binary payloads are not preserved (a `0x00` byte is dropped, invalid
sequences become replacement characters). JSON webhooks are text.

`Content-Length` is one decimal non-negative integer. A value that is not
1\*DIGIT (after the optional whitespace HTTP allows around a field value),
or a repeated `Content-Length`, is answered `400`; it is never treated as
zero. The body is exactly the announced number of bytes: bytes received
past it in the same read are discarded (#5453). A body shorter than the
announcement is `400 short body` (or `408` when the read budget expires).

Body and response lengths use `string-byte-length`, an O(1) count of the
stored UTF-8 bytes (not the code-point count of `string-length`). The
request reader collects chunks and concatenates them once, and
`tcp-send` can resume from a byte offset, so the request path is linear
and a 64 KiB body/response stays in milliseconds (#5448).

### Threads

Nothing is shared between threads except atomics and channels
([fiber-spawn.md](fiber-spawn.md)). The counters are `atomic:counter`s.
The audit trail belongs to the server fiber: handlers hand it records
through a channel (the transport deep-copies them), and `webhook:audit`
asks the server fiber through the control channel. `webhook:stop!` is the
one writer of the caller's server hash after start.

## Audit and counters

`(webhook:stats s)` → `requests`, `ok`, `denied` (401/403), `rejected`
(other 4xx), `failed` (5xx), `busy` (503), `inflight`, `running`, `port`,
`routes`.

`(webhook:audit s [n])` → the newest n of the last 256 records, each
`(hash "id" "method" "path" "status" "ms" "bytes")`. After `webhook:stop!`
the trail is still readable: the server fiber hands it over on its way
out.

## What it is not, and where it does not run

- Not a web framework or a reverse proxy: put TLS and routing prefixes in
  front of it.
- **Thread backend only** (`aura file`, `--load`, `--serve`). Under
  `--serve-async` the `tcp-*` primitives block the scheduler's worker
  thread: a request can hold a worker for its read budget (`"read-timeout-ms"`,
  2 s by default) because the blocking `tcp-recv-timeout` cannot park.
  Use the thread backend until the transport gets a parking receive.
- **Stop the server before the process exits.** A server fiber that
  outlives the evaluator is detached at teardown and then touches freed
  state: measured as a `SIGSEGV` in `StringPool::intern_epoch` after the
  `[fiber:drain]` warning. `webhook:stop!` joins it. A handler it could not
  cancel is a different matter: the runtime ends the process itself at
  that point, with the status the run had decided on, rather than tearing
  the evaluator down under it ([fiber-spawn.md](fiber-spawn.md)).
- Needs the network effect (`--grant net`). On the default Restricted face
  `webhook:start!` answers `could not listen on ...: effect-denied:
  network not granted ... op=tcp-listen`; under `AURA_SANDBOX=strict` the
  `(require "std/webhook")` itself fails.
- A refusal is written before the body is read, then the write side is
  shut down and the peer's remaining bytes are drained/discarded for up to
  `"close-drain-ms"` (250 ms) before close; a client that is still sending
  reads the status line and body instead of a reset. A peer that keeps
  sending past that window is closed anyway.

## Acceptance evidence

`tests/suite/webhook.aura` (the client is a raw socket in the same
process): an authenticated `POST` reaches the handler with its body and
custom response headers; a write without credentials, with a wrong token,
or with a bare token is `401` and the handler never runs; with no hook
reads pass and writes are refused while a `"public"` route takes the
anonymous write; a route's own hook decides and a hook can answer with a
response; the health paths are refused as routes and answered 404; the
health port is refused; an oversized body is `413` before the handler;
chunked is `501`, garbage is `400`, unknown path `404`, wrong method `405`
with `Allow`; a raising handler is `500`; `HEAD` on a `GET` route and
`Expect: 100-continue`; the accept loop serves a fast request while a slow
handler sleeps; `stop` closes the listener and keeps the audit trail. For
#5450: 16 half-sent request heads each time out with a complete `408` and
free their in-flight slots (a following normal request is `200`); a head
with `Content-Length` but no body answers `408`; a busy `503` is readable to
its body. For #5451: a 200-connection burst is accepted and answered `200`
without a reset (the old 5 ms tick and backlog 16 stalled the tail). For
#5453: a `Content-Length: 5` request that sends `helloEXTRA` hands the
handler `hello`; `abc`, `5, 5`, `0x10`, `5 5`, empty, `+5`, `5.0`, `-5`,
`1e1` and a repeated `Content-Length` are all `400`.
