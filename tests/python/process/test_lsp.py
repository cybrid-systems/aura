"""Regression for #5155: aura lsp JSON-RPC parser has no nesting limit.

A ~120 KB message of brackets used to SIGSEGV the language server with an
empty stderr, because the parser recursed without a cap and `aura lsp`
returned before the crash handler was installed.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AURA = ROOT / "build" / "aura"


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
        e.pop(k, None)
    e["AURA_RUNTIME_DIR"] = str(ROOT)
    return e


def frame_raw(body: str) -> bytes:
    raw = body.encode()
    return f"Content-Length: {len(raw)}\r\n\r\n".encode() + raw


def frame(obj: dict) -> bytes:
    return frame_raw(json.dumps(obj))


def read_frames(raw: bytes) -> list[dict]:
    out = []
    while raw:
        head, _, rest = raw.partition(b"\r\n\r\n")
        if not head or b"Content-Length:" not in head:
            break
        n = int(head.split(b":")[1].strip())
        out.append(json.loads(rest[:n]))
        raw = rest[n:]
    return out


def nested_arrays(n: int) -> str:
    return "[" * n + "0" + "]" * n


def nested_objects(n: int) -> str:
    return '{"a":' * n + "1" + "}" * n


class LspDepth5155(unittest.TestCase):
    def test_deep_message_is_an_error_and_the_server_continues(self) -> None:
        deep_arr = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"x":' + nested_arrays(100000) + "}}"
        deep_obj = '{"jsonrpc":"2.0","id":2,"method":"initialize","params":{"x":' + nested_objects(100000) + "}}"
        ok = {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "initialize",
            "params": {"x": json.loads(nested_arrays(20))},
        }
        msgs = (
            frame_raw(deep_arr)
            + frame_raw(deep_obj)
            + frame(ok)
            + frame({"jsonrpc": "2.0", "id": 4, "method": "shutdown"})
            + frame({"jsonrpc": "2.0", "method": "exit"})
        )
        r = subprocess.run(
            [str(AURA), "lsp"],
            input=msgs,
            capture_output=True,
            env=env(),
            timeout=30,
        )
        err = r.stderr.decode(errors="replace")
        self.assertNotEqual(r.returncode, -signal.SIGSEGV, err)
        self.assertNotIn("SIGSEGV", err)
        self.assertNotIn("AURA CRASH", err)
        self.assertEqual(r.returncode, 0, err)
        frames = read_frames(r.stdout)
        by_id = {f.get("id"): f for f in frames}
        self.assertEqual(by_id[1]["error"]["code"], -32700)
        self.assertIn("nesting too deep", by_id[1]["error"]["message"])
        self.assertEqual(by_id[2]["error"]["code"], -32700)
        self.assertIn("nesting too deep", by_id[2]["error"]["message"])
        self.assertIn("definitionProvider", by_id[3]["result"]["capabilities"])
        self.assertIsNone(by_id[4].get("result"))

    def test_crash_handler_prints_diagnostics(self) -> None:
        p = subprocess.Popen(
            [str(AURA), "lsp"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env(),
        )
        assert p.stdin and p.stdout and p.stderr
        p.stdin.write(frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}))
        p.stdin.flush()
        header = b""
        while b"\r\n\r\n" not in header:
            chunk = p.stdout.read(1)
            self.assertTrue(chunk, "lsp closed before a reply")
            header += chunk
        n = int(header.split(b":")[1].strip())
        body = p.stdout.read(n)
        self.assertIn(b"aura-lsp", body)
        p.send_signal(signal.SIGSEGV)
        try:
            err = p.stderr.read()
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(timeout=5)
            self.fail("aura lsp ignored SIGSEGV")
        finally:
            p.stdin.close()
            p.stdout.close()
            p.stderr.close()
        text = err.decode(errors="replace")
        self.assertIn("=== AURA CRASH:", text)
        self.assertIn("SIGSEGV", text)
        self.assertIn("signal 11", text)


PROCESS_THEME = "lsp"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ("lsp_protocol", "lsp_format", "lsp_symbols", "lsp_semantic")
CASES = (
    (
        5155,
        "test_deep_message_is_an_error_and_the_server_continues",
        "LspDepth5155.test_deep_message_is_an_error_and_the_server_continues",
    ),
    (
        5155,
        "test_crash_handler_prints_diagnostics",
        "LspDepth5155.test_crash_handler_prints_diagnostics",
    ),
    # #5258 protocol base: encoding, URI spellings, debounce/pull/close,
    # JSON-RPC errors, AURA_PATH/std definition, untitled buffers.
    (
        5258,
        "test_initialize_negotiates_encoding_and_reports_real_version",
        "lsp_protocol.LspProtocol5258.test_initialize_negotiates_encoding_and_reports_real_version",
    ),
    (
        5258,
        "test_uri_spellings_hit_one_document",
        "lsp_protocol.LspProtocol5258.test_uri_spellings_hit_one_document",
    ),
    (
        5258,
        "test_range_change_is_refused_not_applied_as_fragment",
        "lsp_protocol.LspProtocol5258.test_range_change_is_refused_not_applied_as_fragment",
    ),
    (
        5258,
        "test_debounced_change_publishes_then_pull_matches_then_close_clears",
        "lsp_protocol.LspProtocol5258.test_debounced_change_publishes_then_pull_matches_then_close_clears",
    ),
    (
        5258,
        "test_invalid_json_and_batch_get_error_replies",
        "lsp_protocol.LspProtocol5258.test_invalid_json_and_batch_get_error_replies",
    ),
    (
        5258,
        "test_initialize_without_id_does_not_crash",
        "lsp_protocol.LspProtocol5258.test_initialize_without_id_does_not_crash",
    ),
    (
        5258,
        "test_positions_follow_the_negotiated_encoding",
        "lsp_protocol.LspProtocol5258.test_positions_follow_the_negotiated_encoding",
    ),
    (
        5258,
        "test_aura_path_and_stdlib_definition",
        "lsp_protocol.LspProtocol5258.test_aura_path_and_stdlib_definition",
    ),
    (
        5258,
        "test_untitled_buffer_gets_diagnostics",
        "lsp_protocol.LspProtocol5258.test_untitled_buffer_gets_diagnostics",
    ),
    # #5263: textDocument/formatting + rangeFormatting call the CLI formatter.
    (
        5263,
        "test_formatting_returns_a_text_edit_and_keeps_comments",
        "lsp_format.LspFormat5263.test_formatting_returns_a_text_edit_and_keeps_comments",
    ),
    (
        5263,
        "test_formatting_matches_cli_fmt",
        "lsp_format.LspFormat5263.test_formatting_matches_cli_fmt",
    ),
    (
        5263,
        "test_formatting_is_idempotent_in_the_editor",
        "lsp_format.LspFormat5263.test_formatting_is_idempotent_in_the_editor",
    ),
    (
        5263,
        "test_range_formatting_keeps_other_lines",
        "lsp_format.LspFormat5263.test_range_formatting_keeps_other_lines",
    ),
    (
        5263,
        "test_unparsable_buffer_returns_no_edits",
        "lsp_format.LspFormat5263.test_unparsable_buffer_returns_no_edits",
    ),
    (
        5263,
        "test_tab_size_and_insert_spaces_are_honored",
        "lsp_format.LspFormat5263.test_tab_size_and_insert_spaces_are_honored",
    ),
    # #5259 reader-based symbol layer: comments/strings are not definitions,
    # the definition forms the runtime really has, local bindings, outline,
    # workspace symbols, folding, selection, and require links.
    (
        5259,
        "test_comment_and_string_defines_are_not_symbols",
        "lsp_symbols.LspSymbols5259.test_comment_and_string_defines_are_not_symbols",
    ),
    (
        5259,
        "test_definition_forms_and_the_two_that_are_not",
        "lsp_symbols.LspSymbols5259.test_definition_forms_and_the_two_that_are_not",
    ),
    (
        5259,
        "test_nested_define_is_a_child_of_its_enclosing_scope",
        "lsp_symbols.LspSymbols5259.test_nested_define_is_a_child_of_its_enclosing_scope",
    ),
    (
        5259,
        "test_let_and_lambda_bindings_definition_and_highlight",
        "lsp_symbols.LspSymbols5259.test_let_and_lambda_bindings_definition_and_highlight",
    ),
    (
        5259,
        "test_workspace_symbol_sees_the_project_and_required_files",
        "lsp_symbols.LspSymbols5259.test_workspace_symbol_sees_the_project_and_required_files",
    ),
    (
        5259,
        "test_folding_and_selection_ranges",
        "lsp_symbols.LspSymbols5259.test_folding_and_selection_ranges",
    ),
    (
        5259,
        "test_document_link_resolves_require_paths",
        "lsp_symbols.LspSymbols5259.test_document_link_resolves_require_paths",
    ),
    (
        5259,
        "test_capabilities_advertise_the_symbol_layer",
        "lsp_symbols.LspSymbols5259.test_capabilities_advertise_the_symbol_layer",
    ),
    # #5261: hover / signatureHelp / completion, first wave (no types).
    (
        5261,
        "test_hover_matches_repl_doc_for_primitives",
        "lsp_semantic.LspSemantic5261.test_hover_matches_repl_doc_for_primitives",
    ),
    (
        5261,
        "test_hover_on_a_user_define_shows_params_and_the_comment_above",
        "lsp_semantic.LspSemantic5261.test_hover_on_a_user_define_shows_params_and_the_comment_above",
    ),
    (
        5261,
        "test_hover_on_a_require_string_shows_the_resolved_path",
        "lsp_semantic.LspSemantic5261.test_hover_on_a_require_string_shows_the_resolved_path",
    ),
    (
        5261,
        "test_signature_help_marks_the_current_argument",
        "lsp_semantic.LspSemantic5261.test_signature_help_marks_the_current_argument",
    ),
    (
        5261,
        "test_completion_offers_primitives_defines_and_visible_bindings",
        "lsp_semantic.LspSemantic5261.test_completion_offers_primitives_defines_and_visible_bindings",
    ),
    (
        5261,
        "test_completion_lists_require_paths_from_root_std_and_aura_path",
        "lsp_semantic.LspSemantic5261.test_completion_lists_require_paths_from_root_std_and_aura_path",
    ),
    (
        5261,
        "test_capabilities_advertise_hover_signature_and_completion",
        "lsp_semantic.LspSemantic5261.test_capabilities_advertise_hover_signature_and_completion",
    ),
)


def load_tests(loader, tests, pattern):
    import importlib
    import sys

    suite = unittest.TestSuite()
    for _issue, _title, spec in CASES:
        bits = spec.split(".")
        if len(bits) == 3:
            mod = importlib.import_module(bits[0])
            suite.addTest(getattr(mod, bits[1])(bits[2]))
        else:
            suite.addTest(getattr(sys.modules[__name__], bits[0])(bits[1]))
    return suite


if __name__ == "__main__":
    unittest.main()
