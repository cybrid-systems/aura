// Issue #4911: minimal Language Server (`aura lsp`, JSON-RPC over stdio).
//
//   - diagnostics on open/save: in-process `--typecheck` over the file with
//     its (require …) modules appended, so names from required files resolve
//     and a diagnostic inside a required file is published on that file;
//   - goto-definition across (require "…"): top-level (define …) lookup;
//   - highlighting ships as a TextMate grammar (editors/vscode).
// Pure std; no network, no extra dependencies.

namespace lsp_4911 {

struct Json {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json* get(std::string_view k) const {
        for (auto& [kk, v] : o)
            if (kk == k)
                return &v;
        return nullptr;
    }
    const Json& at(std::string_view k) const {
        static const Json null;
        auto* p = get(k);
        return p ? *p : null;
    }
};

struct Parser {
    std::string_view s;
    std::size_t i = 0;
    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    bool parse(Json& out) {
        ws();
        if (i >= s.size())
            return false;
        const char c = s[i];
        if (c == '{') {
            out.t = Json::T::Obj;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}')
                return ++i, true;
            while (true) {
                Json k;
                ws();
                if (!parse(k) || k.t != Json::T::Str)
                    return false;
                ws();
                if (i >= s.size() || s[i++] != ':')
                    return false;
                Json v;
                if (!parse(v))
                    return false;
                out.o.emplace_back(std::move(k.s), std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                return i < s.size() && s[i++] == '}';
            }
        }
        if (c == '[') {
            out.t = Json::T::Arr;
            ++i;
            ws();
            if (i < s.size() && s[i] == ']')
                return ++i, true;
            while (true) {
                Json v;
                if (!parse(v))
                    return false;
                out.a.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                return i < s.size() && s[i++] == ']';
            }
        }
        if (c == '"') {
            out.t = Json::T::Str;
            for (++i; i < s.size() && s[i] != '"'; ++i) {
                if (s[i] != '\\') {
                    out.s += s[i];
                    continue;
                }
                if (++i >= s.size())
                    return false;
                switch (s[i]) {
                    case 'n':
                        out.s += '\n';
                        break;
                    case 't':
                        out.s += '\t';
                        break;
                    case 'r':
                        out.s += '\r';
                        break;
                    case 'b':
                        out.s += '\b';
                        break;
                    case 'f':
                        out.s += '\f';
                        break;
                    case 'u': {
                        if (i + 4 >= s.size())
                            return false;
                        unsigned cp = 0;
                        std::from_chars(s.data() + i + 1, s.data() + i + 5, cp, 16);
                        i += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() && s[i + 1] == '\\' &&
                            s[i + 2] == 'u') {
                            unsigned lo = 0;
                            std::from_chars(s.data() + i + 3, s.data() + i + 7, lo, 16);
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                        auto put = [&](unsigned v) { out.s += static_cast<char>(v); };
                        if (cp < 0x80) {
                            put(cp);
                        } else if (cp < 0x800) {
                            put(0xC0 | (cp >> 6));
                            put(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            put(0xE0 | (cp >> 12));
                            put(0x80 | ((cp >> 6) & 0x3F));
                            put(0x80 | (cp & 0x3F));
                        } else {
                            put(0xF0 | (cp >> 18));
                            put(0x80 | ((cp >> 12) & 0x3F));
                            put(0x80 | ((cp >> 6) & 0x3F));
                            put(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        out.s += s[i];
                }
            }
            return i < s.size() && s[i++] == '"';
        }
        if (s.substr(i, 4) == "true")
            return out.t = Json::T::Bool, out.b = true, i += 4, true;
        if (s.substr(i, 5) == "false")
            return out.t = Json::T::Bool, i += 5, true;
        if (s.substr(i, 4) == "null")
            return i += 4, true;
        const std::size_t st = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' ||
                                s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            ++i;
        if (st == i)
            return false;
        out.t = Json::T::Num;
        out.n = std::strtod(std::string(s.substr(st, i - st)).c_str(), nullptr);
        return true;
    }
};

inline std::string q(std::string_view s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                o += "\\\"";
                break;
            case '\\':
                o += "\\\\";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                if (c < 0x20)
                    o += std::format("\\u{:04x}", static_cast<unsigned>(c));
                else
                    o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

// Re-encode a parsed id (number or string) for the response.
inline std::string id_text(const Json& id) {
    if (id.t == Json::T::Str)
        return q(id.s);
    if (id.t == Json::T::Num)
        return std::format("{}", static_cast<long long>(id.n));
    return "null";
}

inline std::string uri_to_path(std::string_view uri) {
    if (uri.starts_with("file://"))
        uri.remove_prefix(7);
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            unsigned v = 0;
            std::from_chars(uri.data() + i + 1, uri.data() + i + 3, v, 16);
            out += static_cast<char>(v);
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}
inline std::string path_to_uri(const std::string& p) {
    std::string out = "file://";
    for (unsigned char c : p) {
        if (std::isalnum(c) ||
            std::string_view("/-_.~").find(static_cast<char>(c)) != std::string_view::npos)
            out += static_cast<char>(c);
        else
            out += std::format("%{:02X}", static_cast<unsigned>(c));
    }
    return out;
}

inline std::optional<std::string> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

inline bool sym_char(char c) {
    return !std::isspace(static_cast<unsigned char>(c)) &&
           std::string_view("()[]'\"`,;").find(c) == std::string_view::npos;
}

struct Require {
    std::size_t begin = 0, end = 0; // byte range of the (require …) form
    std::string spec;
};

// Top-level-ish (require "x") forms; strings and comments are skipped.
inline std::vector<Require> find_requires(std::string_view s) {
    std::vector<Require> out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ';') {
            while (i < s.size() && s[i] != '\n')
                ++i;
            continue;
        }
        if (s[i] == '"') {
            for (++i; i < s.size() && s[i] != '"'; ++i)
                if (s[i] == '\\')
                    ++i;
            continue;
        }
        if (s[i] != '(' || s.substr(i + 1, 8) != "require ")
            continue;
        std::size_t j = i + 9;
        while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j])))
            ++j;
        if (j >= s.size() || s[j] != '"')
            continue;
        const std::size_t k = s.find('"', j + 1);
        if (k == std::string_view::npos)
            break;
        std::size_t e = k + 1;
        while (e < s.size() && s[e] != ')')
            ++e;
        out.push_back({i, std::min(e + 1, s.size()), std::string(s.substr(j + 1, k - j - 1))});
        i = e;
    }
    return out;
}

