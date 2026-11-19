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
# 180s was enough for the protocol/format/symbol/semantic themes; the
# references+rename cases (#5260) push a full run past 160s, so the budget is
# explicit headroom rather than a pass/fail coincidence.
# #5346/#5347 added six more server round-trips on top of #5262's diagnostics
# cases: the last green dist run (a97ad49f) spent 291.8s of this 300s budget
# for 48 cases, and the next push died at exactly 300s (rc=124, 28/52). The
# 52 cases take 306.6s locally on an idle box, so 300 was below the work
# itself, not a margin. Give the heavy theme the same 900s headroom as ci/p0;
# the run still finishes on its own and a hang is caught by this cap.
PROCESS_TIMEOUT = 900
PARTS = (
    "lsp_protocol",
    "lsp_format",
    "lsp_symbols",
    "lsp_semantic",
    "lsp_refactor",
    "lsp_diagnostics",
)
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
    # #5359: eglot declares textDocument.diagnostic (pull) and also reads
    # publishDiagnostics, so the same set was delivered twice. One channel.
    (
        5359,
        "test_pull_client_gets_pull_only_and_push_client_keeps_the_push",
        "lsp_protocol.LspProtocol5359.test_pull_client_gets_pull_only_and_push_client_keeps_the_push",
    ),
    (
        5359,
        "test_diagnostic_capability_must_be_an_object",
        "lsp_protocol.LspProtocol5359.test_diagnostic_capability_must_be_an_object",
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
    # #5260: references across requires, prepareRename/rename as a
    # WorkspaceEdit (text only, never mutate:*), and the refusals.
    (
        5260,
        "test_references_list_the_declaration_and_cross_require_uses",
        "lsp_refactor.LspRefactor5260.test_references_list_the_declaration_and_cross_require_uses",
    ),
    (
        5260,
        "test_rename_produces_a_three_file_workspace_edit_and_the_program_runs",
        "lsp_refactor.LspRefactor5260.test_rename_produces_a_three_file_workspace_edit_and_the_program_runs",
    ),
    (
        5260,
        "test_comment_string_and_shadowed_local_are_not_references",
        "lsp_refactor.LspRefactor5260.test_comment_string_and_shadowed_local_are_not_references",
    ),
    (
        5260,
        "test_primitive_and_std_names_are_refused",
        "lsp_refactor.LspRefactor5260.test_primitive_and_std_names_are_refused",
    ),
    (
        5260,
        "test_illegal_and_conflicting_new_names_are_refused",
        "lsp_refactor.LspRefactor5260.test_illegal_and_conflicting_new_names_are_refused",
    ),
    (
        5260,
        "test_references_follow_aura_path_and_std_require",
        "lsp_refactor.LspRefactor5260.test_references_follow_aura_path_and_std_require",
    ),
    (
        5260,
        "test_capabilities_advertise_references_and_rename",
        "lsp_refactor.LspRefactor5260.test_capabilities_advertise_references_and_rename",
    ),
    # #5262: structured diagnostics (code/severity/relatedInformation), type
    # warnings with a configuration switch, and the did-you-mean quickfix.
    (
        5262,
        "test_codes_severity_and_message_match_the_json_faces",
        "lsp_diagnostics.LspDiagnostics5262.test_codes_severity_and_message_match_the_json_faces",
    ),
    (
        5262,
        "test_unbound_arity_and_parse_errors_are_errors",
        "lsp_diagnostics.LspDiagnostics5262.test_unbound_arity_and_parse_errors_are_errors",
    ),
    (
        5262,
        "test_type_diagnostic_is_a_warning_that_matches_typecheck",
        "lsp_diagnostics.LspDiagnostics5262.test_type_diagnostic_is_a_warning_that_matches_typecheck",
    ),
    (
        5262,
        "test_type_mode_off_and_error_from_both_configuration_channels",
        "lsp_diagnostics.LspDiagnostics5262.test_type_mode_off_and_error_from_both_configuration_channels",
    ),
    (
        5262,
        "test_require_chain_is_related_information",
        "lsp_diagnostics.LspDiagnostics5262.test_require_chain_is_related_information",
    ),
    (
        5262,
        "test_did_you_mean_quickfix_replaces_the_name",
        "lsp_diagnostics.LspDiagnostics5262.test_did_you_mean_quickfix_replaces_the_name",
    ),
    # #5346: a type warning must not be the only diagnostic a file publishes.
    (
        5346,
        "test_type_warning_does_not_mask_unbound_or_its_quickfix",
        "lsp_diagnostics.LspDiagnostics5262.test_type_warning_does_not_mask_unbound_or_its_quickfix",
    ),
    (
        5346,
        "test_type_warning_does_not_mask_arity_mismatch",
        "lsp_diagnostics.LspDiagnostics5262.test_type_warning_does_not_mask_arity_mismatch",
    ),
    (
        5346,
        "test_two_unbound_names_and_type_diagnostics_off",
        "lsp_diagnostics.LspDiagnostics5262.test_two_unbound_names_and_type_diagnostics_off",
    ),
    (
        5346,
        "test_per_file_cap_keeps_the_earliest_errors",
        "lsp_diagnostics.LspDiagnostics5262.test_per_file_cap_keeps_the_earliest_errors",
    ),
    # #5347: a (require …) must not shift "opened at line N" off the file's line.
    (
        5347,
        "test_require_std_string_unclosed_line_matches_range_and_cli",
        "lsp_diagnostics.LspDiagnostics5262.test_require_std_string_unclosed_line_matches_range_and_cli",
    ),
    (
        5347,
        "test_unclosed_without_require_line_matches_range",
        "lsp_diagnostics.LspDiagnostics5262.test_unclosed_without_require_line_matches_range",
    ),
    (
        5347,
        "test_unclosed_in_required_file_uses_that_files_line",
        "lsp_diagnostics.LspDiagnostics5262.test_unclosed_in_required_file_uses_that_files_line",
    ),
    # #5362: a wrong argument count is a diagnostic at the call site, like
    # the runtime and the JSON faces say.
    (
        5362,
        "test_arity_mismatch_in_a_nested_call_reports_the_call_site",
        "lsp_diagnostics.LspDiagnostics5362.test_arity_mismatch_in_a_nested_call_reports_the_call_site",
    ),
    # #5364: readable UnboundVariable message, JSON boolean typeDiagnostics,
    # and the http-post decision (a deferred std/net primitive — reported).
    (
        5364,
        "test_unbound_message_is_readable_and_the_filter_still_keys_on_the_name",
        "lsp_diagnostics.LspDiagnostics5364.test_unbound_message_is_readable_and_the_filter_still_keys_on_the_name",
    ),
    (
        5364,
        "test_type_diagnostics_accepts_json_booleans",
        "lsp_diagnostics.LspDiagnostics5364.test_type_diagnostics_accepts_json_booleans",
    ),
    (
        5364,
        "test_http_post_without_std_net_is_unbound_on_every_face",
        "lsp_diagnostics.LspDiagnostics5364.test_http_post_without_std_net_is_unbound_on_every_face",
    ),
    # #5348: tabSize is a tab width; the body indent stays two columns.
    (
        5348,
        "test_tab_size_is_not_the_body_indent",
        "lsp_format.LspFormat5263.test_tab_size_is_not_the_body_indent",
    ),
    # #5352: the symbol requests scale with the file (was quadratic).
    (
        5352,
        "test_symbol_queries_scale_with_the_file",
        "lsp_symbols.LspSymbols5259.test_symbol_queries_scale_with_the_file",
    ),
    # #5360: a symbol declared by a required file — signatureHelp, hover range
    # and documentHighlight look through the require closure.
    (
        5360,
        "test_signature_help_and_hover_range_follow_a_required_define",
        "lsp_semantic.LspSemantic5261.test_signature_help_and_hover_range_follow_a_required_define",
    ),
    (
        5360,
        "test_highlight_collects_cross_file_uses_in_this_document",
        "lsp_symbols.LspSymbols5259.test_highlight_collects_cross_file_uses_in_this_document",
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
