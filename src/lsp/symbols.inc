// Reader-based symbol layer (#5259).
//
// The old index was a string search — text.find("(define ") — so a form in a
// comment or in a string literal became a definition, a definition inside a
// function became a top-level one, and define-hygienic-macro / define-type /
// define-struct were not indexed at all. Here the top-level partition comes
// from the shared form splitter (#5239) with byte spans, and each form is read
// into a small s-expression tree. Every capability — goto-definition,
// documentHighlight, documentSymbol, workspace/symbol, foldingRange,
// selectionRange, documentLink — is a query over that one tree. Comments and
// strings are consumed by the reader, so their text can never become a
// definition.
//
// This is a syntax-level view only: no type inference, no def-use graph
// (#5260). Scopes are reader-level — a binding is visible in the body it was
// written in, which is all goto-definition and highlight need.
#ifndef AURA_LSP_SYMBOLS_INC
#define AURA_LSP_SYMBOLS_INC

namespace lsp_4911 {

// ── spans and the read tree ─────────────────────────────────
struct Span {
    std::size_t begin = 0;
    std::size_t end = 0; // one past the last byte
};

enum class SymKind { Atom, Str, List, Prefix };

struct SymNode {
    SymKind kind = SymKind::Atom;
    Span span;
    int parent = -1;
    std::vector<int> kids;
    std::string text; // atom spelling; the decoded value for Str
};

struct SymForm {
    Span span;
    int root = -1;
};

// A name that can be resolved: a define, a let binding, a lambda parameter.
struct SymBinding {
    std::string name;
    Span name_span; // the name token, not the whole form
    bool definition = false;
};

// One indexed definition, hierarchical through children.
struct SymDef {
    std::string name;
    std::string detail;
    Span name_span;
    Span span;
    int kind = 13; // LSP SymbolKind: 13 Variable
    std::vector<int> children;
};

// (require "spec") → the string span and the spec text.
struct SymLink {
    Span span;
    std::string spec;
};

// One atom occurrence and the binding it resolves to (-1 = unresolved).
struct SymOcc {
    int node = -1;
    int binding = -1;
};

// A resolved name: the name token of its binding.
struct SymHit {
    Span span;
    int binding = -1;
};

struct LineIndex {
    std::vector<std::size_t> starts;

    explicit LineIndex(std::string_view t) {
        starts.push_back(0);
        for (std::size_t i = 0; i < t.size(); ++i)
            if (t[i] == '\n')
                starts.push_back(i + 1);
    }

    int line_of(std::size_t off) const {
        const auto it = std::ranges::upper_bound(starts, off);
        return static_cast<int>(std::distance(starts.begin(), it)) - 1;
    }

    std::size_t start_of(int line) const {
        if (line < 0)
            line = 0;
        const auto i = static_cast<std::size_t>(line);
        return i < starts.size() ? starts[i] : starts.back();
    }
};

// The whole syntax-level view of one document's text.
struct DocIndex {
    std::string text;
    std::vector<SymNode> nodes;
    std::vector<SymForm> forms;
    std::vector<SymDef> defs;
    std::vector<int> top; // top-level defs, in source order
    std::vector<SymBinding> bindings;
    std::vector<SymOcc> occs;
    std::vector<Span> comments; // line and block comments
    std::vector<SymLink> links;
    // #5351: completion asked "which node is this binding's name" and "is this
    // a top-level define" by scanning nodes/defs for every binding, which is
    // O(bindings x nodes) on a large file. Both answers are fixed at build.
    std::unordered_map<std::size_t, int> atom_at_begin; // atom span begin -> node
    std::unordered_set<std::size_t> top_name_begin;     // top-level define name spans

    static DocIndex build(std::string_view text);

    void add_comment(Span s) {
        for (auto& c : comments)
            if (c.begin == s.begin) {
                c.end = std::max(c.end, s.end);
                return;
            }
        comments.push_back(s);
    }

    int top_def(std::string_view name) const {
        for (int d : top)
            if (defs[d].name == name)
                return d;
        return -1;
    }

    bool inside_comment(std::size_t off) const {
        for (const auto& c : comments)
            if (off >= c.begin && off < c.end)
                return true;
        return false;
    }