inline std::filesystem::path project_root(std::filesystem::path dir) {
    for (auto d = dir; !d.empty(); d = d.parent_path()) {
        std::error_code ec;
        if (std::filesystem::exists(d / "aura.toml", ec))
            return d;
        if (d == d.parent_path())
            break;
    }
    return dir;
}

// Resolve like the runtime: relative to the requiring file, then the project
// root (aura.toml), with or without the .aura extension.
inline std::optional<std::string> resolve(const std::string& from, const std::string& spec) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(from).parent_path();
    for (const fs::path& base : {dir, project_root(dir)}) {
        for (const fs::path& cand : {base / spec, base / (spec + ".aura")}) {
            std::error_code ec;
            if (fs::is_regular_file(cand, ec))
                return fs::weakly_canonical(cand, ec).string();
        }
    }
    return std::nullopt;
}

struct Server {
    int out_fd = 1;
    std::unordered_map<std::string, std::string> docs; // uri → text
    std::unordered_set<std::string> published;         // uris with live diagnostics

    std::string text_of(const std::string& path) {
        if (auto it = docs.find(path_to_uri(path)); it != docs.end())
            return it->second;
        return read_file(path).value_or("");
    }

    void send(const std::string& body) {
        const std::string msg = std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
        std::size_t off = 0;
        while (off < msg.size()) {
            const auto n = ::write(out_fd, msg.data() + off, msg.size() - off);
            if (n <= 0)
                return;
            off += static_cast<std::size_t>(n);
        }
    }
    void reply(const Json& id, const std::string& result) {
        send(std::format(R"({{"jsonrpc":"2.0","id":{},"result":{}}})", id_text(id), result));
    }

