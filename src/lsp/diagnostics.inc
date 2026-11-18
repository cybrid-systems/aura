// Diagnostics: typecheck the document plus its (require …) closure.
//
// #5258 moved this out of the method handlers so the debounce, the pull
// request and the push both consume one result. Columns stay in the engine's
// own unit (0-based bytes, #5235/#5240); the server converts them to the
// negotiated position encoding exactly once.
//
// #5262: this is the structured consumer. 'code' is the stable kind ident the
// CLI/serve JSON faces already print, 'message' is the producer's text with
// no kind prefix and no folded-in hint, the hint stays separate (it is what a
// quickfix replaces with), and the notes plus the require chain become LSP
// relatedInformation.
#ifndef AURA_LSP_DIAGNOSTICS_INC
#define AURA_LSP_DIAGNOSTICS_INC

namespace lsp_4911 {

// A location that explains a diagnostic: another document, or another line of
// this one. LSP relatedInformation wants a Location plus a message.
struct RelatedInfo {
    std::string key;
    int line = 0; // 0-based line in that file
    int col = 0;  // 0-based byte column
    int end = 0;  // 0-based byte column, exclusive
    std::string message;
};

struct Diag {
    int line = 0; // 0-based line in the file
    int col = 0;  // 0-based byte column
    int end = 1;  // 0-based byte column, exclusive
    aura::diag::ErrorKind kind = aura::diag::ErrorKind::InternalError;
    std::string code;     // aura::diag::kind_ident(kind)
    int severity = 1;     // LSP DiagnosticSeverity: 1 Error, 2 Warning, 3 Info
    std::string message;  // d.message, byte for byte (shared with the JSON faces)
    std::string hint;     // d.suggestion; never folded into the message
    std::string fix_name; // the name a quickfix would substitute ("" = none)
    std::vector<RelatedInfo> related;
};

struct FileDiagnostics {
    std::string key;
    std::vector<Diag> items;
};

// The type checker's own errors. #5250 made the static face, the runtime and
// --typecheck agree, so they are published as warnings by default and the
// client may turn them off or promote them to errors.
enum class TypeMode { Off, Warning, Error };

// LSP severity for a kind: a Note is information, a Warning is a warning,
// every error kind is an error. TypeError is decided in analyze() because its
// severity follows TypeMode.
inline int severity_of(aura::diag::ErrorKind kind) {
    using K = aura::diag::ErrorKind;
    switch (kind) {
        case K::Note:
            return 3; // Information
        case K::Warning:
            return 2; // Warning
        default:
            return 1; // Error
    }
}

// "did you mean 'member'? (…)" -> "member". #5235 owns the wording; a quickfix
// replaces the underlined name with exactly this token, so a suggestion that
// is not a plain symbol (a module path, say) yields no fix.
inline std::string suggested_name(std::string_view hint) {
    constexpr std::string_view kPrefix = "did you mean '";
    const auto at = hint.find(kPrefix);
    if (at == std::string_view::npos)
        return {};
    const auto rest = hint.substr(at + kPrefix.size());
    const auto close = rest.find('\'');
    if (close == std::string_view::npos || close == 0)
        return {};
    std::string name(rest.substr(0, close));
    if (name.find_first_of(":/") != std::string::npos)
        return {};
    return std::ranges::all_of(name, sym_char) ? name : std::string{};
}

// End of the symbol starting at 'col', in bytes; never shorter than one
// character.
inline int underline_end(std::string_view text, int line, int col) {
    std::size_t off = 0;
    for (int k = 0; k < line; ++k) {
        const auto nl = text.find('\n', off);
        if (nl == std::string_view::npos)
            return col + 1;
        off = nl + 1;
    }
    if (off + static_cast<std::size_t>(col) >= text.size())
        return col + 1;
    std::size_t p = off + static_cast<std::size_t>(col);
    if (text[p] == '(')
        ++p;
    std::size_t e = p;
    while (e < text.size() && sym_char(text[e]))
        ++e;
    const int end = static_cast<int>(e - off);
    return end <= col ? col + 1 : end;
}

struct DiagnosticsEngine {
    const DocumentStore& store;
    // #5262: type diagnostics are warnings unless the client says otherwise.
    TypeMode type_mode = TypeMode::Warning;
    // One runtime session (std prelude loaded) answers "is NAME bound?".
    std::unique_ptr<aura::compiler::CompilerService> rt;
    std::unordered_map<std::string, bool> bound_cache;

    // Accepts "warning"/"warn"/"true"/"on", "error", "off"/"none"/"false".
    static bool parse_type_mode(std::string_view raw, TypeMode& out) {
        if (raw == "off" || raw == "none" || raw == "false" || raw == "disabled") {
            out = TypeMode::Off;
            return true;
        }
        if (raw == "warning" || raw == "warn" || raw == "true" || raw == "on") {
            out = TypeMode::Warning;
            return true;
        }
        if (raw == "error") {
            out = TypeMode::Error;
            return true;
        }
        return false;
    }

