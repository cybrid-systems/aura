// Diagnostics: typecheck the document plus its (require …) closure.
//
// #5258 moved this out of the method handlers so the debounce, the pull
// request and the push both consume one result. Columns stay in the engine's
// own unit (0-based bytes, #5235/#5240); the server converts them to the
// negotiated position encoding exactly once.
#ifndef AURA_LSP_DIAGNOSTICS_INC
#define AURA_LSP_DIAGNOSTICS_INC

namespace lsp_4911 {

struct Diag {
    int line = 0;  // 0-based line in the file
    int col = 0;   // 0-based byte column
    int end = 1;   // 0-based byte column, exclusive
    aura::diag::ErrorKind kind = aura::diag::ErrorKind::InternalError;
    std::string raw;
    std::string msg;
};

struct FileDiagnostics {
    std::string key;
    std::vector<Diag> items;
};

// End of the symbol starting at `col`, in bytes; never shorter than one
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

    // #5240: classify on the structured kind, not on the formatted prefix.
    bool keep_diag(aura::diag::ErrorKind kind, const std::string& raw,
                   const std::unordered_set<std::string>& defined) {
        using K = aura::diag::ErrorKind;
        if (kind == K::TypeError)
            return false;
        // The static signature table lags runtime primitives with optional
        // arguments (mutate :rebind name src [reason]); trust the runtime for
        // callees the project does not define itself.
        if (kind == K::ArityMismatch) {
            static constexpr std::string_view kCall = "call '";
            const auto at = std::string_view(raw).find(kCall);
            if (at == std::string_view::npos)
                return true;
            const auto rest = std::string_view(raw).substr(at + kCall.size());
            const auto callee = std::string(rest.substr(0, rest.find('\'')));
            return defined.contains(callee) || !runtime_bound(callee);
        }
        if (kind != K::UnboundVariable)
            return true;
        auto name = raw.substr(0, raw.find('\n'));
        return !defined.contains(name) && !runtime_bound(name);
    }

    // One analysis of `main_key` and its closure; the caller publishes it,
    // serves it from textDocument/diagnostic, or both.
    std::vector<FileDiagnostics> analyze(const std::string& main_key) {
        std::vector<std::string> files;
        std::unordered_set<std::string> seen;
        store.collect(main_key, files, seen);
        // Masked texts (requires → spaces, newlines kept) concatenated; each
        // segment remembers its first line so diagnostics map back.
        std::string combined;
        struct Segment {
            int first_line = 1;
            std::string key;
        };
        std::vector<Segment> segs;
        int line = 1;
        for (auto& f : files) {
            std::string t = store.text_of(f);
            for (auto& r : find_requires(t))
                for (std::size_t i = r.begin; i < r.end; ++i)
                    if (t[i] != '\n')
                        t[i] = ' ';
            if (!t.empty() && t.back() != '\n')
                t += '\n';
            segs.push_back({line, f});
            line += static_cast<int>(std::ranges::count(t, '\n'));
            combined += t;
        }
        std::unordered_map<std::string, std::vector<Diag>> by_file;
        for (auto& f : files)
            by_file[f];
        // #5240: consume the structured diagnostics directly. The old path
        // formatted them into text and recovered line/column with a regex.
        std::vector<aura::diag::Diagnostic> structured;
        try {
            aura::compiler::CompilerService cs;
            auto tc = cs.typecheck_full(combined);
            structured = std::move(tc.diagnostics);
        } catch (const std::exception& e) {
            aura::diag::Diagnostic d{aura::diag::ErrorKind::InternalError,
                                     std::string("typecheck failed: ") + e.what()};
            structured.push_back(std::move(d));
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
            // #5240: keep_diag() now keys on the structured kind and the
            // raw message; the composed text is wire-only.
            std::string msg = std::string(aura::diag::kind_name(d.kind)) + ": " + d.message;
            if (!d.suggestion.empty())
                msg += "\n" + d.suggestion;
            v.push_back({gl - seg->first_line, std::max(0, static_cast<int>(d.location.column) - 1), 0,
                         d.kind, d.message, std::move(msg)});
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
            std::erase_if(diags, [&](const Diag& d) { return !keep_diag(d.kind, d.raw, defined); });
        std::vector<FileDiagnostics> runs;
        for (auto& [path, diags] : by_file) {
            const std::string text = store.text_of(path);
            for (auto& d : diags)
                d.end = underline_end(text, d.line, d.col);
            runs.push_back({path, std::move(diags)});
        }
        return runs;
    }
};

} // namespace lsp_4911

#endif // AURA_LSP_DIAGNOSTICS_INC