    // Collect `path` and its requires, dependencies first (post-order, each
    // file once): a required module's defines must precede the top-level
    // uses in the requiring file, as at runtime.
    void collect(const std::string& path, std::vector<std::string>& order,
                 std::unordered_set<std::string>& seen, int depth = 0) {
        if (depth > 32 || !seen.insert(path).second)
            return;
        for (auto& r : find_requires(text_of(path)))
            if (auto p = resolve(path, r.spec))
                collect(*p, order, seen, depth + 1);
        order.push_back(path);
    }

    // Top-level (define NAME / (define (NAME …: what goto-definition indexes.
    static std::vector<std::string> top_level_names(const std::string& t) {
        std::vector<std::string> out;
        for (std::string_view head :
             {"(define (", "(define ", "(define-syntax ", "(define-record-type "})
            for (std::size_t at = t.find(head); at != std::string::npos;
                 at = t.find(head, at + 1)) {
                std::size_t b = at + head.size(), e = b;
                while (e < t.size() && sym_char(t[e]))
                    ++e;
                if (e > b)
                    out.emplace_back(t.substr(b, e - b));
            }
        return out;
    }

    // One runtime session (std prelude loaded) answers "is NAME bound?".
    std::unique_ptr<aura::compiler::CompilerService> rt;
    std::unordered_map<std::string, bool> bound_cache;
    bool runtime_bound(const std::string& name) {
        if (name.empty() || !std::ranges::all_of(name, sym_char))
            return false;
        if (auto it = bound_cache.find(name); it != bound_cache.end())
            return it->second;
        bool ok = false;
        try {
            if (!rt) {
                rt = std::make_unique<aura::compiler::CompilerService>();
                (void)rt->eval("(require \"std/list\" all:)(require \"std/string\" all:)"
                               "(require \"std/hash\" all:)(require \"std/math\" all:)");
            }
            ok = static_cast<bool>(rt->eval(name));
        } catch (const std::exception&) {
            ok = false;
        }
        bound_cache[name] = ok;
        return ok;
    }

    bool keep_diag(const std::string& msg, const std::unordered_set<std::string>& defined) {
        if (msg.starts_with("type error"))
            return false;
        // The static signature table lags runtime primitives with optional
        // arguments (mutate:rebind name src [reason]); trust the runtime for
        // callees the project does not define itself.
        static constexpr std::string_view kArity = "arity mismatch: call '";
        if (msg.starts_with(kArity)) {
            const auto rest = msg.substr(kArity.size());
            const auto callee = rest.substr(0, rest.find('\''));
            return defined.contains(callee) || !runtime_bound(callee);
        }
        static constexpr std::string_view kUnbound = "unbound variable: ";
        if (!msg.starts_with(kUnbound))
            return true;
        auto name = msg.substr(kUnbound.size());
        name = name.substr(0, name.find('\n'));
        return !defined.contains(name) && !runtime_bound(name);
    }

    struct Diag {
        int line = 0, col = 0; // 0-based
        std::string msg;
    };