    bool inside_string(std::size_t off) const {
        for (const auto& n : nodes)
            if (n.kind == SymKind::Str && off > n.span.begin && off < n.span.end)
                return true;
        return false;
    }

    int binding_at(std::size_t off) const {
        for (std::size_t i = 0; i < bindings.size(); ++i)
            if (off >= bindings[i].name_span.begin && off <= bindings[i].name_span.end)
                return static_cast<int>(i);
        return -1;
    }

    // The innermost atom at off and the binding it resolves to.
    const SymOcc* occ_at(std::size_t off) const {
        for (const auto& o : occs) {
            const auto& n = nodes[o.node];
            if (off >= n.span.begin && off <= n.span.end)
                return &o;
        }
        return nullptr;
    }

    // goto-definition inside this file: the declaration the name at off
    // resolves to (a local binding, or a top-level define of this file).
    std::optional<SymHit> local_definition(std::size_t off) const {
        if (const int b = binding_at(off); b >= 0)
            return SymHit{bindings[b].name_span, b};
        if (const SymOcc* o = occ_at(off); o && o->binding >= 0)
            return SymHit{bindings[o->binding].name_span, o->binding};
        return std::nullopt;
    }

    // Every occurrence of the binding named at off, declaration included.
    std::vector<std::pair<Span, int>> highlights(std::size_t off) const {
        std::vector<std::pair<Span, int>> out;
        int b = binding_at(off);
        if (b < 0)
            if (const SymOcc* o = occ_at(off); o)
                b = o->binding;
        if (b < 0)
            return out;
        out.push_back({bindings[b].name_span, 3}); // Write
        for (const auto& o : occs)
            if (o.binding == b)
                out.push_back({nodes[o.node].span, 2}); // Read
        return out;
    }

    // Reads of a file-wide name that this document does not declare (#5360).
    // The reader leaves a cross-file reference unresolved (binding -1), and a
    // shadowing let / lambda binding is resolved to its own binding, so it is
    // not one of them.
    std::vector<std::pair<Span, int>> external_highlights(std::string_view name) const {
        std::vector<std::pair<Span, int>> out;
        for (const auto& o : occs) {
            if (o.binding >= 0 || nodes[o.node].text != name)
                continue;
            out.push_back({nodes[o.node].span, 2}); // Read
        }
        return out;
    }

    // The chain of nodes containing off, outermost first.
    std::vector<int> chain_at(std::size_t off) const {
        std::vector<int> chain;
        for (const auto& f : forms) {
            if (f.root < 0 || off < f.span.begin || off > f.span.end)
                continue;
            int n = f.root;
            while (n >= 0) {
                chain.push_back(n);
                int next = -1;
                for (int kid : nodes[n].kids) {
                    const auto& k = nodes[kid];
                    if (off >= k.span.begin && off <= k.span.end) {
                        next = kid;
                        break;
                    }
                }
                n = next;
            }
            break;
        }
        return chain;
    }

    // Folding candidates: every list (top-level forms and the nested ones an
    // editor folds inside them) and every block comment.
    std::vector<std::pair<Span, bool>> folds() const {
        // #5352: the dedup used a linear scan of the output for every node,
        // which is quadratic on a file with N lists. The set makes it one
        // lookup per candidate.
        std::vector<std::pair<Span, bool>> out;
        out.reserve(nodes.size() + forms.size() + comments.size());
        std::unordered_set<std::size_t> seen;
        const auto first_time = [&](std::size_t begin) { return seen.insert(begin).second; };
        for (const auto& n : nodes)
            if (n.kind == SymKind::List && first_time(n.span.begin))
                out.push_back({n.span, false});
        for (const auto& f : forms)
            if (first_time(f.span.begin))
                out.push_back({f.span, false});
        for (const auto& c : comments)
            if (c.end > c.begin + 1 && text.compare(c.begin, 2, "#|") == 0)
                out.push_back({c, true});
        return out;
    }
};

// ── the reader ──────────────────────────────────────────────
// One top-level form's source, read into DocIndex nodes. Spans are absolute
// (base is the form's offset in the document). Comments are recorded, never
// read as data; a malformed tail closes the list at EOF instead of failing,
// so a half-typed buffer still yields an outline.
struct SymReader {
    DocIndex& ix;
    std::string_view s;
    std::size_t base = 0;
    std::size_t i = 0;

