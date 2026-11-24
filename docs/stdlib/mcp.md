# std/mcp — Model Context Protocol client (#5416)

`std/mcp` speaks the MCP stdio transport from Aura: it starts a local MCP
server command, performs the `initialize` handshake, and lists or calls
one tool. It is a client, not a tool registry and not a permission
system.

```aura
(require "std/mcp" all:)

(define c (mcp:client "python3 my_server.py" 20))  ; seconds, default 15
(mcp:initialize c)                                  ; serverInfo/capabilities
(define listed (mcp:list-tools c))
(mcp:tool-names (hash-ref listed "tools" '()))     ; ("echo" "add")
(mcp:call-tool c "echo" (hash "text" "hi"))
```

Each call is one short-lived session: initialize +
`notifications/initialized` + the requested method are written to the
child's stdin as newline-delimited JSON-RPC, the child's stdout is read
until EOF, and non-JSON lines are ignored. A server that needs a
long-lived session (resources, subscriptions, server-initiated
requests) is outside this MVP.

## Results

- `mcp:list-tools` → `(hash "ok" #t "tools" [...])`; an error is
  `(hash "ok" #f "error" message)`.
- `mcp:call-tool` → `(hash "ok" #t "result" ...)` where `result` is the
  MCP result object (`content` array and `isError` when present).
- `mcp:initialize` → `(hash "ok" #t "result" ...)` with the server's
  `protocolVersion`, `capabilities` and `serverInfo`.
- A missing child, a timeout (`timeout` seconds) or an unreadable reply
  is `mcp-server-failed`, `mcp-initialize-failed` or `mcp-list-failed`;
  the module never fabricates a tool result.

## Security

- Starting the server is an exec action: the process still needs
  `--grant exec` (the `shell` primitive enforces it). Without it the
  answer is `mcp-exec-unavailable`.
- Tool descriptions, arguments and results are ingested (Bastion) data.
  They are plain strings and hashes; this module never turns them into a
  capability, a `--grant`, a task scope or an approval. A side effect is
  the server's own authority, not authority borrowed from Aura.
- The child inherits the caller's cwd and environment. Put secrets in the
  secret store (#5417) and pass only what the server needs; the module
  does not read or forward any key itself.
- No MCP runtime dependency enters the default binary: this is a pure
  `std` module over the existing process/file primitives.

## Fixture and tests

`tests/fixtures/mcp_tool_server.py` is a deterministic JSONL server with
`echo` and `add`; `tests/suite/mcp.aura` covers initialize, list, call,
unknown tool and bad arguments. Run:

```sh
./build/aura tests/suite/mcp.aura
```