    void diagnose(const std::string& uri) {
        const std::string main_path = uri_to_path(uri);
        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        collect(main_path, files, seen);
        // Masked texts (requires → spaces, newlines kept) concatenated; each
        // segment remembers its first line so diagnostics map back.
        std::string combined;
        std::vector<std::pair<int, std::string>> segs; // first line (1-based) → path
        int line = 1;
        for (auto& f : files) {
            std::string t = text_of(f);
            for (auto& r : find_requires(t))
                for (std::size_t i = r.begin; i < r.end; ++i)
                    if (t[i] != '\n')
                        t[i] = ' ';
            if (!t.empty() && t.back() != '\n')
                t += '\n';
            segs.emplace_back(line, f);
            line += static_cast<int>(std::ranges::count(t, '\n'));
            combined += t;
        }
        std::unordered_map<std::string, std::vector<Diag>> by_file;
        for (auto& f : files)
            by_file[f];
        std::string output;
        try {
            aura::compiler::CompilerService cs;
            output = cs.typecheck_full(combined).output;
        } catch (const std::exception& e) {
            output = std::format("1:1: typecheck failed: {}\n", e.what());
        }
        std::istringstream in(output);
        std::string l;
        Diag* last = nullptr;
        static const std::regex diag_re(R"(^(?:parse error: )?(\d+):(\d+): (.*)$)");
        while (std::getline(in, l)) {
            std::smatch m;
            if (std::regex_match(l, m, diag_re)) {
                const int gl = std::stoi(m[1].str());
                auto seg = std::ranges::upper_bound(segs, gl, {}, [](auto& p) { return p.first; });
                if (seg == segs.begin()) {
                    last = nullptr;
                    continue;
                }
                --seg;
                auto& v = by_file[seg->second];
                v.push_back({gl - seg->first, std::max(0, std::stoi(m[2].str()) - 1), m[3].str()});
                last = &v.back();
            } else if (last && l.starts_with("  ")) {
                last->msg += "\n" + l.substr(2); // e.g. "did you mean …"
            } else {
                last = nullptr;
            }
        }
        // The static checker is stricter than the runtime: type errors on
        // dynamically typed code and "unbound" for runtime primitives / std
        // prelude names / forward references are noise in an editor. Keep
        // parse errors and unbound names that really are unbound at runtime.
        std::unordered_set<std::string> defined;
        for (auto& f : files)
            for (auto& n : top_level_names(text_of(f)))
                defined.insert(n);
        for (auto& [path, diags] : by_file)
            std::erase_if(diags, [&](const Diag& d) { return !keep_diag(d.msg, defined); });
        for (auto& [path, diags] : by_file) {
            const std::string furi = path_to_uri(path);
            if (diags.empty() && !published.contains(furi) && furi != uri)
                continue;
            const std::string text = text_of(path);
            std::string arr;
            for (auto& d : diags) {
                // Underline the symbol at the reported column.
                std::size_t off = 0;
                for (int k = 0; k < d.line && off != std::string::npos; ++k)
                    off = text.find('\n', off) == std::string::npos ? std::string::npos
                                                                    : text.find('\n', off) + 1;
                int end = d.col + 1;
                if (off != std::string::npos) {
                    std::size_t p = off + static_cast<std::size_t>(d.col);
                    if (p < text.size() && text[p] == '(')
                        ++p;
                    std::size_t e = p;
                    while (e < text.size() && sym_char(text[e]))
                        ++e;
                    end = static_cast<int>(e - off);
                    if (end <= d.col)
                        end = d.col + 1;
                }
                if (!arr.empty())
                    arr += ',';
                arr += std::format(
                    R"({{"range":{{"start":{{"line":{},"character":{}}},"end":{{"line":{},"character":{}}}}},"severity":1,"source":"aura","message":{}}})",
                    d.line, d.col, d.line, end, q(d.msg));
            }
            if (diags.empty())
                published.erase(furi);
            else
                published.insert(furi);
            send(std::format(
                R"({{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{{"uri":{},"diagnostics":[{}]}}}})",
                q(furi), arr));
        }
    }