    SymReader(DocIndex& index, std::string_view src, std::size_t off)
        : ix(index)
        , s(src)
        , base(off) {}

    int add(SymKind k, std::size_t b, std::size_t e) {
        SymNode n;
        n.kind = k;
        n.span = {base + b, base + e};
        ix.nodes.push_back(std::move(n));
        const int index = static_cast<int>(ix.nodes.size()) - 1;
        if (k == SymKind::Atom)
            ix.atom_at_begin.emplace(base + b, index);
        return index;
    }

    void skip_trivia() {
        for (;;) {
            if (i >= s.size())
                return;
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i;
                continue;
            }
            if (c == ';') {
                const std::size_t b = i;
                while (i < s.size() && s[i] != '\n')
                    ++i;
                ix.add_comment({base + b, base + i});
                continue;
            }
            if (c == '#' && i + 1 < s.size() && s[i + 1] == '|') {
                const std::size_t b = i;
                int nest = 0;
                while (i < s.size()) {
                    if (s[i] == '#' && i + 1 < s.size() && s[i + 1] == '|') {
                        i += 2;
                        ++nest;
                        continue;
                    }
                    if (s[i] == '|' && i + 1 < s.size() && s[i + 1] == '#') {
                        i += 2;
                        if (--nest == 0)
                            break;
                        continue;
                    }
                    ++i;
                }
                ix.add_comment({base + b, base + i});
                continue;
            }
            return;
        }
    }

    static bool atom_delim(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '(' || c == ')' ||
               c == '[' || c == ']' || c == '"' || c == '\'' || c == ';' || c == ',' || c == 96;
    }

    int read_list(std::size_t open_len) {
        const std::size_t b = i;
        i += open_len;
        const int n = add(SymKind::List, b, b);
        for (;;) {
            skip_trivia();
            if (i >= s.size())
                break; // unclosed: close at EOF
            if (s[i] == ')' || s[i] == ']') {
                ++i;
                break;
            }
            const int kid = read_datum();
            if (kid < 0)
                break;
            ix.nodes[kid].parent = n;
            ix.nodes[n].kids.push_back(kid);
        }
        ix.nodes[n].span.end = base + i;
        return n;
    }

    int read_atom(std::size_t b) {
        const int n = add(SymKind::Atom, b, i);
        ix.nodes[n].text = std::string(s.substr(b, i - b));
        return n;
    }

    int read_string(std::size_t b) {
        ++i; // opening quote
        std::string value;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                switch (s[i + 1]) {
                    case 'n':
                        value += '\n';
                        break;
                    case 't':
                        value += '\t';
                        break;
                    case 'r':
                        value += '\r';
                        break;
                    case '0':
                        value += '\0';
                        break;
                    default:
                        value += s[i + 1];
                        break;
                }
                i += 2;
                continue;
            }
            value += s[i];
            ++i;
        }
        if (i < s.size())
            ++i; // closing quote
        const int n = add(SymKind::Str, b, i);
        ix.nodes[n].text = std::move(value);
        return n;
    }

    int read_prefix(std::size_t b) {
        ++i; // the quote / quasiquote / unquote / unquote-splicing char
        if (b < s.size() && s[b] == ',' && i < s.size() && s[i] == '@')
            ++i;
        const int n = add(SymKind::Prefix, b, b);
        const int kid = read_datum();
        if (kid >= 0) {
            ix.nodes[kid].parent = n;
            ix.nodes[n].kids.push_back(kid);
            ix.nodes[n].span.end = ix.nodes[kid].span.end;
        } else {
            ix.nodes[n].span.end = base + i;
        }
        return n;
    }

    int read_datum() {
        skip_trivia();
        if (i >= s.size())
            return -1;
        const std::size_t b = i;
        const char c = s[i];
        if (c == '(' || c == '[')
            return read_list(1);
        if (c == '#' && i + 1 < s.size() && s[i + 1] == '(')
            return read_list(2);
        if (c == ')' || c == ']') {
            ++i; // a stray closer: an empty atom, never a stall
            return read_atom(b);
        }
        if (c == '\'' || c == 96 || c == ',')
            return read_prefix(b);
        if (c == '"')
            return read_string(b);
        if (c == '#' && i + 1 < s.size() && s[i + 1] == '\\') {
            const std::size_t len = aura::compiler::form_split::character_literal_length(s, i);
            if (len > 0) {
                i += len;
                return read_atom(b);
            }
        }
        while (i < s.size() && !atom_delim(s[i])) {
            if (s[i] == '#' && i + 1 < s.size() && s[i + 1] == '|')
                break; // a block comment starts right after the atom
            ++i;
        }
        if (i == b)
            ++i; // never stall
        return read_atom(b);
    }
};

