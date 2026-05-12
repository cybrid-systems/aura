module aura.compiler.type_checker;

namespace aura::compiler {

using namespace aura::core;

void TypeChecker::run(aura::ast::FlatAST& flat) {
    // Phase 1: Walk all nodes, identify TypeAnnotation nodes
    // Phase 2: Check type consistency (stub — just pass through for now)
    // Future: full type inference + constraint solving
    
    for (aura::ast::NodeId nh = 0; nh < flat.size(); ++nh) {
        if (flat.get(nh).tag == aura::ast::NodeTag::TypeAnnotation) {
            // TypeAnnotationNode's inner expression is in the first child
            // Type name is stored in sym_id — for now, just skip
            // Will be implemented in L6.3+
        }
    }
}

bool TypeChecker::check_type(aura::ast::FlatAST&, aura::ast::NodeId, TypeId) {
    // Stub — always passes for now
    return true;
}

TypeId TypeChecker::infer_type(aura::ast::FlatAST& flat, aura::ast::NodeId node) {
    switch (flat.get(node).tag) {
        case aura::ast::NodeTag::LiteralInt:
            return types.int_type();
        case aura::ast::NodeTag::LiteralString:
            return types.string_type();
        case aura::ast::NodeTag::TypeAnnotation: {
            // Strip annotation and infer inner expression type
            auto v = flat.get(node);
            auto inner = v.child(0);
            return infer_type(flat, inner);
        }
        default:
            return types.dynamic_type();  // unknown → Any
    }
}

} // namespace aura::compiler
