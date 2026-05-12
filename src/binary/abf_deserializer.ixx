export module aura.binary.abf_deserializer;
import std;
import aura.core;

namespace aura::binary {

export class ABFDeserializer {
public:
    explicit ABFDeserializer(ast::ASTArena& arena) : arena_(arena) {}

    // Deserialize a single expression from ABF v2 binary data
    ast::Expr* deserialize(std::span<const std::byte> data);

    // Check if data starts with ABF v2 magic
    static bool is_abf(std::span<const std::byte> data);

private:
public:
public:
public:
    struct Reader {
        std::span<const std::byte> data;
        std::size_t pos = 0;

        std::uint64_t read_varint();
        std::int64_t read_int64_be();
        std::string read_string();
        std::span<const std::byte> read_bytes(std::size_t n);
        bool eof() const { return pos >= data.size(); }
        std::uint8_t peek_byte() const;
    };

    ast::Expr* read_node(Reader& r);
    ast::Expr* read_literal_int(Reader& r);
    ast::Expr* read_variable(Reader& r);
    ast::Expr* read_call(Reader& r);
    ast::Expr* read_if(Reader& r);
    ast::Expr* read_lambda(Reader& r);
    ast::Expr* read_let(Reader& r, bool is_rec);
    ast::Expr* read_let_nonrec(Reader& r);
    ast::Expr* read_let_rec(Reader& r);
    static void register_all_readers();
    ast::Expr* read_define(Reader& r);
    ast::Expr* read_begin(Reader& r);
    ast::Expr* read_set(Reader& r);
    ast::Expr* read_quote(Reader& r);
    ast::Expr* read_cond(Reader& r);

    ast::ASTArena& arena_;
};

} // namespace aura::binary