// ── the indexer ─────────────────────────────────────────────
// Walks the read tree with a reader-level environment stack. A define binds in
// the body it was written in (so a nested define is not a top-level symbol),
// let/lambda bind their names in the body only, and quoted data is never code.
class SymIndexer {
public:
    explicit SymIndexer(DocIndex& ix)
        : ix_(ix) {}

    void run() {
        // Top-level names are visible everywhere in the file (forward
        // references are legal at top level), so register them first.
        for (int f = 0; f < static_cast<int>(ix_.forms.size()); ++f) {
            const int root = ix_.forms[f].root;
            if (root < 0)
                continue;
            if (define_target(root) < 0)
                continue;
            const int def = make_def(root);
            def_of_node_[root] = def;
            top_env_.push_back(binding_for(root));
        }
        // #5351/#5352: one environment stack for the whole file. Every scope
        // takes a checkpoint and restores it, instead of copying top_env_ (N
        // ints) per top-level form and per define - that copy was quadratic
        // and dominated documentSymbol/foldingRange at 20k lines.
        std::vector<int> env;
        env.reserve(top_env_.size() + 16);
        for (int b : top_env_)
            env_push(env, b);
        for (int f = 0; f < static_cast<int>(ix_.forms.size()); ++f) {
            const int root = ix_.forms[f].root;
            if (root < 0)
                continue;
            const std::size_t checkpoint = env.size();
            walk(root, env);
            env_restore(env, checkpoint);
        }
    }

private:
    DocIndex& ix_;
    std::unordered_map<int, int> def_of_node_; // list node -> def index
    std::vector<int> top_env_;
    std::vector<int> def_stack_; // open defs, innermost last
    // #5351: name -> live bindings, innermost last. resolve() was a linear
    // scan of the whole environment for every atom (O(N^2) on a file with N
    // top-level defines); the stack mirrors env_push/env_pop exactly.
    std::unordered_map<std::string, std::vector<int>, aura::core::TransparentStringHash,
                       std::equal_to<>>
        by_name_;
    std::unordered_map<std::size_t, int> binding_at_; // name span begin -> binding

    void env_push(std::vector<int>& env, int binding) {
        env.push_back(binding);
        by_name_[ix_.bindings[binding].name].push_back(binding);
    }
    void env_pop(std::vector<int>& env) {
        const int binding = env.back();
        auto it = by_name_.find(ix_.bindings[binding].name);
        if (it != by_name_.end() && !it->second.empty() && it->second.back() == binding)
            it->second.pop_back();
        env.pop_back();
    }
    void env_restore(std::vector<int>& env, std::size_t checkpoint) {
        while (env.size() > checkpoint)
            env_pop(env);
    }

    static bool is_define_head(std::string_view h) {
        return h == "define" || h == "define-hygienic-macro" || h == "define-hygienic-macro*" ||
               h == "defmacro";
    }

    static bool is_type_head(std::string_view h) {
        return h == "define-type" || h == "define-struct";
    }

