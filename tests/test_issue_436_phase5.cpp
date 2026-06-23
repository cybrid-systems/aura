// @category: integration
// @reason: SV type definitions (typedef / enum / struct) +
//          package + import (Issue #436 Phase 5: SV type
//          system + encapsulation foundation).

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_436_phase5_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::string run_string(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r)
        return "";
    auto& v = *r;
    if (!aura::compiler::types::is_string(v))
        return "";
    auto idx = aura::compiler::types::as_string_idx(v);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return "";
    return std::string(heap[idx]);
}

static std::string run_symbol_str(aura::compiler::CompilerService& cs, const std::string& src) {
    return run_string(cs, std::string("(eda:name-str ") + src + ")");
}

// ── AC1: typedef constructor + accessors ──
bool test_typedef_constructor() {
    std::println("\n--- AC1: make-eda:typedef + accessors ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define t (make-eda:typedef 'byte_t 8))")) {
        ++g_failed;
        return false;
    }
    auto is_t = run_int(cs, "(if (eda:typedef? t) 1 0)");
    CHECK(is_t == 1, "(eda:typedef? t) returns #t");

    auto name = run_symbol_str(cs, "(eda:typedef-name t)");
    CHECK(name == "byte_t", "(eda:typedef-name t) == 'byte_t'");

    auto width = run_int(cs, "(eda:typedef-width t)");
    CHECK(width == 8, "(eda:typedef-width t) == 8");

    return true;
}

// ── AC2: emit typedef → "typedef logic [W-1:0] name;" ──
bool test_emit_typedef() {
    std::println("\n--- AC2: eda:emit-typedef ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define t (make-eda:typedef 'byte_t 8))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-typedef t)");
    CHECK(s == "typedef logic [8-1:0] byte_t;",
          std::string("typedef emit = \"") + s + "\"");

    // Single-bit typedef (no [..])
    if (!cs.eval("(define t1 (make-eda:typedef 'flag 1))")) {
        ++g_failed;
        return false;
    }
    auto s1 = run_string(cs, "(eda:emit-typedef t1)");
    CHECK(s1 == "typedef logic flag;",
          std::string("1-bit typedef emit = \"") + s1 + "\"");

    return true;
}

// ── AC3: enum IR + auto-width + emit ──
bool test_enum_constructor_and_emit() {
    std::println("\n--- AC3: make-eda:enum + auto-width + emit ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // 3 members → width = 2 (need 2 bits to hold 0..2)
    if (!cs.eval("(define e (make-eda:enum 'state_t '(IDLE BUSY DONE)))")) {
        ++g_failed;
        return false;
    }
    auto is_e = run_int(cs, "(if (eda:enum? e) 1 0)");
    CHECK(is_e == 1, "(eda:enum? e) returns #t");

    auto name = run_symbol_str(cs, "(eda:enum-name e)");
    CHECK(name == "state_t", "enum name == 'state_t'");

    auto mem_count = run_int(cs, "(length (eda:enum-members e))");
    CHECK(mem_count == 3, "enum has 3 members");

    auto w = run_int(cs, "(eda:enum-width (eda:enum-members e))");
    CHECK(w == 2, "auto-width for 3 members == 2");

    auto s = run_string(cs, "(eda:emit-enum e)");
    bool has_typedef = (s.find("typedef enum logic [2-1:0]") != std::string::npos);
    CHECK(has_typedef, "enum emit starts with 'typedef enum logic [2-1:0]'");
    bool has_members = (s.find("IDLE, BUSY, DONE") != std::string::npos);
    CHECK(has_members, "enum emit lists members");
    bool has_name = (s.find("state_t;") != std::string::npos);
    CHECK(has_name, "enum emit ends with 'state_t;'");

    return true;
}

// ── AC4: struct (packed) IR + emit ──
bool test_struct_constructor_and_emit() {
    std::println("\n--- AC4: make-eda:struct packed + emit ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define s "
                 "  (make-eda:struct 'packet_t "
                 "    (list "
                 "      (make-eda:logic 'addr 16) "
                 "      (make-eda:logic 'data 32) "
                 "      (make-eda:bit   'valid 1))))")) {
        ++g_failed;
        return false;
    }
    auto is_s = run_int(cs, "(if (eda:struct? s) 1 0)");
    CHECK(is_s == 1, "(eda:struct? s) returns #t");

    auto name = run_symbol_str(cs, "(eda:struct-name s)");
    CHECK(name == "packet_t", "struct name == 'packet_t'");

    auto fcount = run_int(cs, "(length (eda:struct-fields s))");
    CHECK(fcount == 3, "struct has 3 fields");

    auto emit_s = run_string(cs, "(eda:emit-struct s)");
    bool has_struct_packed = (emit_s.find("typedef struct packed") != std::string::npos);
    CHECK(has_struct_packed, "struct emit has 'typedef struct packed'");
    bool has_addr = (emit_s.find("addr") != std::string::npos);
    bool has_data = (emit_s.find("data") != std::string::npos);
    bool has_valid = (emit_s.find("valid") != std::string::npos);
    bool has_packet_t = (emit_s.find("packet_t;") != std::string::npos);
    CHECK(has_addr && has_data && has_valid,
          "struct emit lists all 3 field names");
    CHECK(has_packet_t, "struct emit ends with 'packet_t;'");

    return true;
}

