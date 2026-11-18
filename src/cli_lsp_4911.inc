// Issue #4911: minimal Language Server (`aura lsp`, JSON-RPC over stdio).
//
//   - diagnostics on open/save/change: in-process `--typecheck` over the file
//     with its (require …) modules appended, so names from required files
//     resolve and a diagnostic inside a required file is published on that
//     file;
//   - goto-definition across (require "…"): top-level (define …) lookup;
//   - highlighting ships as a TextMate grammar (editors/grammar).
//
// #5258 split this file into four layers so A2–A6 can grow independently:
//
//   lsp/json_rpc.inc       JSON model, parser, Content-Length framing
//   lsp/position.inc       positionEncoding negotiation and conversion
//   lsp/document_store.inc URI normalization, documents, (require …) graph
//   lsp/symbols.inc        reader-based symbol index (#5259)
//   lsp/diagnostics.inc    typecheck of the document's require closure
//   lsp/prims.inc          primitive signatures/docs from the registry (#5261)
//   lsp/semantics.inc      hover / signatureHelp / completion (#5261)
//   lsp/refactor.inc       references / prepareRename / rename (#5260)
//   lsp/server.inc         dispatch, method handlers, debounce loop
//
// Pure std; no network, no extra dependencies.

#include "lsp/json_rpc.inc"
#include "lsp/position.inc"
#include "lsp/symbols.inc" // #5351: DocIndex first, the store caches one
#include "lsp/document_store.inc"
#include "lsp/diagnostics.inc"
#include "lsp/prims.inc"     // #5261: the compiler's primitive registry
#include "lsp/semantics.inc" // #5261: hover, signatureHelp, completion
#include "lsp/refactor.inc"  // #5260: references, prepareRename, rename
#include "lsp/server.inc"

static int aura_lsp_4911() {
    // Protocol frames go to the original stdout; anything the compiler prints
    // is diverted to stderr so it cannot corrupt the stream.
    lsp_4911::Server s;
    s.io.in_fd = STDIN_FILENO;
    s.io.out_fd = ::dup(STDOUT_FILENO);
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    std::ios::sync_with_stdio(false);
    return s.run();
}
