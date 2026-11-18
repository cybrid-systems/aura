// references / prepareRename / rename (#5260).
//
// Boundary with #5252: refactor:* / safe-refactor:* / mutate:rename-symbol
// rewrite the *live* workspace through the evaluator and the mutation log.
// Nothing here does that. The handlers only read text and answer with plain
// ranges — a Location list or a WorkspaceEdit (multi-file TextEdit) that the
// client applies. No mutation-log entry, no epoch bump, no file write, and no
// mutate:* primitive: a rename is text, and the editor owns the buffer. Do not
// "helpfully" route these through the workspace; that is the other issue.
//
// The data is #5259's reader index (DocIndex). A name resolves to a binding
// only when the reader's environment says so, so a mention inside a comment or
// a string literal is never a reference, and a let/lambda binding that shadows
// a top-level define splits the two names into two independent binding sets.
//
// Cross-require references use the same two facts the runtime does: a required
// module's top-level defines are visible in the requiring file, and the
// requiring file's own names are not visible to its dependencies. So a use of
// a foreign name is an atom the reader could not resolve locally
// (SymOcc binding == -1) in a file whose require closure contains the
// declaring file. That is exactly "the require graph made this name visible";
// a file that merely spells the same word without requiring the declaration is
// not a reference.
#ifndef AURA_LSP_REFACTOR_INC
#define AURA_LSP_REFACTOR_INC

namespace lsp_4911 {

// One reference site: a byte span in one document.
struct RefSite {
    std::string key;
    Span span;
    bool declaration = false;
};

// What the cursor names. RefTarget::ok means "a binding or define this server
// may rewrite"; RefTarget::external means the atom is unresolved in its own
// file, so the declaring file has to be found through the require closure.
struct RefTarget {
    bool ok = false;
    bool external = false;
    bool top_level = false; // a file-wide define, visible to requiring files
    bool std_file = false;  // declared under std/: references work, rename refuses
    std::string key;        // declaring file
    std::string name;
    Span decl;          // the name token of the declaration
    int binding = -1;   // index into that file's bindings
    std::string reason; // why not, when !ok
};

inline int binding_with_span(const DocIndex& ix, Span s) {
    for (std::size_t i = 0; i < ix.bindings.size(); ++i)
        if (ix.bindings[i].name_span.begin == s.begin && ix.bindings[i].name_span.end == s.end)
            return static_cast<int>(i);
    return -1;
}

// The name under the cursor, in one already-built index.
inline RefTarget target_at(const DocIndex& ix, std::string_view key, std::size_t p) {
    RefTarget t;
    t.key = std::string(key);
    if (ix.inside_comment(p) || ix.inside_string(p)) {
        t.reason = "the position is inside a comment or a string literal, which is not a name";
        return t;
    }
    int b = ix.binding_at(p);
    if (b < 0)
        if (const SymOcc* o = ix.occ_at(p); o && o->binding >= 0)
            b = o->binding;
    if (b >= 0) {
        t.ok = true;
        t.binding = b;
        t.name = ix.bindings[b].name;
        t.decl = ix.bindings[b].name_span;
        t.top_level = is_top_def_name(ix, t.decl);
        return t;
    }
    // No local binding: this is a use of a file-wide name — a top-level define
    // of this file, a required module's define, or a primitive/std name.
    const std::string word = word_at(ix.text, p);
    if (word.empty()) {
        t.reason = "no name at the position";
        return t;
    }
    if (const int d = ix.top_def(word); d >= 0) {
        t.ok = true;
        t.top_level = true;
        t.name = word;
        t.decl = ix.defs[d].name_span;
        t.binding = binding_with_span(ix, t.decl);
        return t;
    }
    t.external = true;
    t.name = word;
    return t;
}

// A file under a std/ directory is the shared standard library: rename refuses
// to edit it (an editor rename must not rewrite a file every project imports).
inline bool is_std_key(const std::string& key) {
    for (const auto& part : std::filesystem::path(key))
        if (part.string() == "std")
            return true;
    return false;
}

// "Is NAME bound by the std prelude?" — the same one-session probe the
// diagnostics engine uses (std/list, std/string, std/hash, std/math), cached
// for the process. A primitive is NOT asked here: prims.find() answers first,
// so the refusal can say which kind of name it is.
inline bool std_prelude_bound(std::string_view name) {
    static std::unordered_map<std::string, bool> cache;
    static std::unique_ptr<aura::compiler::CompilerService> rt;
    if (name.empty())
        return false;
    const std::string key(name);
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    bool ok = false;
    try {
        if (!rt) {
            rt = std::make_unique<aura::compiler::CompilerService>();
            (void)rt->eval("(require \"std/list\" all:)(require \"std/string\" all:)"
                           "(require \"std/hash\" all:)(require \"std/math\" all:)");
        }
        ok = static_cast<bool>(rt->eval(key));
    } catch (const std::exception&) {
        ok = false;
    }
    cache[key] = ok;
    return ok;
}

// A new name must be spellable as one Aura identifier: no whitespace, no
// reader delimiter, no leading digit, no reader prefix characters.
inline bool legal_ident(std::string_view s) {
    if (s.empty() || s.size() > 128)
        return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (i == 0 && std::isdigit(c) != 0)
            return false;
        const bool ok = std::isalnum(c) != 0 || c == '-' || c == '_' || c == '?' || c == '!' ||
                        c == '*' || c == '/' || c == '<' || c == '>' || c == '=' || c == '+' ||
                        c == '%' || c == '&' || c == '^' || c == '~' || c == '.';
        if (!ok)
            return false;
    }
    return true;
}

