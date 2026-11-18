// The language server: dispatch, method handlers, debounce loop.
//
// Included last by src/cli_lsp_4911.inc. Transport/framing (json_rpc.inc),
// documents and URIs (document_store.inc), encoding (position.inc) and
// diagnostics (diagnostics.inc) are separate on purpose: A2–A6 touch the
// handlers here without reopening the protocol layer.
#ifndef AURA_LSP_SERVER_INC
#define AURA_LSP_SERVER_INC

#ifndef AURA_BUILD_VERSION
#define AURA_BUILD_VERSION "dev"
#endif

namespace lsp_4911 {

inline constexpr int kDefaultDebounceMs = 300;

struct Server {
    Transport io;
    DocumentStore store;
    DiagnosticsEngine engine{store};
    PositionCodec codec;
    // #5261: primitive signatures/docs come from the compiler registry (the
    // same one the REPL's :doc reads), and the three semantic handlers query
    // it together with the A2 symbol index.
    PrimRegistryView prims;
    Semantics sem{store, codec, prims};
    // key → the analysis last published/served for it
    std::unordered_map<std::string, std::vector<FileDiagnostics>> last;
    // main key → file keys currently carrying diagnostics from that analysis
    std::unordered_map<std::string, std::unordered_set<std::string>> published_for;
    std::unordered_set<std::string> published; // file keys with live diagnostics
    std::unordered_set<std::string> pending;   // documents waiting for the debounce
    std::chrono::steady_clock::time_point pending_due;
    int debounce_ms = kDefaultDebounceMs;
    bool initialized = false;

    Server() { debounce_ms = read_debounce_ms(); }

    static int read_debounce_ms() {
        const char* raw = aura::config::raw(aura::config::Key::AURA_LSP_DEBOUNCE_MS);
        if (!raw || !*raw)
            return kDefaultDebounceMs;
        const long v = std::strtol(raw, nullptr, 10);
        return v < 0 ? kDefaultDebounceMs : static_cast<int>(std::min<long>(v, 60000));
    }

    void send(const std::string& body) { (void)io.send(body); }

    void reply(const Json& id, const std::string& result) {
        send(std::format(R"({{"jsonrpc":"2.0","id":{},"result":{}}})", id_text(id), result));
    }

    void reply_error(const Json& id, int code, std::string_view message) {
        send(std::format(R"({{"jsonrpc":"2.0","id":{},"error":{{"code":{},"message":{}}}}})",
                         id_text(id), code, q(message)));
    }

    void notify(std::string_view method, const std::string& params) {
        send(std::format(R"({{"jsonrpc":"2.0","method":{},"params":{}}})", q(method), params));
    }

    void log_message(int type, std::string_view message) {
        notify("window/logMessage",
               std::format(R"({{"type":{},"message":{}}})", type, q(message)));
    }

    // ── diagnostics ────────────────────────────────────────────
    std::string items_json(const std::vector<Diag>& items, std::string_view text) const {
        std::string arr;
        for (const auto& d : items) {
            const auto line_text = line_at(text, static_cast<std::size_t>(d.line));
            const int start = codec.from_byte(line_text, static_cast<std::size_t>(d.col));
            const int end = codec.from_byte(line_text, static_cast<std::size_t>(d.end));
            if (!arr.empty())
                arr += ',';
            arr += std::format(
                R"({{"range":{{"start":{{"line":{},"character":{}}},"end":{{"line":{},"character":{}}}}},)"
                R"("severity":1,"source":"aura","code":{},"message":{}}})",
                d.line, start, d.line, end, q(std::string(aura::diag::kind_ident(d.kind))),
                q(d.msg));
        }
        return arr;
    }

