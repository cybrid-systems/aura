// A2: P2996 struct layout validation for ABF-serialized node types.
//
// Verifies that each AST node struct matches the expected ABF
// serialization format at compile time:
//   - First member must be NodeTag (4-byte enum)
//   - Remaining members must be ABF-serializable types
//
// Build: g++ -std=c++26 -freflection -I. tests/validate_node_layout.cpp -o build/validate_node_layout
// Run:   ./build/validate_node_layout

#include "reflect/read_auto_validate.hh"

// Replicate node structs from src/core/ast.ixx
// (can't import from modules with -freflection)

import std;
struct Expr { int _; };

enum class NodeTag : std::uint32_t {
    LiteralInt = 0x01, Variable = 0x02, Call = 0x03,
    IfExpr = 0x04, Lambda = 0x05, Let = 0x06, LetRec = 0x07,
    Define = 0x08, Begin = 0x09, Set = 0x0A, Quote = 0x0B,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
};

struct LiteralIntNode { NodeTag tag; std::int64_t value; };
struct VariableNode   { NodeTag tag; std::string name; };
struct CallNode       { NodeTag tag; Expr* function; std::vector<Expr*> args; };
struct IfExprNode     { NodeTag tag; Expr* condition; Expr* then_branch; Expr* else_branch; };
struct LambdaNode     { NodeTag tag; std::vector<std::string> params; Expr* body; };
struct LetNode        { NodeTag tag; std::string name; Expr* value; Expr* body; };
struct LetRecNode     { NodeTag tag; std::string name; Expr* value; Expr* body; };
struct DefineNode     { NodeTag tag; std::string name; Expr* value; };
struct BeginNode      { NodeTag tag; std::vector<Expr*> exprs; };
struct SetNode        { NodeTag tag; std::string name; Expr* value; };
struct QuoteNode      { NodeTag tag; Expr* value; };
struct TypeAnnotationNode { NodeTag tag; Expr* inner_expr; std::string type_name; };
struct CoercionNode { NodeTag tag; Expr* inner_expr; std::string to_type_name; };

struct BadNode        { int x; int y; };  // should fail

int main() {
    printf("=== P2996 ABF Node Struct Validation ===\n\n");

    auto c = []<typename T>(const char* name, bool expect) {
        auto err = aura::reflect::abf_validate::validate_node<T>();
        bool ok = (err == nullptr);
        if (ok == expect)
            printf("  %s %s\n", ok ? "\u2705" : "\u274c", name);
        else if (err)
            printf("  \u274c %s (unexpected: %s)\n", name, err);
        else
            printf("  \u2705 %s (unexpected: passed)\n", name);
        return ok == expect;
    };

    int passed = 0, total = 0;

    // All valid node types
    passed += c.operator()<LiteralIntNode>("LiteralIntNode", true);
    passed += c.operator()<VariableNode>("VariableNode", true);
    passed += c.operator()<CallNode>("CallNode", true);
    passed += c.operator()<IfExprNode>("IfExprNode", true);
    passed += c.operator()<LambdaNode>("LambdaNode", true);
    passed += c.operator()<LetNode>("LetNode", true);
    passed += c.operator()<LetRecNode>("LetRecNode", true);
    passed += c.operator()<DefineNode>("DefineNode", true);
    passed += c.operator()<BeginNode>("BeginNode", true);
    passed += c.operator()<SetNode>("SetNode", true);
    passed += c.operator()<QuoteNode>("QuoteNode", true);
    passed += c.operator()<TypeAnnotationNode>("TypeAnnotationNode", true);
    passed += c.operator()<CoercionNode>("CoercionNode", true);

    // Expected failures
    passed += c.operator()<BadNode>("BadNode (should fail)", false);

    total += 14;

    printf("\n  %d/%d passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
