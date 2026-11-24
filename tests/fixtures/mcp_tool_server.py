#!/usr/bin/env python3
"""A minimal MCP stdio fixture: initialize, tools/list, tools/call."""

import json
import sys


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
                    "serverInfo": {"name": "aura-mcp-fixture", "version": "1.0"},
                },
            }
        )
    elif method == "notifications/initialized":
        continue
    elif method == "tools/list":
        reply(
            {
                "jsonrpc": "2.0",
                "id": mid,
                "result": {
                    "tools": [
                        {
                            "name": "echo",
                            "description": "Echo text back",
                            "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}},
                        },
                        {
                            "name": "add",
                            "description": "Add two integers",
                            "inputSchema": {
                                "type": "object",
                                "properties": {"a": {"type": "integer"}, "b": {"type": "integer"}},
                            },
                        },
                    ]
                },
            }
        )
    elif method == "tools/call":
        params = msg.get("params") or {}
        name = params.get("name")
        args = params.get("arguments") or {}
        if name == "echo":
            reply({"jsonrpc": "2.0", "id": mid, "result": text_result("echo: " + str(args.get("text", "")))})
        elif name == "add":
            try:
                total = int(args.get("a", 0)) + int(args.get("b", 0))
            except (TypeError, ValueError):
                reply({"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "bad add arguments"}})
            else:
                reply({"jsonrpc": "2.0", "id": mid, "result": text_result(str(total))})
        else:
            reply({"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "unknown tool: " + str(name)}})
    elif mid is not None:
        reply({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}})