    // The name node of a define-shaped form, or -1. Both (define x e) and
    // (define (f a) e) count; the name is the token after the keyword.
    int define_target(int node) const {
        const auto& n = ix_.nodes[node];
        if (n.kind != SymKind::List || n.kids.size() < 2)
            return -1;
        const auto& head = ix_.nodes[n.kids[0]];
        if (head.kind != SymKind::Atom || !(is_define_head(head.text) || is_type_head(head.text)))
            return -1;
        const int target = n.kids[1];
        const auto& t = ix_.nodes[target];
        if (t.kind == SymKind::Atom)
            return t.text.empty() ? -1 : target;
        if (t.kind == SymKind::List && !t.kids.empty() &&
            ix_.nodes[t.kids[0]].kind == SymKind::Atom)
            return t.kids[0];
        return -1;
    }

    int make_def(int node) {
        const auto& n = ix_.nodes[node];
        const auto& head = ix_.nodes[n.kids[0]];
        const int target = n.kids[1];
        const auto& t = ix_.nodes[target];
        const int name_node = define_target(node);
        SymDef d;
        d.name = ix_.nodes[name_node].text;
        d.name_span = ix_.nodes[name_node].span;
        d.span = n.span;
        d.detail = d.name;
        if (is_type_head(head.text)) {
            d.kind = 23; // Struct
        } else if (t.kind == SymKind::List) {
            d.kind = 12; // Function
            for (std::size_t k = 1; k < t.kids.size(); ++k) {
                const auto& p = ix_.nodes[t.kids[k]];
                if (p.kind != SymKind::Atom || p.text == ".")
                    continue;
                d.detail += " " + p.text;
            }
        }
        const int index = static_cast<int>(ix_.defs.size());
        ix_.defs.push_back(std::move(d));
        if (def_stack_.empty()) {
            ix_.top.push_back(index);
            ix_.top_name_begin.insert(ix_.defs[index].name_span.begin);
        } else
            ix_.defs[def_stack_.back()].children.push_back(index);
        return index;
    }

    int add_binding(const SymNode& name_node) {
        // #5351: one name node is registered at most once; the span begin is
        // unique, so the linear scan over every binding is not needed.
        const auto found = binding_at_.find(name_node.span.begin);
        if (found != binding_at_.end() &&
            ix_.bindings[found->second].name_span.end == name_node.span.end)
            return found->second;
        SymBinding b;
        b.name = name_node.text;
        b.name_span = name_node.span;
        b.definition = true;
        ix_.bindings.push_back(std::move(b));
        const int index = static_cast<int>(ix_.bindings.size()) - 1;
        binding_at_[name_node.span.begin] = index;
        return index;
    }

    int binding_for(int node) { return add_binding(ix_.nodes[define_target(node)]); }

    int resolve(std::string_view name) const {
        const auto found = by_name_.find(name);
        if (found == by_name_.end() || found->second.empty())
            return -1;
        return found->second.back();
    }

    void walk_seq(const std::vector<int>& kids, std::size_t from, std::vector<int>& env) {
        for (std::size_t k = from; k < kids.size(); ++k)
            walk(kids[k], env);
    }

    void walk(int n, std::vector<int>& env) {
        const auto& node = ix_.nodes[n];
        if (node.kind == SymKind::Atom) {
            ix_.occs.push_back({n, resolve(node.text)});
            return;
        }
        if (node.kind == SymKind::Str)
            return;
        if (node.kind == SymKind::Prefix)
            return; // quoted datum: data, not code
        if (node.kids.empty())
            return;
        const auto& head_node = ix_.nodes[node.kids[0]];
        if (head_node.kind != SymKind::Atom) {
            walk_seq(node.kids, 0, env);
            return;
        }
        const std::string& h = head_node.text;
        if (h == "quote" || h == "quasiquote" || h == "unquote" || h == "unquote-splicing" ||
            h == "syntax")
            return; // quoted data holds no uses
        if (is_define_head(h)) {
            define_like(n, env);
            return;
        }
        if (is_type_head(h)) {
            type_like(n, env);
            return;
        }
        if (h == "lambda") {
            lambda_like(n, env);
            return;
        }
        if (h == "let" || h == "let*" || h == "letrec" || h == "letrec*") {
            let_like(n, env, h);
            return;
        }
        ix_.occs.push_back({node.kids[0], resolve(h)});
        walk_seq(node.kids, 1, env);
    }

