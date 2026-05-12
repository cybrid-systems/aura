module aura.compiler.query;
import std;

namespace aura::compiler {

using namespace aura::ast;

static std::vector<std::string> tokenize(std::string_view s) {
    std::vector<std::string> tokens;
    std::string cur;
    for (auto c : s) {
        if (c == '(' || c == ')') {
            if (!cur.empty()) { tokens.push_back(std::move(cur)); }
            tokens.emplace_back(1, c);
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty()) { tokens.push_back(std::move(cur)); }
        } else if (c == '"') { /* skip string literal content */ }
        else { cur += c; }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

struct PS {
    std::vector<std::string> t;
    std::size_t p = 0;
    bool open()  const { return p < t.size() && t[p] == "("; }
    bool close() const { return p < t.size() && t[p] == ")"; }
    bool end()   const { return p >= t.size(); }
    std::string next() { return p < t.size() ? t[p++] : ""; }
};

static QueryExpr parse_expr(PS& ps);

static QueryExpr parse_list(PS& ps) {
    ps.next(); // '('
    auto op = ps.next();

    QueryExpr q;
         if (op == "node-type") { q.kind = QueryExpr::Kind::NodeType; auto s = ps.next();
        if (s == "LiteralInt") q.node_tag = NodeTag::LiteralInt;
        else if (s == "Variable")  q.node_tag = NodeTag::Variable;
        else if (s == "Call")      q.node_tag = NodeTag::Call;
        else if (s == "IfExpr" || s == "If") q.node_tag = NodeTag::IfExpr;
        else if (s == "Lambda")    q.node_tag = NodeTag::Lambda;
        else if (s == "Let")       q.node_tag = NodeTag::Let;
        else if (s == "LetRec")    q.node_tag = NodeTag::LetRec;
        else if (s == "Define")    q.node_tag = NodeTag::Define;
        else if (s == "Begin")     q.node_tag = NodeTag::Begin;
        else if (s == "Set")       q.node_tag = NodeTag::Set;
        else if (s == "Quote")     q.node_tag = NodeTag::Quote; }
    else if (op == "callee")   { q.kind = QueryExpr::Kind::Callee; q.str_value = std::string(ps.next()); }
    else if (op == "child")    { q.kind = QueryExpr::Kind::Child; q.child_index = (std::uint32_t)std::stoul(std::string(ps.next())); q.children.push_back(parse_expr(ps)); }
    else if (op == "has-child"){ q.kind = QueryExpr::Kind::HasChild; q.children.push_back(parse_expr(ps)); }
    else if (op == "exists")   { q.kind = QueryExpr::Kind::Exists; q.children.push_back(parse_expr(ps)); }
    else if (op == "=")        { q.kind = QueryExpr::Kind::Eq; q.field_name = std::string(ps.next()); q.str_value = std::string(ps.next()); }
    else if (op == ">" || op == "gt") { q.kind = QueryExpr::Kind::Gt; q.field_name = std::string(ps.next()); q.int_value = std::stoll(std::string(ps.next())); }
    else if (op == "and")      { q.kind = QueryExpr::Kind::And; while (!ps.close() && !ps.end()) q.children.push_back(parse_expr(ps)); }
    else if (op == "or")       { q.kind = QueryExpr::Kind::Or;  while (!ps.close() && !ps.end()) q.children.push_back(parse_expr(ps)); }
    else if (op == "not")      { q.kind = QueryExpr::Kind::Not; q.children.push_back(parse_expr(ps)); }

    if (!ps.end()) ps.next(); // ')'
    return q;
}

static QueryExpr parse_expr(PS& ps) {
    if (!ps.end() && ps.open()) return parse_list(ps);
    return {};
}

QueryExpr QueryEngine::parse(std::string_view sexpr) {
    auto tokens = tokenize(sexpr);
    PS ps{std::move(tokens), 0};
    return parse_expr(ps);
}

bool QueryEngine::match(NodeId id, const QueryExpr& q) {
    if (id >= index_.ast.size()) return false;
    auto v = index_.ast.get(id);
    switch (q.kind) {
    case QueryExpr::Kind::AllNodes: return true;
    case QueryExpr::Kind::NodeType: return v.tag == q.node_tag;
    case QueryExpr::Kind::Callee: {
        if (v.tag != NodeTag::Call || v.children.empty()) return false;
        auto c = index_.ast.get(v.child(0));
        if (c.tag != NodeTag::Variable) return false;
        return index_.pool.resolve(c.sym_id) == q.str_value;
    }
    case QueryExpr::Kind::Child:
        return q.child_index < v.children.size() && match(v.child(q.child_index), q.children[0]);
    case QueryExpr::Kind::HasChild:
        for (auto c : v.children) if (match(c, q.children[0])) return true;
        return false;
    case QueryExpr::Kind::Exists:
        if (match(id, q.children[0])) return true;
        for (auto c : v.children) if (match(c, q)) return true;
        return false;
    case QueryExpr::Kind::Eq:
        if (q.field_name == "name" || q.field_name == "sym_id")
            return index_.pool.resolve(v.sym_id) == q.str_value;
        if (q.field_name == "int_value") return v.int_value == q.int_value;
        if (q.field_name == "child-count") return v.children.size() == (std::size_t)q.int_value;
        return false;
    case QueryExpr::Kind::Gt:
        if (q.field_name == "child-count") return v.children.size() > (std::size_t)q.int_value;
        if (q.field_name == "int_value") return v.int_value > q.int_value;
        return false;
    case QueryExpr::Kind::And: for (auto& c : q.children) if (!match(id, c)) return false; return true;
    case QueryExpr::Kind::Or:  for (auto& c : q.children) if (match(id, c)) return true; return false;
    case QueryExpr::Kind::Not: return !q.children.empty() && !match(id, q.children[0]);
    default: return false;
    }
}

std::vector<NodeId> QueryEngine::execute(const QueryExpr& q) {
    std::vector<NodeId> r;
    for (NodeId i = 0; i < index_.ast.size(); ++i)
        if (match(i, q)) r.push_back(i);
    return r;
}

} // namespace aura::compiler
