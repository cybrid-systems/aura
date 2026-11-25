#!/usr/bin/env python3
"""Issue #5467: a paginated MCP fixture that stays alive after stdin EOF.

tools/list returns alpha/beta with nextCursor "page-2", then gamma/delta.
After the request stream ends the process sleeps, so a client that waits for
EOF would pay its full timeout even though the reply is already on stdout.
"""

import json
import sys
import time


def reply(obj):
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def text_result(text):
    return {"content": [{"type": "text", "text": text}]}


for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        continue
    method = msg.get("method")
    mid = msg.get("id")
    if method == "initialize":
        reply(
            {
                "jsonrpc": "2.0",
                "id": mid,
                "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "aura-mcp-paged", "version": "1.0"},
                },
            }
        )
    elif method == "notifications/initialized":
        continue
    elif method == "tools/list":
        params = msg.get("params") or {}
        cursor = params.get("cursor")
        if cursor in (None, ""):
            reply(
                {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "result": {
                        "tools": [
                            {"name": "alpha", "inputSchema": {"type": "object"}},
                            {"name": "beta", "inputSchema": {"type": "object"}},
                        ],
                        "nextCursor": "page-2",
                    },
                }
            )
        elif cursor == "page-2":
            reply(
                {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "result": {
                        "tools": [
                            {"name": "gamma", "inputSchema": {"type": "object"}},
                            {"name": "delta", "inputSchema": {"type": "object"}},
                        ]
                    },
                }
            )
        else:
            reply(
                {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "error": {"code": -32602, "message": "bad cursor"},
                }
            )
    elif method == "tools/call":
        params = msg.get("params") or {}
        name = params.get("name")
        if name == "delta":
            reply({"jsonrpc": "2.0", "id": mid, "result": text_result("delta")})
        else:
            reply(
                {
                    "jsonrpc": "2.0",
                    "id": mid,
                    "error": {"code": -32602, "message": "unknown tool: " + str(name)},
                }
            )
    elif mid is not None:
        reply(
            {
                "jsonrpc": "2.0",
                "id": mid,
                "error": {"code": -32601, "message": "method not found"},
            }
        )

# Deliberate: do not exit on stdin EOF.
time.sleep(30)
