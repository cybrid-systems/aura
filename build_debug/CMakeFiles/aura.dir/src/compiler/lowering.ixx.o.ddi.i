# 0 "/home/dev/code/aura/src/compiler/lowering.ixx"
# 1 "/home/dev/code/aura/build_debug//"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/lowering.ixx"
export module aura.compiler.lowering;
import std;
import aura.core;
import aura.compiler.ir;

namespace aura::compiler {


enum class BindingKind : std::uint8_t { Local, Captured, Cell };

struct Binding {
    BindingKind kind;
    std::uint32_t slot;
};


struct LoweringState {
    ast::ASTArena& arena;
    aura::ir::IRModule module;
    aura::ir::IRFunction* cur_func = nullptr;
    std::uint32_t cur_block = 0;
    std::uint32_t local_count = 0;
    std::vector<std::unordered_map<std::string, Binding>> scopes;
    std::uint32_t env_slot = 0;
    std::unordered_map<std::string, std::uint32_t> free_var_map;
    std::unordered_set<std::string> cell_free_vars;

    explicit LoweringState(ast::ASTArena& a) : arena(a) {}
    std::uint32_t alloc_local() { return local_count++; }

    void emit(aura::ir::IROpcode op, std::uint32_t op0, std::uint32_t op1 = 0, std::uint32_t op2 = 0, std::uint32_t op3 = 0) {
        if (!cur_func || cur_block >= cur_func->blocks.size()) return;
        auto& blk = cur_func->blocks[cur_block];
        blk.instructions.push_back({op, {op0, op1, op2, op3}});
    }
    std::uint32_t alloc_block() {
        if (!cur_func) return 0;
        cur_func->blocks.push_back({
            static_cast<std::uint32_t>(cur_func->blocks.size())});
        return static_cast<std::uint32_t>(cur_func->blocks.size() - 1);
    }
};




export aura::ir::IRModule lower_to_ir(ast::FlatAST& flat,
                                       ast::StringPool& pool,
                                       ast::ASTArena& arena);







export aura::ir::IRModule lower_to_ir_with_cache(
    ast::FlatAST& flat,
    ast::StringPool& pool,
    ast::ASTArena& arena,
    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
    std::vector<std::string>* cache_hits = nullptr);


export std::string unparse_node(const ast::FlatAST& flat,
                                 const ast::StringPool& pool,
                                 ast::NodeId id,
                                 int indent = 0);

}
