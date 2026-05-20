# 0 "/home/dev/code/aura/src/compiler/query_impl.cpp"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/query_impl.cpp"
module aura.compiler.query;
import std;

namespace aura::compiler {

using namespace aura::ast;



static std::uint32_t resolve_type_id(const std::string& name) {
    if (name == "Int") return 1;
    if (name == "Bool") return 2;
    if (name == "String") return 3;
    if (name == "Void") return 4;
    return 0;
}

static std::vector<std::string> tokenize(std::string_view s) {
    std::vector<std::string> tokens;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = s[i];
        if (c == '(' || c == ')') {
            if (!cur.empty()) { tokens.push_back(std::move(cur)); }
            tokens.emplace_back(1, c);
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty()) { tokens.push_back(std::move(cur)); }
        } else if (c == '"') {

            i++;
            cur.clear();
            while (i < s.size() && s[i] != '"') { cur += s[i]; i++; }
            if (i < s.size()) i++;
            tokens.push_back(std::move(cur));
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
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
    ps.next();
    auto op = ps.next();

    QueryExpr q;
         if (op == "node-type") { q.kind = QueryExpr::Kind::NodeType; auto s = ps.next();
        if (s == "LiteralInt") q.node_tag = NodeTag::LiteralInt;
        else if (s == "Variable") q.node_tag = NodeTag::Variable;
        else if (s == "Call") q.node_tag = NodeTag::Call;
        else if (s == "IfExpr" || s == "If") q.node_tag = NodeTag::IfExpr;
        else if (s == "Lambda") q.node_tag = NodeTag::Lambda;
        else if (s == "Let") q.node_tag = NodeTag::Let;
        else if (s == "LetRec") q.node_tag = NodeTag::LetRec;
        else if (s == "Define") q.node_tag = NodeTag::Define;
        else if (s == "Begin") q.node_tag = NodeTag::Begin;
        else if (s == "Set") q.node_tag = NodeTag::Set;
        else if (s == "Quote") q.node_tag = NodeTag::Quote;
        else if (s == "LiteralString") q.node_tag = NodeTag::LiteralString;
        else if (s == "TypeAnnotation") q.node_tag = NodeTag::TypeAnnotation;
        else if (s == "Coercion") q.node_tag = NodeTag::Coercion; }
    else if (op == "callee") { q.kind = QueryExpr::Kind::Callee; q.str_value = std::string(ps.next()); }
    else if (op == "child") { q.kind = QueryExpr::Kind::Child; q.child_index = (std::uint32_t)std::stoul(std::string(ps.next())); q.children.push_back(parse_expr(ps)); }
    else if (op == "has-child"){ q.kind = QueryExpr::Kind::HasChild; q.children.push_back(parse_expr(ps)); }
    else if (op == "exists") { q.kind = QueryExpr::Kind::Exists; q.children.push_back(parse_expr(ps)); }
    else if (op == "=") { q.kind = QueryExpr::Kind::Eq; q.field_name = std::string(ps.next()); q.str_value = std::string(ps.next()); }
    else if (op == ">" || op == "gt") { q.kind = QueryExpr::Kind::Gt; q.field_name = std::string(ps.next()); q.int_value = std::stoll(std::string(ps.next())); }
    else if (op == "and") { q.kind = QueryExpr::Kind::And; while (!ps.close() && !ps.end()) q.children.push_back(parse_expr(ps)); }
    else if (op == "or") { q.kind = QueryExpr::Kind::Or; while (!ps.close() && !ps.end()) q.children.push_back(parse_expr(ps)); }
    else if (op == "not") { q.kind = QueryExpr::Kind::Not; q.children.push_back(parse_expr(ps)); }
    else if (op == "has-type?" || op == "has-type") { q.kind = QueryExpr::Kind::HasType; q.str_value = std::string(ps.next()); }
    else if (op == "return-type?" || op == "return-type") { q.kind = QueryExpr::Kind::ReturnType; q.str_value = std::string(ps.next()); }
    else if (op == "argument-type?" || op == "argument-type") { q.kind = QueryExpr::Kind::ArgType; q.child_index = (std::uint32_t)std::stoul(std::string(ps.next())); q.str_value = std::string(ps.next()); }
    else if (op == "ref-count") { q.kind = QueryExpr::Kind::RefCount; q.int_value = std::stoll(std::string(ps.next())); }
    else if (op == "has-error?" || op == "has-error") { q.kind = QueryExpr::Kind::HasError; }

    if (!ps.end()) ps.next();
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

static constexpr int MAX_MATCH_DEPTH = 64;

bool QueryEngine::match(NodeId id, const QueryExpr& q, int depth) {
    if (depth > MAX_MATCH_DEPTH) return false;
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
        return q.child_index < v.children.size() && match(v.child(q.child_index), q.children[0], depth+1);
    case QueryExpr::Kind::HasChild:
        for (auto c : v.children) if (match(c, q.children[0], depth+1)) return true;
        return false;
    case QueryExpr::Kind::Exists:
        if (match(id, q.children[0], depth+1)) return true;
        for (auto c : v.children) if (match(c, q, depth+1)) return true;
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
    case QueryExpr::Kind::And: for (auto& c : q.children) if (!match(id, c, depth+1)) return false; return true;
    case QueryExpr::Kind::Or: for (auto& c : q.children) if (match(id, c, depth+1)) return true; return false;
    case QueryExpr::Kind::Not: return !q.children.empty() && !match(id, q.children[0], depth+1);
    case QueryExpr::Kind::HasType:
return index_.ast.type_id(id) != 0 && index_.ast.type_id(id) == resolve_type_id(q.str_value);
    case QueryExpr::Kind::ReturnType: {
        if (v.tag != NodeTag::Call) return false;
        return index_.ast.type_id(id) != 0 && index_.ast.type_id(id) == resolve_type_id(q.str_value);
    }
    case QueryExpr::Kind::ArgType: {
        if (v.tag != NodeTag::Call) return false;
        if (q.child_index >= v.children.size()) return false;
        auto cid = v.child(q.child_index);
        return index_.ast.type_id(cid) != 0 && index_.ast.type_id(cid) == resolve_type_id(q.str_value);
    }
    case QueryExpr::Kind::RefCount: {
        if (v.sym_id == aura::ast::INVALID_SYM) return false;
        ensure_sym_index();
        return sym_index_->count(v.sym_id) == static_cast<std::size_t>(q.int_value);
    }
    case QueryExpr::Kind::HasError:
        return false;
    default: return false;
    }
}

void QueryEngine::ensure_sym_index() const {
    if (sym_index_built_) return;
    auto* idx = new SymRefIndex(index_.ast, index_.pool);
    idx->build();
    sym_index_ = idx;
    sym_index_built_ = true;
    index_.set_sym_index(*sym_index_);
}

std::vector<NodeId> QueryEngine::execute(const QueryExpr& q) {
    std::vector<NodeId> r;


    if (q.kind == QueryExpr::Kind::NodeType && q.children.empty()) {
        auto nodes = index_.by_tag(q.node_tag);
        r.assign(nodes.begin(), nodes.end());
        return r;
    }


    for (NodeId i = 0; i < index_.ast.size(); ++i)
        if (match(i, q)) r.push_back(i);
    return r;
}

}