    void define_like(int n, std::vector<int>& env) {
        const auto& node = ix_.nodes[n];
        const auto found = def_of_node_.find(n);
        const int def = found != def_of_node_.end() ? found->second : make_def(n);
        def_of_node_[n] = def;
        const std::size_t checkpoint = env.size();
        const int b = binding_for(n);
        env_push(env, b);
        const int target = node.kids[1];
        if (ix_.nodes[target].kind == SymKind::List) {
            // Function sugar: (define (f a b) …) binds the parameters.
            for (std::size_t k = 1; k < ix_.nodes[target].kids.size(); ++k) {
                const int p = ix_.nodes[target].kids[k];
                if (ix_.nodes[p].kind != SymKind::Atom || ix_.nodes[p].text == ".")
                    continue;
                env_push(env, add_binding(ix_.nodes[p]));
            }
        }
        def_stack_.push_back(def);
        walk_seq(node.kids, 2, env);
        def_stack_.pop_back();
        env_restore(env, checkpoint);
        env_push(env, b); // visible to the following siblings
    }

    void type_like(int n, std::vector<int>& env) {
        // (define-type Tree (leaf val) …) / (define-struct point (x y)): the
        // type name is the definition; field and constructor lists are
        // declarative and hold no expressions.
        const auto found = def_of_node_.find(n);
        const int def = found != def_of_node_.end() ? found->second : make_def(n);
        def_of_node_[n] = def;
        env_push(env, binding_for(n));
    }

    void lambda_like(int n, std::vector<int>& env) {
        const auto& node = ix_.nodes[n];
        const std::size_t checkpoint = env.size();
        if (node.kids.size() > 1)
            bind_params(node.kids[1], env);
        walk_seq(node.kids, 2, env);
        env_restore(env, checkpoint);
    }

    void bind_params(int node, std::vector<int>& env) {
        const auto& p = ix_.nodes[node];
        if (p.kind == SymKind::Atom) {
            if (!p.text.empty() && p.text != ".")
                env_push(env, add_binding(p));
            return;
        }
        if (p.kind != SymKind::List)
            return;
        for (int kid : p.kids) {
            const auto& a = ix_.nodes[kid];
            if (a.kind == SymKind::Atom && !a.text.empty() && a.text != ".")
                env_push(env, add_binding(a));
        }
    }

    void let_like(int n, std::vector<int>& env, std::string_view h) {
        const auto& node = ix_.nodes[n];
        if (node.kids.size() < 2)
            return;
        const std::size_t checkpoint = env.size();
        const bool star = (h == "let*" || h == "letrec*");
        const bool rec = (h == "letrec" || h == "letrec*");
        std::size_t cursor = 1;
        // (let loop ((i 0)) …): the loop name is a binding of the body. A
        // plain let evaluates its inits outside that binding, so it is pushed
        // only after them; let*/letrec have the name in scope from the start.
        int loop_binding = -1;
        if (ix_.nodes[node.kids[1]].kind == SymKind::Atom) {
            loop_binding = add_binding(ix_.nodes[node.kids[1]]);
            if (star || rec)
                env_push(env, loop_binding);
            cursor = 2;
        }
        if (cursor >= node.kids.size()) {
            env_restore(env, checkpoint);
            return;
        }
        const int bindings = node.kids[cursor];
        const std::size_t body_from = cursor + 1;
        const auto& bl = ix_.nodes[bindings];
        if (bl.kind == SymKind::List) {
            // let: every init runs in the outer scope and the names bind only
            // the body, so they are pushed after the loop; let*: each init sees
            // the names before it; letrec: all names are in scope in the inits.
            std::vector<int> pending;
            for (int pair : bl.kids) {
                const auto& p = ix_.nodes[pair];
                if (p.kind != SymKind::List || p.kids.empty())
                    continue;
                const auto& nm = ix_.nodes[p.kids[0]];
                if (nm.kind != SymKind::Atom)
                    continue;
                if (rec) {
                    env_push(env, add_binding(nm));
                    if (p.kids.size() > 1)
                        walk(p.kids[1], env);
                } else if (star) {
                    if (p.kids.size() > 1)
                        walk(p.kids[1], env);
                    env_push(env, add_binding(nm));
                } else {
                    if (p.kids.size() > 1)
                        walk(p.kids[1], env);
                    pending.push_back(add_binding(nm));
                }
            }
            for (int b : pending)
                env_push(env, b);
        }
        if (loop_binding >= 0 && !star && !rec)
            env_push(env, loop_binding);
        walk_seq(node.kids, body_from, env);
        env_restore(env, checkpoint);
    }
};

