module;
#include "reflect/abf_reader_registry.hh"
#include "reflect/tag_dispatch.hh"
module aura.binary.abf_deserializer;
import std;

namespace aura::binary {

std::uint64_t ABFDeserializer::Reader::read_varint() {
    std::uint64_t result = 0;
    int shift = 0;
    while (pos < data.size()) {
        auto byte = static_cast<std::uint8_t>(data[pos++]);
        result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return result;
        shift += 7;
    }
    throw std::runtime_error("truncated varint");
}

std::int64_t ABFDeserializer::Reader::read_int64_be() {
    if (pos + 8 > data.size()) throw std::runtime_error("truncated int64");
    std::uint64_t val = 0;
    for (int i = 0; i < 8; ++i)
        val = (val << 8) | static_cast<std::uint8_t>(data[pos++]);
    return static_cast<std::int64_t>(val);
}

std::string ABFDeserializer::Reader::read_string() {
    auto len = read_varint();
    if (pos + len > data.size()) throw std::runtime_error("truncated string");
    std::string result(reinterpret_cast<const char*>(data.data() + pos), len);
    pos += len;
    return result;
}

std::span<const std::byte> ABFDeserializer::Reader::read_bytes(std::size_t n) {
    if (pos + n > data.size()) throw std::runtime_error("truncated bytes");
    auto result = data.subspan(pos, n);
    pos += n;
    return result;
}

std::uint8_t ABFDeserializer::Reader::peek_byte() const {
    if (pos >= data.size()) return 0;
    return static_cast<std::uint8_t>(data[pos]);
}

bool ABFDeserializer::is_abf(std::span<const std::byte> data) {
    if (data.size() < 4) return false;
    return data[0] == std::byte{'A'} && data[1] == std::byte{'B'} &&
           data[2] == std::byte{'F'} && data[3] == std::byte{'2'};
}

ast::Expr* ABFDeserializer::deserialize(std::span<const std::byte> data) {
    Reader r{data, 0};
    if (!is_abf(data)) throw std::runtime_error("invalid ABF magic");
    r.pos = 4;
    r.read_varint();  // version
    r.read_varint();  // phase ID
    return read_node(r);
}

// Wrap read functions for dispatch table

// and bridge through the deserializer.

ast::Expr* ABFDeserializer::read_node(Reader& r) {
    auto tag = static_cast<std::uint32_t>(r.read_varint());
    r.read_varint();  // ExtID
    auto ext_len = r.read_varint();  // ExtLen
    if (ext_len > 0) r.read_bytes(ext_len);

    // Dispatch table: index by tag value, 0x01-0x0C
    // ReadFn = member function pointer: (ABFDeserializer::*)(Reader&)
    using MemFn = ast::Expr* (ABFDeserializer::*)(Reader&);
    static constexpr MemFn reader_for[] = {
        nullptr,                    // 0x00 unused
        &ABFDeserializer::read_literal_int, // 0x01
        &ABFDeserializer::read_variable,    // 0x02
        &ABFDeserializer::read_call,        // 0x03
        &ABFDeserializer::read_if,          // 0x04
        &ABFDeserializer::read_lambda,      // 0x05
        &ABFDeserializer::read_let_nonrec, // 0x06
        &ABFDeserializer::read_let_rec,  // 0x07 LetRec
        &ABFDeserializer::read_define,      // 0x08
        &ABFDeserializer::read_begin,       // 0x09
        &ABFDeserializer::read_set,         // 0x0A
        &ABFDeserializer::read_quote,       // 0x0B
        &ABFDeserializer::read_cond,        // 0x0C
    };

    if (tag < sizeof(reader_for)/sizeof(reader_for[0]) && reader_for[tag]) {
        return (this->*reader_for[tag])(r);
    }
    throw std::runtime_error("unknown tag: " + std::to_string(tag));
}

ast::Expr* ABFDeserializer::read_literal_int(Reader& r) {
    return arena_.create<ast::Expr>(ast::LiteralIntNode{{ast::NodeTag::LiteralInt}, r.read_int64_be()});
}

ast::Expr* ABFDeserializer::read_variable(Reader& r) {
    return arena_.create<ast::Expr>(ast::VariableNode{{ast::NodeTag::Variable}, r.read_string()});
}

ast::Expr* ABFDeserializer::read_call(Reader& r) {
    auto* func = read_node(r);
    auto arg_count = r.read_varint();
    ast::CallNode call{{ast::NodeTag::Call}, func, {}};
    for (std::uint64_t i = 0; i < arg_count; ++i)
        call.args.push_back(read_node(r));
    return arena_.create<ast::Expr>(std::move(call));
}

ast::Expr* ABFDeserializer::read_if(Reader& r) {
    auto* c = read_node(r); auto* t = read_node(r); auto* e = read_node(r);
    return arena_.create<ast::Expr>(ast::IfExprNode{{ast::NodeTag::IfExpr}, c, t, e});
}

ast::Expr* ABFDeserializer::read_lambda(Reader& r) {
    auto n = r.read_varint();
    ast::LambdaNode lam{{ast::NodeTag::Lambda}, {}, nullptr};
    for (std::uint64_t i = 0; i < n; ++i) lam.params.push_back(r.read_string());
    lam.body = read_node(r);
    return arena_.create<ast::Expr>(std::move(lam));
}

ast::Expr* ABFDeserializer::read_let_nonrec(Reader& r) {
    return read_let(r, false);
}

ast::Expr* ABFDeserializer::read_let_rec(Reader& r) {
    return read_let(r, true);
}

ast::Expr* ABFDeserializer::read_let(Reader& r, bool is_rec) {
    // ABF format: count | name1 val1 | name2 val2 | ... | nameN valN | body
    auto count = r.read_varint();

    // Read all bindings first (vector of {name, value})
    struct Binding { std::string name; ast::Expr* val; };
    std::vector<Binding> bindings;
    bindings.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        bindings.push_back({r.read_string(), read_node(r)});
    }