    void publish_runs(const std::string& main_key, const std::vector<FileDiagnostics>& runs) {
        auto& previous = published_for[main_key];
        std::unordered_set<std::string> current;
        for (const auto& run : runs) {
            const bool was = published.contains(run.key);
            if (run.items.empty() && !was && run.key != main_key)
                continue;
            const std::string uri = store.uri_of(run.key);
            const std::string text = store.text_of(run.key);
            send(std::format(
                R"({{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{{"uri":{},"diagnostics":[{}]}}}})",
                q(uri), items_json(run.items, text)));
            if (run.items.empty())
                published.erase(run.key);
            else
                published.insert(run.key);
            current.insert(run.key);
        }
        // A file that left the require closure must lose its old diagnostics.
        for (const auto& stale : previous) {
            if (current.contains(stale))
                continue;
            send(std::format(
                R"({{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{{"uri":{},"diagnostics":[]}}}})",
                q(store.uri_of(stale))));
            published.erase(stale);
        }
        previous = std::move(current);
    }

    void analyze_and_publish(const std::string& key) {
        auto runs = engine.analyze(key);
        publish_runs(key, runs);
        last[key] = std::move(runs);
    }

    void flush_pending() {
        if (pending.empty())
            return;
        auto keys = std::move(pending);
        pending.clear();
        for (const auto& key : keys)
            analyze_and_publish(key);
    }

    void schedule(const std::string& key) {
        pending.insert(key);
        pending_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(debounce_ms);
    }

    void clear_diagnostics(const std::string& key, const std::string& uri) {
        pending.erase(key);
        last.erase(key);
        published_for.erase(key);
        send(std::format(
            R"({{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{{"uri":{},"diagnostics":[]}}}})",
            q(uri)));
    }

    // ── goto-definition ───────────────────────────────────────
    // #5259: the index is the reader's, so a name written in a comment or in a
    // string literal is not a definition, and a position inside one is null.
    std::string location_json(const std::string& key, Span s) const {
        const std::string text = store.text_of(key);
        const LineIndex li(text);
        return std::format(R"({{"uri":{},"range":{}}})", q(store.uri_of(key)),
                           range_json(text, li, codec, s));
    }

    std::string definition(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const int line = static_cast<int>(params.at("position").at("line").n);
        const auto character = static_cast<long long>(params.at("position").at("character").n);
        const std::string text = store.text_of(key);
        const LineIndex li(text);
        const std::size_t p = std::min(text.size(), byte_offset_at(text, li, codec, line, character));
        const DocIndex ix = DocIndex::build(text);
        if (!ix.inside_comment(p) && !ix.inside_string(p))
            if (const auto hit = ix.local_definition(p))
                return location_json(key, hit->span);
        const std::string word = word_at(text, p);
        if (word.empty())
            return "null";
        if (const int d = ix.top_def(word); d >= 0)
            return location_json(key, ix.defs[d].name_span);
        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        store.collect(key, files, seen);
        // Prefer this file's own define: collect() puts it last.
        if (!files.empty())
            std::ranges::rotate(files, files.end() - 1);
        for (auto& f : files) {
            const DocIndex other = DocIndex::build(store.text_of(f));
            if (const int d = other.top_def(word); d >= 0)
                return location_json(f, other.defs[d].name_span);
        }
        return "null";
    }