    bool set_type_mode(std::string_view raw) {
        TypeMode mode;
        if (!parse_type_mode(raw, mode))
            return false;
        type_mode = mode;
        return true;
    }

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

    // #5240: classify on the structured kind, not on the formatted prefix.
    bool keep_diag(aura::diag::ErrorKind kind, const std::string& message,
                   const std::unordered_set<std::string>& defined) {
        using K = aura::diag::ErrorKind;
        if (kind == K::TypeError)
            return type_mode != TypeMode::Off;
        // The static signature table lags runtime primitives with optional
        // arguments (mutate :rebind name src [reason]); trust the runtime for
        // callees the project does not define itself.
        if (kind == K::ArityMismatch) {
            static constexpr std::string_view kCall = "call '";
            const auto at = std::string_view(message).find(kCall);
            if (at == std::string_view::npos)
                return true;
            const auto rest = std::string_view(message).substr(at + kCall.size());
            const auto callee = std::string(rest.substr(0, rest.find('\'')));
            return defined.contains(callee) || !runtime_bound(callee);
        }
        if (kind != K::UnboundVariable)
            return true;
        auto name = message.substr(0, message.find('\n'));
        return !defined.contains(name) && !runtime_bound(name);
    }

    // One analysis of 'main_key' and its closure; the caller publishes it,
    // serves it from textDocument/diagnostic, or both.
    std::vector<FileDiagnostics> analyze(const std::string& main_key) {
        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        store.collect(main_key, files, seen);
        // Masked texts (requires -> spaces, newlines kept) concatenated; each
        // segment remembers its first line so diagnostics map back.
        std::string combined;
        struct Segment {
            int first_line = 1;
            std::string key;
        };
        struct RequireSite {
            std::string key; // the requiring file
            Span span;       // the (require …) form in that file
            std::string spec;
        };
        std::vector<Segment> segs;
        std::vector<std::string> parts;
        // #5347: each segment's byte offset is a line baseline for the parser.
        // Token locations stay buffer lines (the segment map below turns those
        // into ranges); messages name the file's own line.
        std::vector<aura::parser::SourceLineOrigin> origins;
        // #5262: a diagnostic inside a required module is explained by the
        // (require …) that pulled it in, so remember each edge.
        std::unordered_map<std::string, RequireSite> require_site;
        int line = 1;
        std::size_t offset = 0;
        for (auto& f : files) {
            std::string t = store.text_of(f);
            for (auto& r : find_requires(t)) {
                if (auto p = store.resolve(f, r.spec); p && !require_site.contains(*p))
                    require_site.emplace(*p, RequireSite{f, Span{r.begin, r.end}, r.spec});
                for (std::size_t i = r.begin; i < r.end; ++i)
                    if (t[i] != '\n')
                        t[i] = ' ';
            }
            if (!t.empty() && t.back() != '\n')
                t += '\n';
            segs.push_back({line, f});
            origins.push_back({offset, 1});
            line += static_cast<int>(std::ranges::count(t, '\n'));
            offset += t.size();
            combined += t;
            parts.push_back(std::move(t));
        }
        std::unordered_map<std::string, std::vector<Diag>> by_file;
        for (auto& f : files)
            by_file[f];
        // #5347: an unclosed list in an earlier segment would otherwise
        // swallow the files spliced after it, and the diagnostic would land
        // in the wrong buffer. Parse each file on its own text first (its
        // line baseline is 1). A blank segment is a (require …) that was
        // masked out — nothing of that file remains to diagnose.
        auto blank = [](std::string_view s) {
            return std::ranges::all_of(
                s, [](char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; });
        };
        bool parsed_apart = false;
        if (parts.size() > 1) {
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (blank(parts[i]))
                    continue;
                aura::ast::ASTArena arena;
                auto alloc = arena.allocator();
                aura::ast::StringPool pool(alloc);
                aura::ast::FlatAST flat(alloc);
                auto pr = aura::parser::parse_to_flat(parts[i], flat, pool);
                if (pr.success && pr.root != aura::ast::NULL_NODE)
                    continue;
                parsed_apart = true;
                aura::diag::Diagnostic d =
                    pr.errors.empty()
                        ? aura::diag::Diagnostic{aura::diag::ErrorKind::ParseError,
                                                 pr.error.empty() ? "parse error" : pr.error}
                        : aura::diag::Diagnostic{aura::diag::ErrorKind::ParseError,
                                                 pr.errors[0].message, pr.errors[0].location};
                if (d.location.line == 0)
                    continue;
                Diag out;
                out.line = static_cast<int>(d.location.line) - 1;
                out.col = std::max(0, static_cast<int>(d.location.column) - 1);
                out.kind = d.kind;
                out.code = std::string(aura::diag::kind_ident(d.kind));
                out.severity = severity_of(d.kind);
                out.message = std::move(d.message);
                by_file[files[i]].push_back(std::move(out));
            }
        }
        std::vector<aura::diag::Diagnostic> structured;
        if (!parsed_apart) {
            try {
                aura::compiler::CompilerService cs;
                auto tc = cs.typecheck_full(combined, origins);
                structured = std::move(tc.diagnostics);
            } catch (const std::exception& e) {
                aura::diag::Diagnostic d{aura::diag::ErrorKind::InternalError,
                                         std::string("typecheck failed: ") + e.what()};
                structured.push_back(std::move(d));
            }
        }
        for (const auto& d : structured) {
            if (d.location.line == 0)
                continue;
            const int gl = static_cast<int>(d.location.line);
            auto seg = std::ranges::upper_bound(segs, gl, {}, [](auto& p) { return p.first_line; });
            if (seg == segs.begin())
                continue;
            --seg;
            auto& v = by_file[seg->key];
            // Keep the first diagnostic per file (CLI default: first error stops).
            if (!v.empty())
                continue;
            if (d.kind == aura::diag::ErrorKind::TypeError && type_mode == TypeMode::Off)
                continue;
            Diag out;
            out.line = gl - seg->first_line;
            out.col = std::max(0, static_cast<int>(d.location.column) - 1);
            out.kind = d.kind;
            out.code = std::string(aura::diag::kind_ident(d.kind));
            out.severity = d.kind == aura::diag::ErrorKind::TypeError
                               ? (type_mode == TypeMode::Error ? 1 : 2)
                               : severity_of(d.kind);
            out.message = d.message;
            out.hint = d.suggestion;
            if (d.kind == aura::diag::ErrorKind::UnboundVariable)
                out.fix_name = suggested_name(d.suggestion);
            // #5262: the producer's notes become relatedInformation. They
            // carry no range of their own, so they point at the diagnostic.
            for (const auto& note : d.context_stack)
                out.related.push_back({seg->key, out.line, out.col, 0, note});
            v.push_back(std::move(out));
        }
        // The static checker is stricter than the runtime: type errors on
        // dynamically typed code and "unbound" for runtime primitives / std
        // prelude names / forward references are noise in an editor. Keep
        // parse errors and unbound names that really are unbound at runtime.
        // #5259: the unbound filter consumes the same reader-based index as
        // goto-definition. A (define ghost …) in a comment or a string no
        // longer hides a real unbound reference to ghost.
        std::unordered_set<std::string> defined;
        for (auto& f : files) {
            const DocIndex ix = DocIndex::build(store.text_of(f));
            for (const int d : ix.top)
                defined.insert(ix.defs[d].name);
        }
        for (auto& [path, diags] : by_file)
            std::erase_if(diags,
                          [&](const Diag& d) { return !keep_diag(d.kind, d.message, defined); });
        // #5262: follow the require chain upwards so the editor can show
        // where a required module (or a module it requires) came from.
        for (auto& [path, diags] : by_file) {
            if (path == main_key)
                continue;
            for (auto& d : diags) {
                std::string cur = path;
                for (int depth = 0; depth < 8; ++depth) {
                    const auto it = require_site.find(cur);
                    if (it == require_site.end())
                        break;
                    const std::string& parent = it->second.key;
                    const std::string ptext = store.text_of(parent);
                    const LineIndex li(ptext);
                    const std::size_t b = std::min(it->second.span.begin, ptext.size());
                    const std::size_t e = std::min(it->second.span.end, ptext.size());
                    const int rline = li.line_of(b);
                    const auto ltext = line_at(ptext, static_cast<std::size_t>(rline));
                    const int rcol = static_cast<int>(b - li.start_of(rline));
                    const std::size_t lb = li.start_of(rline);
                    const int rend =
                        static_cast<int>(std::min<std::size_t>(ltext.size(), e > lb ? e - lb : 0));
                    d.related.push_back(
                        {parent, rline, rcol, rend, "required here: \"" + it->second.spec + "\""});
                    if (parent == main_key)
                        break;
                    cur = parent;
                }
            }
        }
        std::vector<FileDiagnostics> runs;
        for (auto& [path, diags] : by_file) {
            const std::string text = store.text_of(path);
            for (auto& d : diags) {
                d.end = underline_end(text, d.line, d.col);
                // A note points at the same token as its diagnostic.
                for (auto& r : d.related)
                    if (r.key == path && r.line == d.line && r.col == d.col && r.end <= r.col)
                        r.end = d.end;
            }
            runs.push_back({path, std::move(diags)});
        }
        return runs;
    }
};

} // namespace lsp_4911

#endif // AURA_LSP_DIAGNOSTICS_INC