// The three queries over one workspace snapshot. The candidate file list is
// passed in (open documents, their require closures, the project root's .aura
// files) so this layer never walks the filesystem on its own.
struct RenameService {
    const DocumentStore& store;
    PrimRegistryView& prims;
    std::vector<std::string> workspace;

    std::vector<std::string> closure_of(const std::string& key) const {
        std::vector<std::string> order;
        std::unordered_set<std::string> seen;
        store.collect(key, order, seen);
        return order;
    }

    // Files whose require closure contains def_key: the only files in which an
    // unresolved atom of that name can mean def_key's define.
    std::vector<std::string> dependents(const std::string& def_key) const {
        std::vector<std::string> out;
        for (const auto& f : workspace) {
            if (f == def_key)
                continue;
            std::unordered_set<std::string> seen;
            std::vector<std::string> order;
            store.collect(f, order, seen);
            if (seen.contains(def_key))
                out.push_back(f);
        }
        std::ranges::sort(out);
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    RefTarget resolve(const std::string& key, std::size_t p) const {
        const DocIndex& ix = store.index_of(key);
        RefTarget t = target_at(ix, key, p);
        if (!t.ok && !t.external)
            return t;
        if (t.external) {
            for (const auto& f : closure_of(key)) {
                const DocIndex& dep = store.index_of(f);
                const int d = dep.top_def(t.name);
                if (d < 0)
                    continue;
                t.ok = true;
                t.external = false;
                t.top_level = true;
                t.key = f;
                t.decl = dep.defs[d].name_span;
                t.binding = binding_with_span(dep, t.decl);
                break;
            }
            if (!t.ok) {
                if (prims.find(t.name))
                    t.reason = "'" + t.name +
                               "' is a primitive: its name is a slot in the compiler's registry, "
                               "not text this server may edit";
                else if (std_prelude_bound(t.name))
                    t.reason = "'" + t.name +
                               "' is a std prelude name from the standard library; rename only "
                               "edits the program";
                else
                    t.reason = "'" + t.name +
                               "' has no definition in this file or its requires, so there is "
                               "nothing to rename";
                return t;
            }
        }
        // A std/ declaration is a real binding (references must list it); only
        // rename refuses to edit the shared library file.
        t.std_file = is_std_key(t.key);
        return t;
    }

    std::vector<RefSite> sites(const RefTarget& t) const {
        std::vector<RefSite> out;
        if (!t.ok)
            return out;
        const DocIndex& ix = store.index_of(t.key);
        out.push_back({t.key, t.decl, true});
        for (const auto& o : ix.occs) {
            if (o.binding < 0 || o.binding != t.binding)
                continue;
            const Span s = ix.nodes[o.node].span;
            if (s.begin == t.decl.begin && s.end == t.decl.end)
                continue;
            out.push_back({t.key, s, false});
        }
        if (t.top_level) {
            for (const auto& f : dependents(t.key)) {
                const DocIndex& dep = store.index_of(f);
                for (const auto& o : dep.occs) {
                    // Unresolved at reader level == a name the require graph
                    // provides. A shadowing let/lambda parameter is resolved
                    // (to its own binding) and therefore not collected, and a
                    // mention in a string or comment never becomes an atom.
                    if (o.binding >= 0 || dep.nodes[o.node].text != t.name)
                        continue;
                    out.push_back({f, dep.nodes[o.node].span, false});
                }
            }
        }
        std::ranges::sort(out, [](const RefSite& a, const RefSite& b) {
            return a.key != b.key ? a.key < b.key : a.span.begin < b.span.begin;
        });
        out.erase(std::unique(out.begin(), out.end(),
                              [](const RefSite& a, const RefSite& b) {
                                  return a.key == b.key && a.span.begin == b.span.begin &&
                                         a.span.end == b.span.end;
                              }),
                  out.end());
        return out;
    }

    // Empty when the new name is acceptable; otherwise the reason the request
    // is refused. Conflicts are checked in every file the rename would edit: a
    // binding there with the new name would capture (or be captured by) the
    // rewritten sites. Conservative by design — a rename that might change
    // which binding a name resolves to is refused rather than guessed.
    std::string check_new_name(const RefTarget& t, const std::vector<RefSite>& sites,
                               std::string_view new_name) const {
        if (new_name == t.name)
            return {};
        if (!legal_ident(new_name))
            return "'" + std::string(new_name) +
                   "' is not a legal identifier: use a letter or symbol first, then letters, "
                   "digits, '-', '_', '?', '!', '*', '/' and the comparison characters";
        if (prims.find(new_name))
            return "'" + std::string(new_name) +
                   "' is a primitive name; renaming onto it would shadow the primitive";
        if (std_prelude_bound(new_name))
            return "'" + std::string(new_name) +
                   "' is a std prelude name; pick a name the standard library does not bind";
        std::vector<std::string> files;
        for (const auto& s : sites)
            files.push_back(s.key);
        std::ranges::sort(files);
        files.erase(std::unique(files.begin(), files.end()), files.end());
        for (const auto& f : files) {
            const DocIndex& ix = store.index_of(f);
            for (const auto& b : ix.bindings) {
                if (b.name != new_name)
                    continue;
                if (f == t.key && b.name_span.begin == t.decl.begin &&
                    b.name_span.end == t.decl.end)
                    continue; // the site being renamed is not a conflict with itself
                return "'" + std::string(new_name) + "' is already bound in " + f +
                       "; pick a name that is free in every file this rename edits";
            }
        }
        return {};
    }
};

} // namespace lsp_4911

#endif // AURA_LSP_REFACTOR_INC