    // ── symbol layer (#5259) ──────────────────────────────────
    static std::string lowered(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // textDocument/documentHighlight: the declaration plus every reference
    // that resolves to the same binding (#5259, reader-level scope).
    std::string highlight(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const int line = static_cast<int>(params.at("position").at("line").n);
        const auto character = static_cast<long long>(params.at("position").at("character").n);
        const std::string text = store.text_of(key);
        const LineIndex li(text);
        const std::size_t p = std::min(text.size(), byte_offset_at(text, li, codec, line, character));
        const DocIndex ix = DocIndex::build(text);
        std::string arr;
        for (const auto& [span, kind] : ix.highlights(p)) {
            if (!arr.empty())
                arr += ',';
            arr += std::format(R"({{"range":{},"kind":{}}})", range_json(text, li, codec, span), kind);
        }
        return "[" + arr + "]";
    }

    std::string symbol_json(const DocIndex& ix, std::string_view text, const LineIndex& li,
                            int def) const {
        const auto& d = ix.defs[def];
        std::string out = std::format(
            R"({{"name":{},"detail":{},"kind":{},"range":{},"selectionRange":{}}})", q(d.name),
            q(d.detail), d.kind, range_json(text, li, codec, d.span),
            range_json(text, li, codec, d.name_span));
        if (!d.children.empty()) {
            out.pop_back(); // the closing brace
            out += R"(,"children":[)";
            for (std::size_t k = 0; k < d.children.size(); ++k) {
                if (k > 0)
                    out += ',';
                out += symbol_json(ix, text, li, d.children[k]);
            }
            out += "]}";
        }
        return out;
    }

    std::string document_symbol(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const std::string text = store.text_of(key);
        const LineIndex li(text);
        const DocIndex ix = DocIndex::build(text);
        std::string arr;
        for (const int d : ix.top) {
            if (!arr.empty())
                arr += ',';
            arr += symbol_json(ix, text, li, d);
        }
        return "[" + arr + "]";
    }

    std::string folding_ranges(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const std::string text = store.text_of(key);
        const LineIndex li(text);
        const DocIndex ix = DocIndex::build(text);
        std::string arr;
        for (const auto& [span, is_comment] : ix.folds()) {
            if (span.end <= span.begin)
                continue;
            const int l1 = li.line_of(span.begin);
            const int l2 = li.line_of(span.end - 1);
            if (l2 <= l1)
                continue;
            if (!arr.empty())
                arr += ',';
            arr += std::format(R"({{"startLine":{},"endLine":{},"kind":"{}"}})", l1, l2,
                               is_comment ? "comment" : "region");
        }
        return "[" + arr + "]";
    }

    std::string selection_range(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        // LSP sends a positions array; a single position is accepted too so a
        // hand-written probe does not need the wrapper.
        const Json& positions = params.at("positions");
        const Json& position = positions.t == Json::T::Arr && !positions.a.empty()
                                   ? positions.a.front()
                                   : params.at("position");
        const int line = static_cast<int>(position.at("line").n);
        const auto character = static_cast<long long>(position.at("character").n);
        const std::string text = store.text_of(key);
        const LineIndex li(text);
        const std::size_t p = std::min(text.size(), byte_offset_at(text, li, codec, line, character));
        const DocIndex ix = DocIndex::build(text);
        const auto chain = ix.chain_at(p);
        if (chain.empty())
            return "null";
        // The chain is outermost first; LSP wants the innermost range as the
        // element and each wider range under "parent", so wrap outward-in.
        std::string out;
        for (const int n : chain) {
            const std::string range = range_json(text, li, codec, ix.nodes[n].span);
            out = out.empty() ? std::format(R"({{"range":{}}})", range)
                              : std::format(R"({{"range":{},"parent":{}}})", range, out);
        }
        return "[" + out + "]";
    }

    std::string document_links(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const std::string text = store.text_of(key);
        const LineIndex li(text);
        const DocIndex ix = DocIndex::build(text);
        std::string arr;
        for (const auto& link : ix.links) {
            // The require graph and this link resolve through the same A1
            // function, so a link and a jump can never disagree.
            const auto target = store.resolve(key, link.spec);
            if (!target)
                continue;
            if (!arr.empty())
                arr += ',';
            arr += std::format(R"({{"range":{},"target":{}}})", range_json(text, li, codec, link.span),
                               q(store.uri_of(*target)));
        }
        return "[" + arr + "]";
    }

    // Files a workspace query looks at: the open documents, their require
    // closure, and the project root's .aura files.
    std::vector<std::string> workspace_files() const {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        std::vector<std::string> open_keys;
        for (const auto& [key, doc] : store.docs) {
            (void)doc;
            open_keys.push_back(key);
        }
        for (const auto& key : open_keys)
            if (seen.insert(key).second)
                out.push_back(key);
        for (const auto& key : open_keys) {
            std::vector<std::string> files;
            std::unordered_set<std::string> closure_seen;
            store.collect(key, files, closure_seen);
            for (const auto& f : files)
                if (seen.insert(f).second)
                    out.push_back(f);
        }
        if (!store.root.empty()) {
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(
                store.root, std::filesystem::directory_options::skip_permission_denied, ec);
            const std::filesystem::recursive_directory_iterator end;
            for (; !ec && it != end && out.size() < 2000; it.increment(ec)) {
                const auto& p = it->path();
                std::error_code dir_ec;
                if (it->is_directory(dir_ec)) {
                    const std::string name = p.filename().string();
                    if (name == ".git" || name == "build" || name == "node_modules" ||
                        name == ".cache")
                        it.disable_recursion_pending();
                    continue;
                }
                if (p.extension() != ".aura")
                    continue;
                const std::string key = normalize_path(p.string());
                if (seen.insert(key).second)
                    out.push_back(key);
            }
        }
        return out;
    }

    std::string workspace_symbol(const Json& params) {
        std::string query;
        if (const Json* qn = params.get("query"); qn && qn->t == Json::T::Str)
            query = lowered(qn->s);
        std::string arr;
        for (const auto& key : workspace_files()) {
            const std::string text = store.text_of(key);
            if (text.empty())
                continue;
            const LineIndex li(text);
            const DocIndex ix = DocIndex::build(text);
            for (int d = 0; d < static_cast<int>(ix.defs.size()); ++d) {
                const auto& sym = ix.defs[d];
                if (!query.empty() && lowered(sym.name).find(query) == std::string::npos)
                    continue;
                if (!arr.empty())
                    arr += ',';
                arr += std::format(
                    R"({{"name":{},"kind":{},"location":{{"uri":{},"range":{}}}}})", q(sym.name),
                    sym.kind, q(store.uri_of(key)), range_json(text, li, codec, sym.name_span));
            }
        }
        return "[" + arr + "]";
    }

    std::string pull_diagnostics(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        if (pending.contains(key)) {
            // A pull racing the debounce must see the edited buffer (and the
            // push stays consistent with it).
            pending.erase(key);
            analyze_and_publish(key);
        } else if (!last.contains(key)) {
            last[key] = engine.analyze(key);
        }
        const auto& runs = last[key];
        const auto run = std::ranges::find_if(runs, [&](const FileDiagnostics& r) { return r.key == key; });
        const std::vector<Diag> empty;
        const auto& items = run == runs.end() ? empty : run->items;
        return std::format(R"({{"kind":"full","items":[{}]}})", items_json(items, store.text_of(key)));
    }

    // ── formatting ────────────────────────────────────────────
    // One C++ formatter for both faces: cli_fmt_5263::reindent() is exactly
    // what "aura --fmt" runs, so the editor and the CLI cannot drift. No
    // shell out, no Aura primitive (#5002, #5263).
    //
    // Two deliberate limits:
    //   - a buffer the reader rejects is never rewritten. The reply is an
    //     empty TextEdit list plus one window/logMessage saying why; an error
    //     reply would paint a half-typed form red on every save, and a
    //     rewrite could destroy text the parser only misread.
    //   - rangeFormatting formats the whole document and then keeps the
    //     requested lines. Indentation is a property of a line and the
    //     formatter never adds, drops or reorders lines, so the line span
    //     maps 1:1; the edit range is widened to whole lines for the same
    //     reason. A client that needs a byte-tight range gets whole lines.
    int fmt_tab_size = 2;
    bool fmt_insert_spaces = true;

    struct FmtSpan {
        std::size_t begin = 0; // byte offset of the first line's start
        std::size_t end = 0;   // byte offset of the next line's start (or EOF)
        int first_line = 0;
        int last_line = 0;
    };

    static std::vector<std::size_t> line_starts(std::string_view t) {
        std::vector<std::size_t> starts{0};
        for (std::size_t i = 0; i < t.size(); ++i)
            if (t[i] == '\n')
                starts.push_back(i + 1);
        return starts;
    }

    // Whole lines [first, last] (0-based, clamped) as byte offsets.
    static FmtSpan line_span(std::string_view t, int first, int last) {
        const auto starts = line_starts(t);
        const int lines = static_cast<int>(starts.size());
        FmtSpan s;
        s.first_line = std::clamp(first, 0, lines - 1);
        s.last_line = std::clamp(last, s.first_line, lines - 1);
        s.begin = starts[static_cast<std::size_t>(s.first_line)];
        // The span ends where the *next* line begins, so a replacement carries
        // the newline that terminates the last line. The matching edit range
        // ends at (last + 1, 0); a newText without that newline would eat it.
        s.end =
            s.last_line + 1 < lines ? starts[static_cast<std::size_t>(s.last_line) + 1] : t.size();
        return s;
    }

    std::string format_edits(const Json& params, bool range_mode) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const std::string text = store.text_of(key);
        if (text.empty())
            return "[]";
        // FormattingOptions travel with the request; initializationOptions
        // hold the client's defaults for a request that omits them.
        int tab = fmt_tab_size;
        bool spaces = fmt_insert_spaces;
        if (const Json* opts = params.get("options")) {
            if (const Json* ts = opts->get("tabSize"); ts && ts->t == Json::T::Num && ts->n >= 1)
                tab = static_cast<int>(std::min(8.0, ts->n));
            if (const Json* sp = opts->get("insertSpaces"); sp && sp->t == Json::T::Bool)
                spaces = sp->b;
        }
        std::string err;
        if (!cli_fmt_5263::parses(text, &err)) {
            log_message(2, "aura lsp: not formatting " + key +
                               ": the reader rejects this buffer, so there is nothing safe to "
                               "rewrite (" +
                               err + ")");
            return "[]";
        }
        const std::string formatted = cli_fmt_5263::reindent(text, tab, spaces);
        const auto in_lines = line_starts(text);
        int first = 0;
        int last = static_cast<int>(in_lines.size()) - 1;
        if (range_mode) {
            const Json& range = params.at("range");
            first = static_cast<int>(range.at("start").at("line").n);
            last = static_cast<int>(range.at("end").at("line").n);
        }
        const FmtSpan in = line_span(text, first, last);
        // The formatter only rewrites leading whitespace, so line N of the
        // input is line N of the output and the requested span maps 1:1. If
        // that ever stops holding, fall back to one whole-document edit
        // instead of returning an edit whose lines do not correspond.
        const bool aligned = line_starts(formatted).size() == in_lines.size();
        const std::string before = aligned ? text.substr(in.begin, in.end - in.begin) : text;
        const FmtSpan out =
            line_span(formatted, aligned ? in.first_line : 0,
                      aligned ? in.last_line : static_cast<int>(in_lines.size()) - 1);
        const std::string after =
            aligned ? formatted.substr(out.begin, out.end - out.begin) : formatted;
        if (before == after)
            return "[]";
        const int lines = static_cast<int>(in_lines.size());
        const int last_line = aligned ? in.last_line : lines - 1;
        const bool has_next = last_line + 1 < lines;
        const int end_line = has_next ? last_line + 1 : last_line;
        // Ending at the next line's start needs character 0; at EOF it needs
        // the last line's full width.
        const auto end_line_text = line_at(text, static_cast<std::size_t>(end_line));
        const int end_char = has_next ? 0 : codec.from_byte(end_line_text, end_line_text.size());
        return std::format(
            R"([{{"range":{{"start":{{"line":{},"character":0}},"end":{{"line":{},"character":{}}}}},)"
            R"("newText":{}}}])",
            aligned ? in.first_line : 0, end_line, end_char, q(after));
    }