    // goto-definition: the symbol under the cursor → its top-level define in
    // this file or any file it (transitively) requires.
    std::string definition(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const int line = static_cast<int>(params.at("position").at("line").n);
        const int ch = static_cast<int>(params.at("position").at("character").n);
        const std::string path = uri_to_path(uri);
        const std::string text = text_of(path);
        std::size_t off = 0;
        for (int k = 0; k < line; ++k) {
            off = text.find('\n', off);
            if (off == std::string::npos)
                return "null";
            ++off;
        }
        std::size_t p = std::min(text.size(), off + static_cast<std::size_t>(ch));
        std::size_t b = p, e = p;
        while (b > off && sym_char(text[b - 1]))
            --b;
        while (e < text.size() && sym_char(text[e]))
            ++e;
        if (b == e)
            return "null";
        const std::string word = text.substr(b, e - b);
        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        collect(path, files, seen);
        // Prefer this file's own define: collect() puts it last.
        if (!files.empty())
            std::ranges::rotate(files, files.end() - 1);
        for (auto& f : files) {
            const std::string t = text_of(f);
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
                    const int dl = static_cast<int>(std::count(t.begin(), t.begin() + s, '\n'));
                    const std::size_t ls = t.rfind('\n', s == 0 ? 0 : s - 1);
                    const int dc =
                        static_cast<int>(ls == std::string::npos || s == 0 ? s : s - ls - 1);
                    return std::format(
                        R"({{"uri":{},"range":{{"start":{{"line":{},"character":{}}},"end":{{"line":{},"character":{}}}}}}})",
                        q(path_to_uri(f)), dl, dc, dl, dc + static_cast<int>(word.size()));
                }
            }
        }
        return "null";
    }

    // One JSON-RPC message from stdin (Content-Length framing).
    static std::optional<std::string> read_message() {
        std::size_t len = 0;
        bool any = false;
        std::string line;
        while (std::getline(std::cin, line)) {
            any = true;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                break;
            if (line.starts_with("Content-Length:"))
                len = static_cast<std::size_t>(std::strtoull(line.c_str() + 15, nullptr, 10));
        }
        if (!any || !std::cin)
            return std::nullopt;
        std::string body(len, '\0');
        std::cin.read(body.data(), static_cast<std::streamsize>(len));
        if (static_cast<std::size_t>(std::cin.gcount()) != len)
            return std::nullopt;
        return body;
    }

    int run() {
        bool shutdown = false;
        while (auto body = read_message()) {
            Json m;
            Parser p{*body};
            if (!p.parse(m))
                continue;
            const std::string method = m.at("method").s;
            const Json* id = m.get("id");
            const Json& params = m.at("params");
            if (method == "initialize") {
                reply(*id, R"({"capabilities":{"textDocumentSync":{"openClose":true,"change":1,)"
                           R"("save":{"includeText":false}},"definitionProvider":true},)"
                           R"("serverInfo":{"name":"aura-lsp","version":"0.1"}})");
            } else if (method == "shutdown") {
                shutdown = true;
                if (id)
                    reply(*id, "null");
            } else if (method == "exit") {
                return shutdown ? 0 : 1;
            } else if (method == "textDocument/didOpen") {
                const auto& td = params.at("textDocument");
                docs[td.at("uri").s] = td.at("text").s;
                diagnose(td.at("uri").s);
            } else if (method == "textDocument/didChange") {
                const auto& ch = params.at("contentChanges");
                if (!ch.a.empty())
                    docs[params.at("textDocument").at("uri").s] = ch.a.back().at("text").s;
            } else if (method == "textDocument/didSave") {
                const std::string uri = params.at("textDocument").at("uri").s;
                if (auto* t = params.get("text"))
                    docs[uri] = t->s;
                else if (auto disk = read_file(uri_to_path(uri)))
                    docs[uri] = *disk;
                diagnose(uri);
            } else if (method == "textDocument/didClose") {
                docs.erase(params.at("textDocument").at("uri").s);
            } else if (method == "textDocument/definition") {
                if (id)
                    reply(*id, definition(params));
            } else if (id) {
                send(std::format(
                    R"({{"jsonrpc":"2.0","id":{},"error":{{"code":-32601,"message":{}}}}})",
                    id_text(*id), q("method not found: " + method)));
            }
        }
        return shutdown ? 0 : 1;
    }
};

} // namespace lsp_4911

static int aura_lsp_4911() {
    // Protocol frames go to the original stdout; anything the compiler prints
    // is diverted to stderr so it cannot corrupt the stream.
    lsp_4911::Server s;
    s.out_fd = ::dup(STDOUT_FILENO);
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    std::ios::sync_with_stdio(false);
    return s.run();
}
