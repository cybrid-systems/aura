module aura.compiler.query;
import std;

namespace aura::compiler {

using namespace aura::ast;

// ── resolve_type_id — map type name to hardcoded TypeId values ──
// Matches TypeRegistry initialization order (type_impl.cpp):
//   DYNAMIC=0, INT=1, BOOL=2, STRING=3, VOID=4, TYPE=5,
//   VECTOR=6, FLOAT=7, PAIR=8, HASH=9
static std::uint32_t resolve_type_id(const std::string& name) {
    if (name == "Any" || name == "Dyn" || name == "Dynamic")
        return 0;
    if (name == "Int")
        return 1;
    if (name == "Bool")
        return 2;
    if (name == "String")
        return 3;
    if (name == "Void")
        return 4;
    if (name == "Type")
        return 5;
    if (name == "Vector")
        return 6;
    if (name == "Float")
        return 7;
    if (name == "Pair")
        return 8;
    if (name == "Hash")
        return 9;
    return 0; // DYNAMIC / unknown
}

static std::vector<std::string> tokenize(std::string_view s) {
    std::vector<std::string> tokens;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = s[i];
        if (c == '(' || c == ')') {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
            }
            tokens.emplace_back(1, c);
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
            }
        } else if (c == '"') {
            // Capture string content between quotes, strip the quotes
            i++;
            cur.clear();
            while (i < s.size() && s[i] != '"') {
                cur += s[i];
                i++;
            }
            if (i < s.size())
                i++; // skip closing quote
            tokens.push_back(std::move(cur));
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        tokens.push_back(std::move(cur));
    return tokens;
}

struct PS {
    std::vector<std::string> t;
    std::size_t p = 0;
    bool open() const { return p < t.size() && t[p] == "("; }
    bool close() const { return p < t.size() && t[p] == ")"; }
    bool end() const { return p >= t.size(); }
    std::string next() { return p < t.size() ? t[p++] : ""; }
};

static QueryExpr parse_expr(PS& ps);

static QueryExpr parse_list(PS& ps) {
    ps.next(); // '('
    auto op = ps.next();

    QueryExpr q;
    if (op == "node-type") {
        q.kind = QueryExpr::Kind::NodeType;
        auto s = ps.next();
        if (s == "LiteralInt")
            q.node_tag = NodeTag::LiteralInt;
        else if (s == "Variable")
            q.node_tag = NodeTag::Variable;
        else if (s == "Call")
            q.node_tag = NodeTag::Call;
        else if (s == "IfExpr" || s == "If")
            q.node_tag = NodeTag::IfExpr;
        else if (s == "Lambda")
            q.node_tag = NodeTag::Lambda;
        else if (s == "Let")
            q.node_tag = NodeTag::Let;
        else if (s == "LetRec")
            q.node_tag = NodeTag::LetRec;
        else if (s == "Define")
            q.node_tag = NodeTag::Define;
        else if (s == "Begin")
            q.node_tag = NodeTag::Begin;
        else if (s == "Set")
            q.node_tag = NodeTag::Set;
        else if (s == "Quote")
            q.node_tag = NodeTag::Quote;
        else if (s == "LiteralString")
            q.node_tag = NodeTag::LiteralString;
        else if (s == "TypeAnnotation")
            q.node_tag = NodeTag::TypeAnnotation;
        else if (s == "Coercion")
            q.node_tag = NodeTag::Coercion;
    } else if (op == "callee") {
        q.kind = QueryExpr::Kind::Callee;
        q.str_value = std::string(ps.next());
    } else if (op == "child") {
        q.kind = QueryExpr::Kind::Child;
        q.child_index = (std::uint32_t)std::stoul(std::string(ps.next()));
        q.children.push_back(parse_expr(ps));
    } else if (op == "has-child") {
        q.kind = QueryExpr::Kind::HasChild;
        q.children.push_back(parse_expr(ps));
    } else if (op == "exists") {
        q.kind = QueryExpr::Kind::Exists;
        q.children.push_back(parse_expr(ps));
    } else if (op == "=") {
        q.kind = QueryExpr::Kind::Eq;
        q.field_name = std::string(ps.next());
        q.str_value = std::string(ps.next());
    } else if (op == ">" || op == "gt") {
        q.kind = QueryExpr::Kind::Gt;
        q.field_name = std::string(ps.next());
        q.int_value = std::stoll(std::string(ps.next()));
    } else if (op == "and") {
        q.kind = QueryExpr::Kind::And;
        while (!ps.close() && !ps.end())
            q.children.push_back(parse_expr(ps));
    } else if (op == "or") {
        q.kind = QueryExpr::Kind::Or;
        while (!ps.close() && !ps.end())
            q.children.push_back(parse_expr(ps));
    } else if (op == "not") {
        q.kind = QueryExpr::Kind::Not;
        q.children.push_back(parse_expr(ps));
    } else if (op == "has-type?" || op == "has-type") {
        q.kind = QueryExpr::Kind::HasType;
        q.str_value = std::string(ps.next());
    } else if (op == "return-type?" || op == "return-type") {
        q.kind = QueryExpr::Kind::ReturnType;
        q.str_value = std::string(ps.next());
    } else if (op == "argument-type?" || op == "argument-type") {
        q.kind = QueryExpr::Kind::ArgType;
        q.child_index = (std::uint32_t)std::stoul(std::string(ps.next()));
        q.str_value = std::string(ps.next());
    } else if (op == "ref-count") {
        q.kind = QueryExpr::Kind::RefCount;
        q.int_value = std::stoll(std::string(ps.next()));
    } else if (op == "has-error?" || op == "has-error") {
        q.kind = QueryExpr::Kind::HasError;
    } else if (op == "deopt-count" || op == ":deopt-count") {
        // Issue #62 Iter 4: global observability queries.
        q.kind = QueryExpr::Kind::DeoptCount;
    } else if (op == "arena-usage" || op == ":arena-usage") {
        q.kind = QueryExpr::Kind::ArenaUsage;
    } else if (op == "specialization-count" || op == ":specialization-count") {
        q.kind = QueryExpr::Kind::SpecializationCount;
    }

    if (!ps.end())
        ps.next(); // ')'
    return q;
}