    // ── lifecycle / sync ──────────────────────────────────────
    void did_open(const Json& params) {
        const auto& td = params.at("textDocument");
        const std::string uri = td.at("uri").s;
        store.open(uri, td.at("text").s);
        analyze_and_publish(DocumentStore::key_of(uri));
    }

    void did_change(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const auto& changes = params.at("contentChanges");
        bool ranged = false;
        for (const auto& change : changes.a)
            if (change.get("range"))
                ranged = true;
        if (ranged) {
            // We advertise change: 1. Applying a range edit as if it were the
            // whole document is how the old server silently replaced the
            // buffer with a fragment; refuse it and say so (#5258).
            log_message(2, "aura lsp: incremental (range) change refused; this server uses full "
                           "synchronization (textDocumentSync.change = 1)");
        } else if (!changes.a.empty()) {
            const std::string text = changes.a.back().at("text").s;
            store.open(uri, text); // insert if the client skipped didOpen
        }
        schedule(key);
    }

    void did_save(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        if (const auto* text = params.get("text")) {
            store.open(uri, text->s);
        } else if (!store.find(key)) {
            // Not an open buffer: fall back to disk. An open buffer's text is
            // authoritative — re-reading the file used to undo unsaved edits.
            if (auto disk = read_file(key))
                store.open(uri, *disk);
        }
        pending.erase(key);
        analyze_and_publish(key);
    }

