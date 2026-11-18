// Hover, signatureHelp and completion (#5261).
//
// All three are queries over the A2 reader index (DocIndex) and the compiler's
// own primitive registry (lsp/prims.inc), so no capability invents a second
// notion of scope or a second copy of a primitive's text:
//
//   hover           primitive -> signature + the :doc description; user define
//                   -> "(name p1 p2)" + the comment written right above it;
//                   require string -> the path A1 resolved it to.
//   signatureHelp   the innermost call's argument list with the current
//                   argument marked, for primitives and user defines.
//   completion      primitives, top-level defines of the document and its
//                   require closure, bindings visible at the cursor, and
//                   require specs (std/ modules, project root, AURA_PATH).
//
// Types are deliberately absent: they wait for #5250. Positions go through A1's
// PositionCodec -- this file never formats a column itself.
#ifndef AURA_LSP_SEMANTICS_INC
#define AURA_LSP_SEMANTICS_INC

namespace lsp_4911 {

// A markdown code fence, spelled without a literal backtick run in the source.
inline const std::string kFence = "\x60\x60\x60";

// The REPL's Tab-completion word rule (#5040): a candidate replaces the run of
// these characters at the cursor.
inline bool ident_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '-' || c == '?' || c == '!' || c == '*' || c == '/' ||
           c == '>' || c == '<' || c == '=' || c == '_' || c == ':';
}

// Comment text without its delimiters, one line per source line.
inline std::string clean_comment(std::string_view raw) {
    if (raw.starts_with("#|")) {
        auto body = raw.substr(2);
        if (body.ends_with("|#"))
            body.remove_suffix(2);
        std::string trimmed(body);
        const auto first = trimmed.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = trimmed.find_last_not_of(" \t\r\n");
        return trimmed.substr(first, last - first + 1);
    }
    std::string out;
    std::size_t i = 0;
    while (i <= raw.size()) {
        const auto nl = raw.find('\n', i);
        auto line = raw.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl - i);
        std::size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t'))
            ++b;
        if (b < line.size() && line[b] == ';')
            ++b;
        if (b < line.size() && line[b] == ' ')
            ++b;
        if (!out.empty())
            out += '\n';
        out += line.substr(b);
        if (nl == std::string_view::npos)
            break;
        i = nl + 1;
    }
    return out;
}

// The comment immediately above form_begin, if only whitespace separates them.
// A run of adjacent line comments counts as one block.
inline std::optional<std::string> preceding_comment(const DocIndex& ix, std::size_t form_begin) {
    // #5351: comments are recorded in source order, and completion asks for the
    // comment above every define. Two passes over every comment per define made
    // completion O(defs x comments) on a large file; the nearest comment is a
    // binary search and the adjacent run is a walk backwards from it.
    const auto& comments = ix.comments;
    const auto after =
        std::upper_bound(comments.begin(), comments.end(), form_begin,
                         [](std::size_t pos, const Span& c) { return pos < c.begin; });
    if (after == comments.begin())
        return std::nullopt;
    auto cur = after - 1;
    if (cur->end > form_begin)
        return std::nullopt;
    for (std::size_t i = cur->end; i < form_begin; ++i) {
        if (!std::isspace(static_cast<unsigned char>(ix.text[i])))
            return std::nullopt;
    }
    Span block = *cur;
    while (cur != comments.begin()) {
        const Span& prev = *(cur - 1);
        if (prev.end > block.begin)
            break;
        // Adjacent lines: at most one newline between the two comments.
        const auto between = std::string_view(ix.text).substr(prev.end, block.begin - prev.end);
        if (between.find_first_not_of(" \t\n") != std::string_view::npos ||
            std::ranges::count(between, '\n') > 1)
            break;
        block.begin = prev.begin;
        --cur;
    }
    const auto text =
        clean_comment(std::string_view(ix.text).substr(block.begin, block.end - block.begin));
    if (text.empty())
        return std::nullopt;
    return text;
}

// True when span is the name token of a top-level define (a file-wide name,
// not a local binding).
inline bool is_top_def_name(const DocIndex& ix, Span span) {
    // #5351: the build records every top-level define name; the linear scan
    // over ix.top made completion O(bindings x defs).
    return ix.top_name_begin.contains(span.begin);
}

