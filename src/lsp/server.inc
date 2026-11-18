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
        notify("window/logMessage", std::format(R"({{"type":{},"message":{}}})", type, q(message)));
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
    std::string definition(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const int line = static_cast<int>(params.at("position").at("line").n);
        const auto character = static_cast<long long>(params.at("position").at("character").n);
        const std::string text = store.text_of(key);
        std::size_t line_off = 0;
        for (int k = 0; k < std::max(0, line); ++k) {
            const auto nl = text.find('\n', line_off);
            if (nl == std::string::npos)
                return "null";
            line_off = nl + 1;
        }
        const auto line_text = line_at(text, static_cast<std::size_t>(std::max(0, line)));
        const std::size_t byte = codec.to_byte(line_text, character);
        std::size_t p = std::min(text.size(), line_off + byte);
        std::size_t b = p, e = p;
        while (b > line_off && sym_char(text[b - 1]))
            --b;
        while (e < text.size() && sym_char(text[e]))
            ++e;
        if (b == e)
            return "null";
        const std::string word = text.substr(b, e - b);
        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        store.collect(key, files, seen);
        // Prefer this file's own define: collect() puts it last.
        if (!files.empty())
            std::ranges::rotate(files, files.end() - 1);
        for (auto& f : files) {
            const std::string t = store.text_of(f);
            for (std::string_view head :
                 {"(define (", "(define ", "(define-syntax ", "(define-record-type "}) {
                for (std::size_t at = t.find(head); at != std::string::npos;
                     at = t.find(head, at + 1)) {
                    std::size_t s = at + head.size();
                    while (s < t.size() && t[s] == ' ')
                        ++s;
                    std::size_t se = s;
                    while (se < t.size() && sym_char(t[se]))
                        ++se;
                    if (t.compare(s, se - s, word) != 0 || se - s != word.size())
                        continue;
                    const int dl = static_cast<int>(
                        std::count(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(s), '\n'));
                    const std::size_t ls = t.rfind('\n', s == 0 ? 0 : s - 1);
                    const std::size_t dc_byte = ls == std::string::npos || s == 0 ? s : s - ls - 1;
                    const auto def_line = line_at(t, static_cast<std::size_t>(dl));
                    const int start = codec.from_byte(def_line, dc_byte);
                    const int end = codec.from_byte(def_line, dc_byte + word.size());
                    return std::format(
                        R"({{"uri":{},"range":{{"start":{{"line":{},"character":{}}},"end":{{"line":{},"character":{}}}}}}})",
                        q(store.uri_of(f)), dl, start, dl, end);
                }
            }
        }
        return "null";
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
        const auto run =
            std::ranges::find_if(runs, [&](const FileDiagnostics& r) { return r.key == key; });
        const std::vector<Diag> empty;
        const auto& items = run == runs.end() ? empty : run->items;
        return std::format(R"({{"kind":"full","items":[{}]}})",
                           items_json(items, store.text_of(key)));
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
        if (const Json* opts = params.get("initializationOptions")) {
            if (const Json* ts = opts->get("tabSize"); ts && ts->t == Json::T::Num && ts->n >= 1)
                fmt_tab_size = static_cast<int>(std::min(8.0, ts->n));
            if (const Json* sp = opts->get("insertSpaces"); sp && sp->t == Json::T::Bool)
                fmt_insert_spaces = sp->b;
        }

        std::string root_path;
        const Json& root_uri = params.at("rootUri");
        if (root_uri.t == Json::T::Str && !root_uri.s.empty()) {
            root_path = uri_to_path(root_uri.s);
        } else if (const Json* folders = params.get("workspaceFolders");
                   folders && folders->t == Json::T::Arr && !folders->a.empty()) {
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
        reply(
            *id,
            std::format(
                R"({{"capabilities":{{"positionEncoding":{},"textDocumentSync":{{"openClose":true,"change":1,"save":{{"includeText":false}}}},"definitionProvider":true,"documentFormattingProvider":true,"documentRangeFormattingProvider":true,"diagnosticProvider":{{"identifier":"aura","interFileDependencies":true,"workspaceDiagnostics":false}}}},"serverInfo":{{"name":"aura-lsp","version":{}}}}})",
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
            send(
                std::format(R"({{"jsonrpc":"2.0","id":{},"error":{{"code":-32700,"message":{}}}}})",
                            id.value_or("null"),
                            q(parser.nesting_refused ? "nesting too deep" : "Parse error")));
            return std::nullopt;
        }
        if (m.t != Json::T::Obj) {
            // A top-level array is a batch request (unsupported: -32600
            // instead of the old silent drop); any other non-object is an
            // invalid request.
            send(std::format(
                R"({{"jsonrpc":"2.0","id":null,"error":{{"code":-32600,"message":{}}}}})",
                q(m.t == Json::T::Arr ? "batch requests are not supported" : "invalid request")));
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
        } else if (method == "textDocument/diagnostic") {
            if (id)
                reply(*id, pull_diagnostics(params));
        } else if (method == "textDocument/formatting") {
            if (id)
                reply(*id, format_edits(params, false));
        } else if (method == "textDocument/rangeFormatting") {
            if (id)
                reply(*id, format_edits(params, true));
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
