// evaluator_primitives_compile.cpp — P0 step 14: compile:* / concurrency:* / syntax-marker
// primitives aura.compiler.evaluator module partition; registered via
// evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "per_defuse_index.h"
#include "hash_meta.h"    // FNV constants (#901)
#include "basis_points.h" // #905

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.mutators; // Phase 4 follow-up #4: (compile:mutator-dispatch-stats)
import aura.core.type;
import aura.compiler.ir; // Issue #375: (compile:ir-stats) needs IROpcode + IRModule
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.pass_manager;
import aura.compiler.query;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;


// Issue #909: thin compile registration dispatcher
aura::ast::FlatAST* CompilePrims::pick_macro_flat(Evaluator& ev) {
    return ev.current_flat() ? ev.current_flat() : ev.workspace_flat();
}

void CompilePrims::register_all(PrimRegistrar add, Evaluator& ev) {
    register_compile_p0(add, ev);
    register_compile_p1(add, ev);
    register_compile_p2(add, ev);
    register_compile_p3(add, ev);
    register_compile_p4(add, ev);
    register_compile_p5(add, ev);
    register_compile_p6(add, ev);
    register_compile_p7(add, ev);
    register_compile_p8(add, ev);
    register_compile_p9(add, ev);
    register_compile_p10(add, ev);
    register_compile_p11(add, ev);
    register_compile_p12(add, ev);
    register_compile_p13(add, ev);
    register_compile_p14(add, ev);
    register_compile_p15(add, ev);
    register_compile_p16(add, ev);
    register_compile_p17(add, ev);
    register_compile_p18(add, ev);
    register_compile_p19(add, ev);
    register_compile_p20(add, ev);
    register_compile_p21(add, ev);
    register_compile_p22(add, ev);
    register_compile_p23(add, ev);
    register_compile_p24(add, ev);
    register_compile_p25(add, ev);
    register_compile_p26(add, ev);
    register_compile_p27(add, ev);
    register_compile_p28(add, ev);
    register_compile_p29(add, ev);
    register_compile_p30(add, ev);
    register_compile_p31(add, ev);
    register_compile_p32(add, ev);
    register_compile_p33(add, ev);
    register_compile_p34(add, ev);
    register_compile_p35(add, ev);
    register_compile_p36(add, ev);
    register_compile_p37(add, ev);
    register_compile_p38(add, ev);
    register_compile_p39(add, ev);
    register_compile_p40(add, ev);
    register_compile_p41(add, ev);
    register_compile_p42(add, ev);
    register_compile_p43(add, ev);
    register_compile_p44(add, ev);
    register_compile_p45(add, ev);
    register_compile_p46(add, ev);
    register_compile_p47(add, ev);
    register_compile_p48(add, ev);
    register_compile_p49(add, ev);
    register_compile_p50(add, ev);
    register_compile_p51(add, ev);
    register_compile_p52(add, ev);
    register_compile_p53(add, ev);
    register_compile_p54(add, ev);
    register_compile_p55(add, ev);
    register_compile_p56(add, ev);
    register_compile_p57(add, ev);
    register_compile_p58(add, ev);
    register_compile_p59(add, ev);
    register_compile_p60(add, ev);
    register_compile_p61(add, ev);
    register_compile_p62(add, ev);
    register_compile_p63(add, ev);
}

void register_compile_primitives(PrimRegistrar add, Evaluator& ev) {
    CompilePrims::register_all(add, ev);
}


} // namespace aura::compiler::primitives_detail