// Bindings whose declaration is visible at p: a top-level define is visible in
// the whole file; a parameter / let name only inside the form that binds it,
// from its declaration onward.
inline std::vector<int> visible_bindings(const DocIndex& ix, std::size_t p) {
    std::vector<int> out;
    for (std::size_t b = 0; b < ix.bindings.size(); ++b) {
        const auto& bind = ix.bindings[b];
        if (p < bind.name_span.begin)
            continue;
        if (is_top_def_name(ix, bind.name_span)) {
            out.push_back(static_cast<int>(b));
            continue;
        }
        // #5351: built once per index instead of scanning every node for
        // every binding (the completion hot path on a large file).
        const auto found = ix.atom_at_begin.find(bind.name_span.begin);
        const int node = found == ix.atom_at_begin.end() ? -1 : found->second;
        int scope = -1;
        for (int up = node >= 0 ? ix.nodes[node].parent : -1; up >= 0; up = ix.nodes[up].parent) {
            const auto& list = ix.nodes[up];
            if (list.kind != SymKind::List || list.kids.empty())
                continue;
            const auto& head = ix.nodes[list.kids[0]];
            if (head.kind != SymKind::Atom)
                continue;
            if (head.text == "define" || head.text == "define-hygienic-macro" ||
                head.text == "define-hygienic-macro*" || head.text == "defmacro" ||
                head.text == "lambda" || head.text == "let" || head.text == "let*" ||
                head.text == "letrec" || head.text == "letrec*") {
                scope = up;
                break;
            }
        }
        if (scope < 0 || (p >= ix.nodes[scope].span.begin && p <= ix.nodes[scope].span.end))
            out.push_back(static_cast<int>(b));
    }
    return out;
}

// Is the cursor inside a string that is the argument of (require ...)? Gives
// the string's content start so the completion prefix can be read from there.
inline bool in_require_string(std::string_view text, std::size_t p, std::size_t& content_start) {
    std::size_t open = std::string_view::npos;
    int quotes = 0;
    for (std::size_t i = 0; i < p && i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < p) {
            ++i;
            continue;
        }
        if (text[i] == '"') {
            ++quotes;
            open = i;
        }
    }
    if (quotes == 0 || quotes % 2 == 0 || open == std::string_view::npos)
        return false;
    std::size_t e = open;
    while (e > 0 && std::isspace(static_cast<unsigned char>(text[e - 1])))
        --e;
    std::size_t b = e;
    while (b > 0 && sym_char(text[b - 1]))
        --b;
    if (std::string_view(text).substr(b, e - b) != "require")
        return false;
    content_start = open + 1;
    return true;
}

struct Semantics {
    const DocumentStore& store;
    const PositionCodec& codec;
    PrimRegistryView& prims;

    // -- small JSON pieces ------------------------------------
    static std::string markup(std::string_view md) {
        return std::format(R"({{"kind":"markdown","value":{}}})", q(md));
    }

    std::string hover_result(std::string md, std::string_view text, const LineIndex& li,
                             Span at) const {
        return std::format(R"({{"contents":{},"range":{}}})", markup(md),
                           range_json(text, li, codec, at));
    }

    static std::string code_block(std::string_view sig) {
        return kFence + "aura\n" + std::string(sig) + "\n" + kFence;
    }

    // -- hover ------------------------------------------------
    std::string define_hover(std::string_view text, const LineIndex& li, const DocIndex& ix,
                             int d) const {
        const auto& def = ix.defs[d];
        std::string sig = def.name;
        if (def.kind == 12)
            sig = "(" + def.detail + ")";
        std::string md = code_block(sig);
        if (const auto comment = preceding_comment(ix, def.span.begin))
            md += "\n\n" + *comment;
        return hover_result(md, text, li, def.name_span);
    }

    std::string prim_hover(const PrimView& prim, std::string_view text, const LineIndex& li,
                           Span at) const {
        std::string md = code_block(prim.signature);
        if (!prim.doc.empty())
            md += "\n\n" + prim.doc;
        return hover_result(md, text, li, at);
    }