    void did_close(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        store.close(key);
        clear_diagnostics(key, uri);
    }

    void initialize(const Json& params, const Json* id) {
        const Json& encodings = params.at("capabilities").at("general").at("positionEncodings");
        bool wants_utf8 = false;
        for (const auto& e : encodings.a)
            if (e.t == Json::T::Str && e.s == "utf-8")
                wants_utf8 = true;
        codec.enc = wants_utf8 ? Encoding::Utf8 : Encoding::Utf16;

        // #5263: the client's default FormattingOptions. A formatting request
        // that carries its own options still wins (see format_edits).
        if (const Json* opts = params.get("initializationOptions"); opts && opts->t == Json::T::Obj) {
            if (const Json* ts = opts->get("tabSize"); ts && ts->t == Json::T::Num && ts->n >= 1)
                fmt_tab_size = static_cast<int>(std::min(8.0, ts->n));
            if (const Json* sp = opts->get("insertSpaces"); sp && sp->t == Json::T::Bool)
                fmt_insert_spaces = sp->b;
        }

        std::string root_path;
        const Json& root_uri = params.at("rootUri");
        if (root_uri.t == Json::T::Str && !root_uri.s.empty()) {
            root_path = uri_to_path(root_uri.s);
        } else if (const Json* folders = params.get("workspaceFolders"); folders && folders->t == Json::T::Arr &&
                                                                        !folders->a.empty()) {
            const Json& first_uri = folders->a.front().at("uri");
            if (first_uri.t == Json::T::Str)
                root_path = uri_to_path(first_uri.s);
        }
        if (!root_path.empty())
            store.root = project_root(normalize_path(root_path));
        initialized = true;

        const std::string encoding = codec.enc == Encoding::Utf8 ? "utf-8" : "utf-16";
        if (!id)
            return; // initialize as a notification: state is set, nothing to answer
        reply(*id, std::format(
            R"({{"capabilities":{{"positionEncoding":{},"textDocumentSync":{{"openClose":true,"change":1,"save":{{"includeText":false}}}},"definitionProvider":true,"hoverProvider":true,"signatureHelpProvider":{{"triggerCharacters":["("," "]}},"completionProvider":{{"triggerCharacters":["("," ","\""],"resolveProvider":false}},"documentFormattingProvider":true,"documentRangeFormattingProvider":true,"documentHighlightProvider":true,"documentSymbolProvider":true,"workspaceSymbolProvider":true,"foldingRangeProvider":true,"selectionRangeProvider":true,"documentLinkProvider":{{"resolveProvider":false}},"diagnosticProvider":{{"identifier":"aura","interFileDependencies":true,"workspaceDiagnostics":false}}}},"serverInfo":{{"name":"aura-lsp","version":{}}}}})",
            q(encoding), q(AURA_BUILD_VERSION)));
    }

    // ── JSON-RPC dispatch ─────────────────────────────────────
    // Returns the process exit code when `exit` arrives, nullopt otherwise.
    std::optional<int> dispatch(std::string_view body, bool& shutdown) {
        Json m;
        Parser parser{body};
        if (!parser.parse(m)) {
            // #5258: malformed JSON is -32700, not silence. The id survives
            // when the header was parsed before the failure (#5155 keeps the
            // "nesting too deep" wording).
            const std::optional<std::string> id =
                m.get("id") ? std::optional<std::string>(id_text(*m.get("id"))) : std::nullopt;
            send(std::format(R"({{"jsonrpc":"2.0","id":{},"error":{{"code":-32700,"message":{}}}}})",
                             id.value_or("null"),
                             q(parser.nesting_refused ? "nesting too deep" : "Parse error")));
            return std::nullopt;
        }
        if (m.t != Json::T::Obj) {
            // A top-level array is a batch request (unsupported: -32600
            // instead of the old silent drop); any other non-object is an
            // invalid request.
            send(std::format(R"({{"jsonrpc":"2.0","id":null,"error":{{"code":-32600,"message":{}}}}})",
                             q(m.t == Json::T::Arr ? "batch requests are not supported"
                                                   : "invalid request")));
            return std::nullopt;
        }
        const Json* id = m.get("id");
        const std::string method = m.at("method").s;
        const Json& params = m.at("params");
        if (method.empty()) {
            if (id)
                reply_error(*id, -32600, "missing method");
            return std::nullopt;
        }
        if (method == "initialize") {
            // #5258: an initialize without an id is a notification. The old
            // code dereferenced the null id and died with SIGSEGV.
            initialize(params, id);
        } else if (method == "initialized") {
            // notification, nothing to do
        } else if (method == "shutdown") {
            shutdown = true;
            if (id)
                reply(*id, "null");
        } else if (method == "exit") {
            return shutdown ? 0 : 1;
        } else if (method == "textDocument/didOpen") {
            did_open(params);
        } else if (method == "textDocument/didChange") {
            did_change(params);
        } else if (method == "textDocument/didSave") {
            did_save(params);
        } else if (method == "textDocument/didClose") {
            did_close(params);
        } else if (method == "textDocument/definition") {
            if (id)
                reply(*id, definition(params));
        } else if (method == "textDocument/hover") {
            if (id)
                reply(*id, sem.hover(params));
        } else if (method == "textDocument/signatureHelp") {
            if (id)
                reply(*id, sem.signature_help(params));
        } else if (method == "textDocument/completion") {
            if (id)
                reply(*id, sem.completion(params));
        } else if (method == "textDocument/documentSymbol") {
            if (id)
                reply(*id, document_symbol(params));
        } else if (method == "workspace/symbol") {
            if (id)
                reply(*id, workspace_symbol(params));
        } else if (method == "textDocument/foldingRange") {
            if (id)
                reply(*id, folding_ranges(params));
        } else if (method == "textDocument/selectionRange") {
            if (id)
                reply(*id, selection_range(params));
        } else if (method == "textDocument/documentLink") {
            if (id)
                reply(*id, document_links(params));
        } else if (method == "textDocument/documentHighlight") {
            if (id)
                reply(*id, highlight(params));
        } else if (method == "textDocument/formatting") {
            if (id)
                reply(*id, format_edits(params, false));
        } else if (method == "textDocument/rangeFormatting") {
            if (id)
                reply(*id, format_edits(params, true));
        } else if (method == "textDocument/diagnostic") {
            if (id)
                reply(*id, pull_diagnostics(params));
        } else if (id) {
            reply_error(*id, -32601, "method not found: " + method);
        }
        return std::nullopt;
    }

    int run() {
        bool shutdown = false;
        for (;;) {
            int timeout = -1;
            if (!pending.empty()) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      pending_due - std::chrono::steady_clock::now())
                                      .count();
                timeout = left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, 1000));
            }
            if (!io.wait_input(timeout)) {
                flush_pending();
                continue;
            }
            const auto body = io.read_message();
            if (!body) {
                flush_pending();
                break;
            }
            if (body->empty())
                continue;
            if (const auto code = dispatch(*body, shutdown))
                return *code;
        }
        return shutdown ? 0 : 1;
    }
};

} // namespace lsp_4911

#endif // AURA_LSP_SERVER_INC
