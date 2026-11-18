// Document store, URI normalization and the (require …) graph.
//
// #5258: clients do not agree on URI escaping. eglot/lsp-mode hexify with
// url-hexify-string, which leaves "+", "@" and ":" alone; other clients emit
// upper-case %XX or raw UTF-8. Every spelling of one file must hit the same
// document, so the store's key is the decoded, canonical path — never the
// client's URI text. The client's original URI is kept for replies.
#ifndef AURA_LSP_DOCUMENT_STORE_INC
#define AURA_LSP_DOCUMENT_STORE_INC

namespace lsp_4911 {

inline int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// %41 and %6a decode; a bare "+" stays "+" (a space shortcut is for query
// strings, and a path may legitimately contain '+'). Invalid escapes pass
// through unchanged.
inline std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_value(s[i + 1]);
            const int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

inline std::string percent_encode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[(c >> 4u) & 0xfu];
            out += kHex[c & 0xfu];
        }
    }
    return out;
}

inline bool is_untitled_uri(std::string_view uri) {
    return uri.starts_with("untitled:");
}

// file:///tmp/x, file://localhost/tmp/x, file:/tmp/x → /tmp/x
inline std::string uri_to_path(std::string_view uri) {
    std::string_view rest = uri;
    if (rest.starts_with("file:")) {
        rest.remove_prefix(5);
        if (rest.starts_with("//"))
            rest.remove_prefix(2);
        if (rest.starts_with("localhost/"))
            rest.remove_prefix(9);
    }
    return percent_decode(rest);
}

inline std::string path_to_uri(const std::string& p) {
    if (p.starts_with("untitled:"))
        return p;
    return "file://" + percent_encode(p);
}

inline std::string normalize_path(const std::string& p) {
    if (p.empty())
        return p;
    std::error_code ec;
    const auto canon = std::filesystem::weakly_canonical(std::filesystem::path(p), ec);
    if (!ec)
        return canon.string();
    return std::filesystem::path(p).lexically_normal().string();
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

// The symbol token around byte offset p ("", when there is none).
inline std::string word_at(std::string_view text, std::size_t p) {
    std::size_t b = std::min(p, text.size());
    std::size_t e = b;
    while (b > 0 && sym_char(text[b - 1]))
        --b;
    while (e < text.size() && sym_char(text[e]))
        ++e;
    return std::string(text.substr(b, e - b));
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

inline std::string project_root(std::string dir) {
    for (auto d = std::filesystem::path(dir); !d.empty(); d = d.parent_path()) {
        std::error_code ec;
        if (std::filesystem::exists(d / "aura.toml", ec))
            return d.string();
        if (d == d.parent_path())
            break;
    }
    return dir;
}

// Resolve a require spec for a plain file path: next to the requiring file,
// then the project root (aura.toml), then the runtime rule (CWD, AURA_PATH,
// the binary's lib/, std/). *extra_root* is the editor's workspace root.
// main.cpp's #5250 typecheck path shares this function with the server, so
// "unbound variable" means the same thing in the CLI, the REPL and the LSP.
inline std::optional<std::string> resolve(const std::string& from, const std::string& spec,
                                          std::string_view extra_root = {}) {
    const auto dir = std::filesystem::path(from).parent_path().string();
    const std::string base = dir.empty() ? std::string(".") : dir;
    if (!spec.empty() && spec.front() == '/')
        return aura::module_search::resolve(spec, base);
    std::vector<std::string> roots{base};
    if (!extra_root.empty())
        roots.emplace_back(extra_root);
    roots.push_back(project_root(base));
    for (const std::string& root : roots) {
        if (root.empty())
            continue;
        for (const std::string& cand : {root + "/" + spec, root + "/" + spec + ".aura"}) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(cand, ec))
                return normalize_path(cand);
        }
    }
    return aura::module_search::resolve(spec, base);
}

struct Document {
    std::string text;
    std::string uri; // the client's spelling, echoed back on publish/definition
};

struct DocumentStore {
    std::unordered_map<std::string, Document> docs; // canonical key → document
    std::string root;                               // workspace/project root ("" = unknown)

    static std::string key_of(std::string_view uri) {
        if (is_untitled_uri(uri))
            return percent_decode(uri);
        return normalize_path(uri_to_path(uri));
    }

    const Document* find(const std::string& key) const {
        const auto it = docs.find(key);
        return it == docs.end() ? nullptr : &it->second;
    }

    void open(std::string_view uri, std::string text) {
        Document d{std::move(text), std::string(uri)};
        docs[key_of(uri)] = std::move(d);
    }

    void close(const std::string& key) { docs.erase(key); }

    std::string text_of(const std::string& key) const {
        if (const auto* d = find(key))
            return d->text;
        if (key.starts_with("untitled:"))
            return {};
        return read_file(key).value_or("");
    }

    // The URI a client should see for this key: the original spelling for an
    // open document, a fresh file:// URI otherwise.
    std::string uri_of(const std::string& key) const {
        if (const auto* d = find(key))
            return d->uri;
        return path_to_uri(key);
    }

    std::string base_dir(const std::string& key) const {
        if (key.starts_with("untitled:"))
            return root.empty() ? std::string(".") : root;
        const auto dir = std::filesystem::path(key).parent_path().string();
        return dir.empty() ? std::string(".") : dir;
    }

    // Resolve a require spec: next to the requiring file, then the project
    // root (aura.toml), then the runtime rule. Untitled buffers have no
    // directory of their own, so they search from the workspace root.
    std::optional<std::string> resolve(const std::string& from_key, const std::string& spec) const {
        if (from_key.starts_with("untitled:"))
            return lsp_4911::resolve((root.empty() ? std::string(".") : root) + "/untitled.aura", spec);
        return lsp_4911::resolve(from_key, spec);
    }

    // Collect `key` and its requires, dependencies first (post-order, each
    // file once): a required module's defines must precede the top-level
    // uses in the requiring file, as at runtime.
    void collect(const std::string& key, std::vector<std::string>& order,
                 std::unordered_set<std::string>& seen, int depth = 0) const {
        if (depth > 32 || !seen.insert(key).second)
            return;
        for (auto& r : find_requires(text_of(key)))
            if (auto p = resolve(key, r.spec))
                collect(*p, order, seen, depth + 1);
        order.push_back(key);
    }
};

} // namespace lsp_4911

#endif // AURA_LSP_DOCUMENT_STORE_INC