    std::string hover(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const int line = static_cast<int>(params.at("position").at("line").n);
        const auto character = static_cast<long long>(params.at("position").at("character").n);
        const std::string text = store.text_of(key);
        if (text.empty())
            return "null";
        const LineIndex li(text);
        const std::size_t p =
            std::min(text.size(), byte_offset_at(text, li, codec, line, character));
        const DocIndex& ix = store.index_of(key);

        // (require "...") : show where A1 resolves the spec.
        for (const auto& link : ix.links) {
            if (p < link.span.begin || p > link.span.end)
                continue;
            const auto target = store.resolve(key, link.spec);
            std::string md = code_block("require \"" + link.spec + "\"");
            md += "\n\n" + (target ? *target : std::string("unresolved (no such module)"));
            return hover_result(md, text, li, link.span);
        }
        // A non-require string is data, not a name to describe.
        if (ix.inside_comment(p) || ix.inside_string(p))
            return "null";

        // A parameter / let name in scope wins over a same-named primitive.
        if (const auto hit = ix.local_definition(p)) {
            const auto& binding = ix.bindings[hit->binding];
            if (!is_top_def_name(ix, binding.name_span)) {
                std::string md = code_block(binding.name) + "\n\nLocal binding.";
                return hover_result(md, text, li, binding.name_span);
            }
        }

        const std::string word = word_at(text, p);
        if (word.empty())
            return "null";
        if (const int d = ix.top_def(word); d >= 0)
            return define_hover(text, li, ix, d);

        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        store.collect(key, files, seen);
        for (const auto& f : files) {
            if (f == key)
                continue;
            const std::string other_text = store.text_of(f);
            const DocIndex other = DocIndex::build(other_text);
            const int d = other.top_def(word);
            if (d < 0)
                continue;
            const LineIndex other_li(other_text);
            return define_hover(other_text, other_li, other, d);
        }

        if (const PrimView* prim = prims.find(word)) {
            // The hover range is the whole token, not just the part after the
            // cursor.
            std::size_t b = std::min(p, text.size());
            while (b > 0 && sym_char(text[b - 1]))
                --b;
            return prim_hover(*prim, text, li, Span{b, b + word.size()});
        }
        return "null";
    }

