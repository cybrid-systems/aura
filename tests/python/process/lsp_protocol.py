"""Protocol-conformance cases for `aura lsp` (#5258).

The theme file (test_lsp.py) owns the case table; this module holds the
client-side helpers and the cases. Everything here drives the real binary over
Content-Length framed stdio — no in-process shortcuts.
"""

from __future__ import annotations

import json
import os
import select
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[3]
AURA = ROOT / "build" / "aura"


def env(extra: dict[str, str] | None = None) -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING", "AURA_PATH"):
        e.pop(k, None)
    e["AURA_RUNTIME_DIR"] = str(ROOT)
    if extra:
        e.update(extra)
    return e


def frame(obj: object) -> bytes:
    raw = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(raw) + raw


def frame_raw(body: str) -> bytes:
    raw = body.encode()
    return b"Content-Length: %d\r\n\r\n" % len(raw) + raw


class Client:
    """One `aura lsp` process and its framed stdout."""

    def __init__(self, extra_env: dict[str, str] | None = None) -> None:
        self.p = subprocess.Popen(
            [str(AURA), "lsp"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env(extra_env),
        )
        self.buf = b""
        self.frames: list[dict] = []

    def send(self, obj: object) -> None:
        try:
            assert self.p.stdin
            self.p.stdin.write(frame(obj))
            self.p.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass

    def send_raw(self, body: str) -> None:
        try:
            assert self.p.stdin
            self.p.stdin.write(frame_raw(body))
            self.p.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass

    def pump(self, timeout: float = 1.0) -> list[dict]:
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            try:
                ready, _, _ = select.select([self.p.stdout], [], [], left)
            except (ValueError, OSError):
                break
            if not ready or self.p.stdout is None:
                break
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                break
            self.buf += chunk
            self._parse()
        return self.frames

    def _parse(self) -> None:
        while True:
            head_end = self.buf.find(b"\r\n\r\n")
            if head_end < 0:
                return
            length = None
            for line in self.buf[:head_end].split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            if length is None or len(self.buf) < head_end + 4 + length:
                return
            body = self.buf[head_end + 4 : head_end + 4 + length]
            self.buf = self.buf[head_end + 4 + length :]
            try:
                self.frames.append(json.loads(body))
            except json.JSONDecodeError:
                self.frames.append({"__raw__": body.decode(errors="replace")})

    def clear(self) -> None:
        self.frames = []

    def close(self) -> tuple[int, str]:
        self.send({"jsonrpc": "2.0", "id": 9999, "method": "shutdown"})
        self.send({"jsonrpc": "2.0", "method": "exit"})
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        try:
            err = self.p.stderr.read().decode(errors="replace") if self.p.stderr else ""
        except (ValueError, OSError):
            err = ""
        for pipe in (self.p.stdin, self.p.stdout, self.p.stderr):
            try:
                if pipe:
                    pipe.close()
            except (ValueError, OSError):
                pass
        return self.p.returncode, err

    # ── protocol sugar ────────────────────────────────────────
    def initialize(
        self,
        caps: dict | None = None,
        root: str | None = None,
        client_info: dict | None = None,
    ) -> dict:
        params: dict = {"capabilities": caps or {}}
        if root:
            params["rootUri"] = root
        if client_info:
            params["clientInfo"] = client_info
        self.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": params})
        for f in self.pump(2.0):
            if f.get("id") == 1:
                return f.get("result", {})
        raise AssertionError("no initialize reply")

    def did_open(self, uri: str, text: str) -> None:
        self.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {"textDocument": {"uri": uri, "languageId": "aura", "version": 1, "text": text}},
            }
        )

    def did_change(self, uri: str, text: str, version: int = 2) -> None:
        self.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": version},
                    "contentChanges": [{"text": text}],
                },
            }
        )

    def definition(self, uri: str, line: int, character: int) -> object:
        first = len(self.frames)
        self.send(
            {
                "jsonrpc": "2.0",
                "id": 20,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": line, "character": character},
                },
            }
        )
        for f in self.pump(2.0)[first:]:
            if f.get("id") == 20:
                return f.get("result")
        return "no reply"

    def pull(self, uri: str) -> object:
        first = len(self.frames)
        self.send(
            {
                "jsonrpc": "2.0",
                "id": 10,
                "method": "textDocument/diagnostic",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        for f in self.pump(2.0)[first:]:
            if f.get("id") == 10:
                return f.get("result")
        return "no reply"


PULL_CAPS = {"textDocument": {"diagnostic": {"dynamicRegistration": False, "relatedDocumentSupport": False}}}


def publications(frames: list[dict]) -> list[dict]:
    return [f["params"] for f in frames if f.get("method") == "textDocument/publishDiagnostics"]


def last_publication(frames: list[dict], uri: str | None = None) -> dict | None:
    found = [p for p in publications(frames) if uri is None or p.get("uri") == uri]
    return found[-1] if found else None


def starts(publication: dict | None) -> list[tuple[int, int]]:
    if not publication:
        return []
    return [(d["range"]["start"]["line"], d["range"]["start"]["character"]) for d in publication.get("diagnostics", [])]


class LspProtocol5359(unittest.TestCase):
    """One channel per client: pull displaces the push, and only for pull clients.

    eglot 1.24 declares textDocument.diagnostic and also feeds
    publishDiagnostics to flymake (eglot--flymake-report-push+pulled reports
    both, no dedup), so the same set arrived twice and every diagnostic was
    shown twice. A client that declares the capability is answered by
    textDocument/diagnostic alone; a client that does not keeps the push
    (lsp-mode relies on it).
    """

    def test_pull_client_gets_pull_only_and_push_client_keeps_the_push(self) -> None:
        text = "(display oops)\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "p.aura"
            path.write_text(text)
            uri = path.as_uri()

            # eglot: declares the pull capability and identifies itself.
            # One delivery, not two.
            c = Client()
            try:
                caps = c.initialize(PULL_CAPS, client_info={"name": "Eglot"})
                self.assertIn("diagnosticProvider", caps["capabilities"])
                c.did_open(uri, text)
                frames = c.pump(2.0)
                self.assertEqual(publications(frames), [], "a pull client must not also be pushed")
                pulled = c.pull(uri)
                self.assertEqual(
                    [(i["range"]["start"]["line"], i["range"]["start"]["character"]) for i in pulled["items"]],
                    [(0, 9)],
                    "the pull answer is the one and only set",
                )
                # A change must not switch the channel back on either.
                c.clear()
                c.did_change(uri, "(define ok 1)\n(display oops)\n")
                self.assertEqual(publications(c.pump(1.5)), [])
                pulled = c.pull(uri)
                self.assertEqual([i["range"]["start"]["line"] for i in pulled["items"]], [1])
            finally:
                c.close()

            # lsp-mode: no pull capability, so the push is unchanged.
            c = Client()
            try:
                c.initialize({"general": {"positionEncodings": ["utf-16"]}})
                c.did_open(uri, text)
                frames = c.pump(2.0)
                self.assertEqual(starts(last_publication(frames, uri)), [(0, 9)])
                self.assertEqual(len(publications(frames)), 1, frames)
            finally:
                c.close()

            # lsp-mode 10.0.0 declares the pull capability too, but its
            # full-sync path never pulls after an edit, so it must keep the
            # push: suppressing it froze diagnostics after the first change.
            c = Client()
            try:
                c.initialize(PULL_CAPS, client_info={"name": "emacs"})
                c.did_open(uri, text)
                frames = c.pump(2.0)
                self.assertEqual(starts(last_publication(frames, uri)), [(0, 9)])
                self.assertEqual(len(publications(frames)), 1, frames)
                c.clear()
                c.did_change(uri, "(define ok 1)\n(display oops)\n")
                frames = c.pump(1.5)
                self.assertEqual(
                    [p["diagnostics"][0]["range"]["start"]["line"] for p in publications(frames)],
                    [1],
                    "a pull-capable client that does not pull after a change keeps the push",
                )
            finally:
                c.close()

    def test_diagnostic_capability_must_be_an_object(self) -> None:
        # The capability is an object in the spec; a null/absent value is a
        # client that does not pull, and must keep the push.
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "n.aura"
            text = "(display oops)\n"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize({"textDocument": {"diagnostic": None}}, client_info={"name": "Eglot"})
                c.did_open(uri, text)
                self.assertEqual(starts(last_publication(c.pump(2.0), uri)), [(0, 9)])
            finally:
                c.close()


class LspProtocol5258(unittest.TestCase):
    def test_initialize_negotiates_encoding_and_reports_real_version(self) -> None:
        c = Client()
        try:
            result = c.initialize({"general": {"positionEncodings": ["utf-32", "utf-8", "utf-16"]}})
            caps = result["capabilities"]
            self.assertEqual(caps["positionEncoding"], "utf-8")
            self.assertTrue(caps["textDocumentSync"]["openClose"])
            self.assertEqual(caps["textDocumentSync"]["change"], 1)
            self.assertIn("diagnosticProvider", caps)
            version = subprocess.run(
                [str(AURA), "--version"], capture_output=True, text=True, env=env(), check=True
            ).stdout.split()[1]
            self.assertEqual(result["serverInfo"]["version"], version)
            self.assertNotEqual(result["serverInfo"]["version"], "0.1")
        finally:
            c.close()

        c = Client()
        try:
            result = c.initialize({"general": {"positionEncodings": ["utf-16"]}})
            self.assertEqual(result["capabilities"]["positionEncoding"], "utf-16")
        finally:
            c.close()

    def test_uri_spellings_hit_one_document(self) -> None:
        # eglot/lsp-mode leave "+", "@" and ":" unescaped and send raw UTF-8;
        # other clients send upper-case %XX. Both name the open buffer.
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "a+b@c \u6f22.aura"
            path.write_text("(display oops)\n")
            emacs_uri = "file://" + str(path)
            hex_uri = "file://" + quote(str(path), safe="/")
            for uri in (emacs_uri, hex_uri):
                c = Client()
                try:
                    c.initialize()
                    c.clear()
                    c.did_open(uri, "(define uu 1)\n(display uu)\n")
                    frames = c.pump(2.0)
                    pub = last_publication(frames)
                    self.assertIsNotNone(pub, uri)
                    self.assertEqual(pub["uri"], uri, "diagnostics must echo the client URI")
                    self.assertEqual(starts(pub), [], "the open buffer, not the disk file")
                    target = c.definition(uri, 1, 10)
                    self.assertIsInstance(target, dict, uri)
                finally:
                    c.close()

    def test_range_change_is_refused_not_applied_as_fragment(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "r.aura"
            text = "(define keep 1)\n(display keep)\n"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)
                c.pump(1.5)
                c.clear()
                c.send(
                    {
                        "jsonrpc": "2.0",
                        "method": "textDocument/didChange",
                        "params": {
                            "textDocument": {"uri": uri, "version": 2},
                            "contentChanges": [
                                {
                                    "range": {
                                        "start": {"line": 0, "character": 0},
                                        "end": {"line": 0, "character": 0},
                                    },
                                    "text": "(display oops)\n",
                                }
                            ],
                        },
                    }
                )
                frames = c.pump(1.5)
                self.assertEqual(starts(last_publication(frames, uri)), [])
                logs = [f["params"]["message"] for f in frames if f.get("method") == "window/logMessage"]
                self.assertTrue(any("range" in m for m in logs), logs)
                target = c.definition(uri, 1, 10)
                self.assertIsInstance(target, dict, "the document must not become the fragment")
            finally:
                c.close()

    def test_debounced_change_publishes_then_pull_matches_then_close_clears(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "d.aura"
            path.write_text("(display #t)\n")
            uri = path.as_uri()
            # 800 ms proves both the debounce and that it is configurable.
            c = Client({"AURA_LSP_DEBOUNCE_MS": "800"})
            try:
                c.initialize()
                c.did_open(uri, "(display #t)\n")
                c.pump(1.0)
                c.clear()
                c.did_change(uri, "(display oops)\n")
                early = c.pump(0.4)
                self.assertEqual(starts(last_publication(early, uri)), [], "published before the debounce")
                late = c.pump(2.5)
                pub = last_publication(late, uri)
                self.assertIsNotNone(pub, "diagnostics after the debounce")
                self.assertEqual(starts(pub), [(0, 9)])

                pulled = c.pull(uri)
                self.assertEqual(pulled["kind"], "full")
                self.assertEqual(
                    [(i["range"]["start"]["line"], i["range"]["start"]["character"]) for i in pulled["items"]],
                    starts(pub),
                )

                c.clear()
                c.send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {"textDocument": {"uri": uri}}})
                frames = c.pump(1.0)
                self.assertEqual([(p["uri"], p["diagnostics"]) for p in publications(frames)], [(uri, [])])
            finally:
                c.close()

    def test_invalid_json_and_batch_get_error_replies(self) -> None:
        c = Client()
        try:
            c.initialize()
            c.clear()
            c.send_raw("{not json")
            frames = c.pump(1.0)
            errors = [f["error"] for f in frames if "error" in f]
            self.assertEqual([e["code"] for e in errors], [-32700])
            c.clear()
            c.send_raw(json.dumps([{"jsonrpc": "2.0", "id": 2, "method": "initialize", "params": {}}]))
            frames = c.pump(1.0)
            errors = [f["error"] for f in frames if "error" in f]
            self.assertEqual([e["code"] for e in errors], [-32600])
        finally:
            rc, err = c.close()
            self.assertNotIn("SIGSEGV", err)
            self.assertEqual(rc, 0)

    def test_initialize_without_id_does_not_crash(self) -> None:
        c = Client()
        c.send({"jsonrpc": "2.0", "method": "initialize", "params": {"capabilities": {}}})
        frames = c.pump(1.0)
        self.assertIsNone(c.p.poll(), "the server must survive an initialize notification")
        self.assertEqual(frames, [], "a notification gets no reply")
        c.did_open("untitled:Notify-1", "(display #t)\n")
        self.assertIsNotNone(c.pump(2.0))
        rc, err = c.close()
        self.assertNotIn("SIGSEGV", err)
        self.assertNotIn("AURA CRASH", err)
        self.assertEqual(rc, 0)

    def test_positions_follow_the_negotiated_encoding(self) -> None:
        # #5235 fixed the byte column; the LSP layer converts it. "漢字" is
        # four UTF-16 units in the block comment, so the wire column is 12
        # bytes lower than the file's byte column.
        text = "#|\u6f22\u5b57|# (display oops)\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "u.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize({"general": {"positionEncodings": ["utf-16"]}})
                c.did_open(uri, text)
                self.assertEqual(starts(last_publication(c.pump(2.0), uri)), [(0, 16)])
            finally:
                c.close()
            c = Client()
            try:
                c.initialize({"general": {"positionEncodings": ["utf-8", "utf-16"]}})
                c.did_open(uri, text)
                self.assertEqual(starts(last_publication(c.pump(2.0), uri)), [(0, 20)])
            finally:
                c.close()

    def test_aura_path_and_stdlib_definition(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            proj = Path(d) / "proj"
            extlib = Path(d) / "extlib"
            proj.mkdir()
            extlib.mkdir()
            (proj / "aura.toml").write_text('[project]\nname = "p"\n')
            (extlib / "ext.aura").write_text("(define (ext-fn x) (+ x 1))\n")
            main = proj / "main.aura"
            main.write_text(
                '(require "ext")\n(require "std/list")\n(display (ext-fn 1))\n'
                "(display (append-map (lambda (x) x) (list 1 2)))\n"
            )
            uri = main.as_uri()
            c = Client({"AURA_PATH": str(extlib)})
            try:
                c.initialize({}, proj.as_uri())
                c.did_open(uri, main.read_text())
                self.assertEqual(
                    starts(last_publication(c.pump(4.0), uri)), [], "names from AURA_PATH modules are bound"
                )
                ext = c.definition(uri, 2, 11)
                self.assertIsInstance(ext, dict, ext)
                self.assertEqual(Path(ext["uri"].removeprefix("file://")).name, "ext.aura")
                std = c.definition(uri, 3, 12)
                self.assertIsInstance(std, dict, std)
                self.assertEqual(Path(std["uri"].removeprefix("file://")).name, "list.aura")
                self.assertIn("/lib/std/", std["uri"])
            finally:
                c.close()

    def test_untitled_buffer_gets_diagnostics(self) -> None:
        c = Client()
        try:
            c.initialize()
            c.clear()
            c.did_open("untitled:Untitled-1", "(display oops)\n")
            pub = last_publication(c.pump(2.0))
            self.assertIsNotNone(pub)
            self.assertEqual(pub["uri"], "untitled:Untitled-1")
            self.assertEqual(starts(pub), [(0, 9)])
        finally:
            c.close()