inline DocIndex DocIndex::build(std::string_view text) {
    DocIndex ix;
    ix.text = std::string(text);
    const auto split = aura::compiler::form_split::split_top_level_forms(text);
    for (const auto& c : split.comments)
        ix.add_comment({c.begin, c.end});
    std::vector<Span> form_spans;
    if (split.ok() && split.spans.size() == split.forms.size()) {
        for (const auto& s : split.spans)
            form_spans.push_back({s.begin, s.end});
    } else {
        // A buffer mid-edit (an unclosed form) must keep its outline: read the
        // rest of the file as one tolerant form instead of dropping symbols.
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos)
            form_spans.push_back({first, text.size()});
    }
    for (const auto& span : form_spans) {
        if (span.end <= span.begin)
            continue;
        SymReader rd(ix, text.substr(span.begin, span.end - span.begin), span.begin);
        // A span normally holds exactly one datum. The tolerant fallback above
        // can hold the rest of a half-typed file, so read every datum in it.
        for (;;) {
            const int root = rd.read_datum();
            if (root < 0)
                break;
            ix.forms.push_back({ix.nodes[root].span, root});
            // (require "spec") links live on top-level forms only.
            const auto& n = ix.nodes[root];
            if (n.kind != SymKind::List || n.kids.size() < 2)
                continue;
            const auto& head = ix.nodes[n.kids[0]];
            const auto& spec = ix.nodes[n.kids[1]];
            if (head.kind == SymKind::Atom && head.text == "require" && spec.kind == SymKind::Str &&
                spec.span.end >= spec.span.begin + 2)
                ix.links.push_back({{spec.span.begin + 1, spec.span.end - 1}, spec.text});
        }
    }
    SymIndexer(ix).run();
    return ix;
}

// ── position helpers (one encoding path: A1's PositionCodec) ─
inline std::size_t byte_offset_at(std::string_view text, const LineIndex& li,
                                  const PositionCodec& codec, int line, long long character) {
    const int last = static_cast<int>(li.starts.size()) - 1;
    const auto start = li.start_of(std::clamp(line, 0, last));
    const auto rest = text.substr(std::min(start, text.size()));
    const auto line_text = rest.substr(0, rest.find('\n'));
    return start + codec.to_byte(line_text, character);
}

// #5352: the bytes of one line, located through the index. line_at() walks
// from the top of the file, and range_json runs once per symbol, which made
// documentSymbol/foldingRange quadratic; the line start is already indexed.
inline std::string_view line_view(std::string_view text, const LineIndex& li, int line) {
    const std::size_t begin = li.start_of(line);
    const std::size_t next = li.start_of(line + 1);
    const std::size_t end = next > begin ? next - 1 : text.size();
    return text.substr(begin, std::min(end, text.size()) - begin);
}

inline std::string range_json(std::string_view text, const LineIndex& li,
                              const PositionCodec& codec, Span s) {
    const auto clamp = [&](std::size_t off) { return std::min(off, text.size()); };
    const int l1 = li.line_of(clamp(s.begin));
    const int l2 = li.line_of(clamp(s.end));
    const auto line1 = line_view(text, li, l1);
    const auto line2 = line_view(text, li, l2);
    const int c1 = codec.from_byte(line1, clamp(s.begin) - li.start_of(l1));
    const int c2 = codec.from_byte(line2, clamp(s.end) - li.start_of(l2));
    return std::format(
        R"({{"start":{{"line":{},"character":{}}},"end":{{"line":{},"character":{}}}}})", l1, c1,
        l2, c2);
}

} // namespace lsp_4911

#endif // AURA_LSP_SYMBOLS_INC