static QueryExpr parse_expr(PS& ps) {
    if (!ps.end() && ps.open())
        return parse_list(ps);
    return {};
}

QueryExpr QueryEngine::parse(std::string_view sexpr) {
    auto tokens = tokenize(sexpr);
    PS ps{std::move(tokens), 0};
    return parse_expr(ps);
}

static constexpr int MAX_MATCH_DEPTH = 64;

bool QueryEngine::match(NodeId id, const QueryExpr& q, int depth) {
    // The pre (depth >= 0) is on the declaration in query.ixx.
    // The id < index_.ast.size() check is repeated as a soft
    // runtime check (returns false, doesn't abort).
    if (depth > MAX_MATCH_DEPTH)
        return false;
    if (id >= index_.ast.size())
        return false;
    auto v = index_.ast.get(id);
    switch (q.kind) {
        case QueryExpr::Kind::AllNodes:
            return true;
        case QueryExpr::Kind::NodeType:
            return v.tag == q.node_tag;
        case QueryExpr::Kind::Callee: {
            if (v.tag != NodeTag::Call || v.children.empty())
                return false;
            auto c = index_.ast.get(v.child(0));
            if (c.tag != NodeTag::Variable)
                return false;
            return index_.pool.resolve(c.sym_id) == q.str_value;
        }
        case QueryExpr::Kind::Child:
            return q.child_index < v.children.size() &&
                   match(v.child(q.child_index), q.children[0], depth + 1);
        case QueryExpr::Kind::HasChild:
            for (auto c : v.children)
                if (match(c, q.children[0], depth + 1))
                    return true;
            return false;
        case QueryExpr::Kind::Exists:
            if (match(id, q.children[0], depth + 1))
                return true;
            for (auto c : v.children)
                if (match(c, q, depth + 1))
                    return true;
            return false;
        case QueryExpr::Kind::Eq:
            if (q.field_name == "name" || q.field_name == "sym_id")
                return index_.pool.resolve(v.sym_id) == q.str_value;
            if (q.field_name == "int_value")
                return v.int_value == q.int_value;
            if (q.field_name == "child-count")
                return v.children.size() == (std::size_t)q.int_value;
            return false;
        case QueryExpr::Kind::Gt:
            if (q.field_name == "child-count")
                return v.children.size() > (std::size_t)q.int_value;
            if (q.field_name == "int_value")
                return v.int_value > q.int_value;
            return false;
        case QueryExpr::Kind::And:
            for (auto& c : q.children)
                if (!match(id, c, depth + 1))
                    return false;
            return true;
        case QueryExpr::Kind::Or:
            for (auto& c : q.children)
                if (match(id, c, depth + 1))
                    return true;
            return false;
        case QueryExpr::Kind::Not:
            return !q.children.empty() && !match(id, q.children[0], depth + 1);
        case QueryExpr::Kind::HasType: {
            auto node_tid = index_.ast.type_id(id);
            auto query_tid = resolve_type_id(q.str_value);
            if (node_tid == 0 || query_tid == 0)
                return true; // Any consistent with everything
            return node_tid == query_tid;
        }
        case QueryExpr::Kind::ReturnType: {
            if (v.tag != NodeTag::Call)
                return false;
            auto node_tid = index_.ast.type_id(id);
            auto query_tid = resolve_type_id(q.str_value);
            if (node_tid == 0 || query_tid == 0)
                return true; // Any consistent
            return node_tid == query_tid;
        }
        case QueryExpr::Kind::ArgType: {
            if (v.tag != NodeTag::Call)
                return false;
            if (q.child_index >= v.children.size())
                return false;
            auto cid = v.child(q.child_index);
            auto node_tid = index_.ast.type_id(cid);
            auto query_tid = resolve_type_id(q.str_value);
            if (node_tid == 0 || query_tid == 0)
                return true; // Any consistent
            return node_tid == query_tid;
        }
        case QueryExpr::Kind::RefCount: {
            if (v.sym_id == aura::ast::INVALID_SYM)
                return false;
            ensure_sym_index();
            return sym_index_->count(v.sym_id) == static_cast<std::size_t>(q.int_value);
        }
        case QueryExpr::Kind::HasError:
            // Issue #79: per-node error kind is now tracked in FlatAST's
            // error_kind_ SoA column. Populated by the type-checker and
            // runtime evaluator via flat.set_node_error(id, kind). The
            // query returns true iff the node has any non-zero error kind.
            return id < index_.ast.size() && index_.ast.node_error(id) != 0;
        default:
            return false;
    }
}

