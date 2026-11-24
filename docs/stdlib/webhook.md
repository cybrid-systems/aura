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
  answered `503` at once.
- **Cancellable.** The server fiber sleeps between polls and every handler
  sleeps in its own waits, so `fiber:cancel` reaches them at the next
  safepoint. `(webhook:stop! s)` is the orderly form: it closes the
  listener first (no new request), lets the requests in flight finish,
  stops the server fiber, and keeps the audit trail for `webhook:audit`.
- **Limits** (`webhook:make`): `"max-body"` bytes (65536) — `413` as soon
  as the head announces more; `"max-header"` bytes (16384) — `431`;
  `"max-inflight"` (16).
- **Not supported:** `Transfer-Encoding: chunked` (`501`), multipart or
  form decoding, TLS, keep-alive. `Expect: 100-continue` gets its go-ahead.

A body is UTF-8 **text**: the transport decodes bytes into a string, so
binary payloads are not preserved (a `0x00` byte is dropped, invalid
sequences become replacement characters). JSON webhooks are text.

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
  thread: measured there, one request costs the transport's 10 s receive
  timeout, because the worker that must run the handler is held by the
  client's `tcp-recv`. Use the thread backend until the transport gets a
  parking receive.
- **Stop the server before the process exits.** A server fiber that
  outlives the evaluator is detached at teardown and then touches freed
  state: measured as a `SIGSEGV` in `StringPool::intern_epoch` after the
  `[fiber:drain]` warning. `webhook:stop!` joins it.
- Needs the network effect (`--grant net`). On the default Restricted face
  `webhook:start!` answers `could not listen on ...: effect-denied:
  network not granted ... op=tcp-listen`; under `AURA_SANDBOX=strict` the
  `(require "std/webhook")` itself fails.
- An oversized request is refused `413` and its connection is closed at
  once, before the body is read: a client that is still sending may see a
  reset instead of the status.

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
handler sleeps; `stop` closes the listener and keeps the audit trail.