    // -- signatureHelp ----------------------------------------
    static std::vector<std::string> split_words(std::string_view s) {
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            const std::size_t b = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            if (i > b)
                out.emplace_back(s.substr(b, i - b));
        }
        return out;
    }

    static std::vector<std::string> define_params(const SymDef& def) {
        std::vector<std::string> out;
        for (const auto& w : split_words(def.detail))
            if (w != def.name)
                out.push_back(w);
        return out;
    }

    std::string signature_help(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const int line = static_cast<int>(params.at("position").at("line").n);
        const auto character = static_cast<long long>(params.at("position").at("character").n);
        const std::string text = store.text_of(key);
        if (text.empty())
            return "null";
        const LineIndex li(text);
        const std::size_t p =
            std::min(text.size(), byte_offset_at(text, li, codec, line, character));
        const DocIndex& ix = store.index_of(key);

        // The innermost list with an atom head is the call being typed.
        int call = -1;
        for (const int n : ix.chain_at(p)) {
            const auto& node = ix.nodes[n];
            if (node.kind != SymKind::List || node.kids.empty())
                continue;
            if (ix.nodes[node.kids[0]].kind == SymKind::Atom)
                call = n;
        }
        if (call < 0)
            return "null";
        const auto& node = ix.nodes[call];
        const std::string callee = ix.nodes[node.kids[0]].text;

        int active = 0;
        for (std::size_t k = 1; k < node.kids.size(); ++k) {
            const auto& kid = ix.nodes[node.kids[k]];
            if (p <= kid.span.end) {
                active = static_cast<int>(k) - 1;
                break;
            }
            active = static_cast<int>(k);
        }

        std::string label;
        std::vector<std::string> arg_labels;
        if (const PrimView* prim = prims.find(callee)) {
            label = prim->call_label();
            arg_labels = prim_param_labels(*prim);
        } else {
            const int d = ix.top_def(callee);
            if (d < 0)
                return "null";
            label = "(" + ix.defs[d].detail + ")";
            arg_labels = define_params(ix.defs[d]);
        }

        std::string plist;
        for (const auto& a : arg_labels) {
            if (!plist.empty())
                plist += ',';
            plist += std::format(R"({{"label":{}}})", q(a));
        }
        std::string out =
            std::format(R"({{"signatures":[{{"label":{},"parameters":[{}])", q(label), plist);
        int marked = -1;
        if (!arg_labels.empty()) {
            const int last = static_cast<int>(arg_labels.size()) - 1;
            marked = std::clamp(active, 0, last);
            out += std::format(R"(,"activeParameter":{})", marked);
        }
        out += "}],\"activeSignature\":0";
        // The spec allows either level; emitting both is what eglot and
        // lsp-mode read without a fallback.
        if (marked >= 0)
            out += std::format(R"(,"activeParameter":{})", marked);
        out += "}";
        return out;
    }

    // -- completion -------------------------------------------
    // One require spec per resolvable module. std/ modules come from the same
    // lib/std directories the REPL's Tab completion scans; the project root and
    // every AURA_PATH entry are listed as the runtime searches them.
    std::vector<std::pair<std::string, std::string>>
    require_candidates(const std::string& key) const {
        std::vector<std::pair<std::string, std::string>> out;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& spec, const std::string& path) {
            if (spec.empty() || !seen.insert(spec).second)
                return;
            out.emplace_back(spec, path);
        };
        auto list_dir = [&](const std::filesystem::path& dir, const std::string& prefix) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec))
                return;
            std::filesystem::recursive_directory_iterator it(
                dir, std::filesystem::directory_options::skip_permission_denied, ec);
            const std::filesystem::recursive_directory_iterator end;
            for (; !ec && it != end && out.size() < 2000; it.increment(ec)) {
                const auto& path = it->path();
                std::error_code dir_ec;
                if (it->is_directory(dir_ec)) {
                    const std::string name = path.filename().string();
                    if (name == ".git" || name == "build" || name == "node_modules" ||
                        name == ".cache")
                        it.disable_recursion_pending();
                    continue;
                }
                if (path.extension() != ".aura")
                    continue;
                std::string spec = prefix + path.filename().string();
                spec.resize(spec.size() - 5); // drop ".aura"
                add(spec, path.string());
            }
        };
        // std/ modules: one scan shared with the REPL's Tab completion, so the
        // editor and the REPL cannot offer different module names.
        for (const auto& [spec, path] : aura::module_search::std_module_files())
            add(spec, path);

        // The document's own directory, as "./name".
        if (!key.starts_with("untitled:"))
            list_dir(std::filesystem::path(key).parent_path(), "./");
        // The project root (aura.toml), as "dir/name".
        if (!store.root.empty())
            list_dir(store.root, "");
        // Every AURA_PATH entry, as "name".
        for (const auto& dir :
             aura::module_search::split_path_list(aura::config::raw(aura::config::Key::AURA_PATH)))
            list_dir(dir, "");
        return out;
    }

    std::string completion(const Json& params) {
        const std::string uri = params.at("textDocument").at("uri").s;
        const std::string key = DocumentStore::key_of(uri);
        const int line = static_cast<int>(params.at("position").at("line").n);
        const auto character = static_cast<long long>(params.at("position").at("character").n);
        const std::string text = store.text_of(key);
        if (text.empty())
            return "[]";
        const LineIndex li(text);
        const std::size_t p =
            std::min(text.size(), byte_offset_at(text, li, codec, line, character));
        std::size_t start = p;
        while (start > 0 && ident_char(text[start - 1]))
            --start;
        const std::string prefix = text.substr(start, p - start);

        std::string items;
        std::unordered_set<std::string> emitted;
        auto emit = [&](const std::string& label, int kind, std::string_view detail,
                        std::string_view doc) {
            if (label.empty() || !label.starts_with(prefix))
                return;
            if (!emitted.insert(label).second)
                return;
            if (!items.empty())
                items += ',';
            items +=
                std::format(R"({{"label":{},"kind":{},"detail":{})", q(label), kind, q(detail));
            if (!doc.empty())
                items += std::format(R"(,"documentation":{})", markup(doc));
            items += '}';
        };

        const DocIndex& ix = store.index_of(key);
        std::size_t content_start = 0;
        if (in_require_string(text, p, content_start)) {
            for (const auto& [spec, path] : require_candidates(key))
                emit(spec, 17, path, kFence + "text\n" + path + "\n" + kFence);
            return "[" + items + "]";
        }
        // Other strings hold data, not names.
        if (ix.inside_string(p))
            return "[]";
        // Local bindings first: they shadow primitives and outer defines.
        for (const int b : visible_bindings(ix, p)) {
            const auto& bind = ix.bindings[b];
            // Top-level defines are emitted below with their real kind; their
            // file-wide binding must not shadow that entry as "local".
            if (is_top_def_name(ix, bind.name_span))
                continue;
            emit(bind.name, 6, "local binding", {});
        }
        for (const int d : ix.top) {
            const auto& def = ix.defs[d];
            const int kind = def.kind == 12 ? 3 : def.kind == 23 ? 22 : 6;
            const std::string detail = def.kind == 12 ? "(" + def.detail + ")" : def.name;
            emit(def.name, kind, detail, preceding_comment(ix, def.span.begin).value_or(""));
        }
        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        store.collect(key, files, seen);
        for (const auto& f : files) {
            if (f == key)
                continue;
            const DocIndex& other = store.index_of(f);
            for (const int d : other.top) {
                const auto& def = other.defs[d];
                const int kind = def.kind == 12 ? 3 : def.kind == 23 ? 22 : 6;
                const std::string detail = def.kind == 12 ? "(" + def.detail + ")" : def.name;
                emit(def.name, kind, detail, preceding_comment(other, def.span.begin).value_or(""));
            }
        }
        prims.ensure();
        for (const auto& prim : prims.items)
            emit(prim.name, 3, prim.signature, prim.doc);
        return "[" + items + "]";
    }
};

} // namespace lsp_4911

#endif // AURA_LSP_SEMANTICS_INC