void QueryEngine::ensure_sym_index() const {
    if (sym_index_built_)
        return;
    auto* idx = new SymRefIndex(index_.ast, index_.pool);
    idx->build();
    sym_index_ = idx;
    sym_index_built_ = true;
    index_.set_sym_index(*sym_index_);
}

std::vector<NodeId> QueryEngine::execute(const QueryExpr& q) {
    // The pre (index_.ast.size() > 0) is on the declaration in query.ixx.
    std::vector<NodeId> r;

    // Fast path: simple NodeType query uses TagIndex (O(1) lookup)
    if (q.kind == QueryExpr::Kind::NodeType && q.children.empty()) {
        auto nodes = index_.by_tag(q.node_tag);
        r.assign(nodes.begin(), nodes.end());
        return r;
    }

    // General path: linear scan with full match
    for (NodeId i = 0; i < index_.ast.size(); ++i)
        if (match(i, q))
            r.push_back(i);
    return r;
}

// Issue #62 Iter 4: global observability queries. Returns the
// counter value (uint64_t) for one of the 3 new kinds, or
// 0xFFFFFFFFFFFFFFFF if the query kind isn't a global
// observability kind.
std::uint64_t QueryEngine::execute_global(const QueryExpr& q) const {
    switch (q.kind) {
        case QueryExpr::Kind::DeoptCount:
            if (metrics_provider)
                return metrics_provider->deopt_count();
            return 0;
        case QueryExpr::Kind::ArenaUsage:
            if (metrics_provider)
                return metrics_provider->arena_bytes_used();
            return 0;
        case QueryExpr::Kind::SpecializationCount:
            if (metrics_provider)
                return metrics_provider->jit_compilations();
            return 0;
        default:
            return std::numeric_limits<std::uint64_t>::max();
    }
}

// Issue #62 Iter 4: attach an external metrics source. The
// QueryEngine doesn't own a CompilerService; callers (e.g. the
// --query CLI in main.cpp) wire a metrics provider that can
// return counter values. The provider is held as a non-owning
// pointer; lifetime must outlive the QueryEngine.
void QueryEngine::set_metrics_provider(MetricsProvider* p) {
    metrics_provider = p;
}

} // namespace aura::compiler