    // Read the body (comes after all bindings)
    auto* body = read_node(r);

    // Build nested let structure from the inside out:
    // Let(x, 1, Let(y, 2, Let(z, 3, body)))
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
        auto node = is_rec
            ? ast::Expr(ast::LetRecNode{{ast::NodeTag::LetRec}, it->name, it->val, body})
            : ast::Expr(ast::LetNode{{ast::NodeTag::Let}, it->name, it->val, body});
        body = arena_.create<ast::Expr>(std::move(node));
    }

    return body;
}

ast::Expr* ABFDeserializer::read_define(Reader& r) {
    auto name = r.read_string();
    auto* val = read_node(r);
    return arena_.create<ast::Expr>(ast::DefineNode{{ast::NodeTag::Define}, name, val});
}

ast::Expr* ABFDeserializer::read_begin(Reader& r) {
    auto count = r.read_varint();
    ast::BeginNode begin{{}, {}};
    for (std::uint64_t i = 0; i < count; ++i)
        begin.exprs.push_back(read_node(r));
    return arena_.create<ast::Expr>(std::move(begin));
}

ast::Expr* ABFDeserializer::read_set(Reader& r) {
    auto name = r.read_string();
    auto* val = read_node(r);
    return arena_.create<ast::Expr>(ast::SetNode{{ast::NodeTag::Set}, name, val});
}


ast::Expr* ABFDeserializer::read_cond(Reader& r) {
    // Format: test1 val1 [TAG-IF test2 val2 [TAG-IF test3 v3 ...]]
    // Read the first (test, val) pair
    auto* test = read_node(r);
    auto* val = read_node(r);
    // Check if there's more (peek for another IF continuation tag)
    if (!r.eof() && r.peek_byte() == 0x04) {
        r.read_varint();  // skip TAG-IF
        r.read_varint();  // skip ext_id
        r.read_varint();  // skip ext_len
        auto* else_branch = read_cond(r);
        return arena_.create<ast::Expr>(
            ast::IfExprNode{{ast::NodeTag::IfExpr}, test, val, else_branch});
    }
    // Last clause: just the value
    return val;
}

ast::Expr* ABFDeserializer::read_quote(Reader& r) {
    auto* val = read_node(r);
    return arena_.create<ast::Expr>(ast::QuoteNode{{ast::NodeTag::Quote}, val});
}


void ABFDeserializer::register_all_readers() {
    auto reg = ::aura::reflect::abf_registry::ReaderRegistry::reg;
    reg(0x01, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_literal_int(*(Reader*)r); });
    reg(0x02, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_variable(*(Reader*)r); });
    reg(0x03, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_call(*(Reader*)r); });
    reg(0x04, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_if(*(Reader*)r); });
    reg(0x05, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_lambda(*(Reader*)r); });
    reg(0x06, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_let(*(Reader*)r, false); });
    reg(0x07, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_let(*(Reader*)r, true); });
    reg(0x08, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_define(*(Reader*)r); });
    reg(0x09, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_begin(*(Reader*)r); });
    reg(0x0A, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_set(*(Reader*)r); });
    reg(0x0B, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_quote(*(Reader*)r); });
    reg(0x0C, [](void*r, void*o) -> void* { return ((ABFDeserializer*)o)->read_cond(*(Reader*)r); });
}
} // namespace aura::binary