// ── AC5: package + import IR + emit ──
bool test_package_and_import() {
    std::println("\n--- AC5: make-eda:package + make-eda:import ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define pkg "
                 "  (make-eda:package 'my_pkg "
                 "    (list "
                 "      (make-eda:typedef 'byte_t 8) "
                 "      (make-eda:enum 'state_t '(IDLE BUSY)) "
                 "      (make-eda:struct 'pkt_t "
                 "        (list (make-eda:logic 'a 8))))))")) {
        ++g_failed;
        return false;
    }
    auto is_pkg = run_int(cs, "(if (eda:package? pkg) 1 0)");
    CHECK(is_pkg == 1, "(eda:package? pkg) returns #t");

    auto pname = run_symbol_str(cs, "(eda:package-name pkg)");
    CHECK(pname == "my_pkg", "package name == 'my_pkg'");

    auto pbody_len = run_int(cs, "(length (eda:package-body pkg))");
    CHECK(pbody_len == 3, "package body has 3 items");

    auto ps = run_string(cs, "(eda:emit-package pkg)");
    bool has_package_kw = (ps.find("package my_pkg;") != std::string::npos);
    bool has_endpackage = (ps.find("endpackage") != std::string::npos);
    CHECK(has_package_kw, "package emit starts with 'package my_pkg;'");
    CHECK(has_endpackage, "package emit ends with 'endpackage'");

    // import
    if (!cs.eval("(define imp (make-eda:import 'my_pkg))")) {
        ++g_failed;
        return false;
    }
    auto is_imp = run_int(cs, "(if (eda:import? imp) 1 0)");
    CHECK(is_imp == 1, "(eda:import? imp) returns #t");

    auto is_emitted = run_string(cs, "(eda:emit-import imp)");
    CHECK(is_emitted == "import my_pkg::*;",
          std::string("import emit = \"") + is_emitted + "\"");

    return true;
}

// ── AC6: type-defs + import in a module body (full emit-verilog) ──
bool test_module_with_typedef_enum_import() {
    std::println("\n--- AC6: typedef + enum + import inside a module ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define m "
                 "  (make-eda:module 'cpu_top "
                 "    (list (make-eda:port 'clk 'input 1) "
                 "          (make-eda:port 'q 'output 8)) "
                 "    (list "
                 "      (make-eda:import 'my_pkg) "
                 "      (make-eda:typedef 'state_t 3) "
                 "      (make-eda:enum 'phase_t '(RESET RUN DONE)) "
                 "      (make-eda:logic 'counter 8) "
                 "      (make-eda:always-ff "
                 "        (list (make-eda:sensitivity 'posedge 'clk)) "
                 "        (list))))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-verilog m)");

    bool has_module = (s.find("module cpu_top") != std::string::npos);
    CHECK(has_module, "module emits 'module cpu_top'");

    bool has_import = (s.find("import my_pkg::*;") != std::string::npos);
    CHECK(has_import, "module body contains 'import my_pkg::*;'");

    bool has_typedef = (s.find("typedef logic [3-1:0] state_t;") != std::string::npos);
    CHECK(has_typedef, "module body contains 'typedef logic [3-1:0] state_t;'");

    bool has_enum = (s.find("typedef enum logic [2-1:0]") != std::string::npos);
    CHECK(has_enum, "module body contains 'typedef enum logic [2-1:0]'");

    bool has_enum_members = (s.find("RESET, RUN, DONE") != std::string::npos);
    CHECK(has_enum_members, "module body lists enum members");

    bool has_counter = (s.find("logic [8-1:0] counter;") != std::string::npos);
    CHECK(has_counter, "module body contains 'logic [8-1:0] counter;'");

    bool has_ff = (s.find("always_ff") != std::string::npos);
    CHECK(has_ff, "module body contains 'always_ff'");

    bool has_endmodule = (s.find("endmodule") != std::string::npos);
    CHECK(has_endmodule, "module ends with 'endmodule'");

    return true;
}

// ── AC7: display dispatch for new types ──
bool test_display_dispatch() {
    std::println("\n--- AC7: eda:display dispatches typedef/enum/struct/package/import ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define t (make-eda:typedef 'word_t 16))")) {
        ++g_failed;
        return false;
    }
    auto rt = cs.eval("(eda:display t)");
    CHECK(rt.has_value(), "(eda:display typedef) dispatches without crash");

    if (!cs.eval("(define e (make-eda:enum 'mode_t '(A B C D)))")) {
        ++g_failed;
        return false;
    }
    auto re = cs.eval("(eda:display e)");
    CHECK(re.has_value(), "(eda:display enum) dispatches without crash");

    if (!cs.eval("(define s (make-eda:struct 'pair_t "
                 "  (list (make-eda:logic 'x 8) (make-eda:logic 'y 8))))")) {
        ++g_failed;
        return false;
    }
    auto rs = cs.eval("(eda:display s)");
    CHECK(rs.has_value(), "(eda:display struct) dispatches without crash");

    if (!cs.eval("(define pkg (make-eda:package 'p1 '()))")) {
        ++g_failed;
        return false;
    }
    auto rp = cs.eval("(eda:display pkg)");
    CHECK(rp.has_value(), "(eda:display package) dispatches without crash");

    if (!cs.eval("(define imp (make-eda:import 'p1))")) {
        ++g_failed;
        return false;
    }
    auto ri = cs.eval("(eda:display imp)");
    CHECK(ri.has_value(), "(eda:display import) dispatches without crash");

    return true;
}

int run_tests() {
    std::println("Issue #436 Phase 5 (typedef / enum / struct / package / import)\n");
    test_typedef_constructor();
    test_emit_typedef();
    test_enum_constructor_and_emit();
    test_struct_constructor_and_emit();
    test_package_and_import();
    test_module_with_typedef_enum_import();
    test_display_dispatch();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_436_phase5_detail

int aura_issue_436_phase5_run() { return aura_issue_436_phase5_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_436_phase5_run(); }
#endif