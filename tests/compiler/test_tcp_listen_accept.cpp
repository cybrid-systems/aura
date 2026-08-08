// @category: unit
// @reason: Issue #2771 — tcp-listen / tcp-accept server path for multi-host
//          denseness (Hermes Phase 5 residual). Prefer-existing json_io batch
//          membership per #81967 (no test_issue_2771.cpp).
//
//   AC1: tcp-listen / tcp-local-port / tcp-accept / tcp-accept-timeout registered
//   AC2: listen 127.0.0.1:ephemeral, accept one connection, echo payload
//   AC3: client tcp-connect + send/recv (second fiber, stdin denseness)
//   AC4: std/socket export + adaptive help + #2771 cite
//   AC5: coverage linter wired; no docs/design/2771-*

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static bool eval_bool(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

static void ac1_prims_registered() {
    std::println("\n--- #2771 AC1: server prims registered ---");
    CompilerService cs;
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    for (const char* name : {"tcp-listen", "tcp-local-port", "tcp-accept", "tcp-accept-timeout",
                             "tcp-connect", "tcp-send", "tcp-recv", "tcp-close"}) {
        CHECK(eval_bool(cs, std::string("(procedure? ") + name + ")"),
              std::string("AC1: procedure? ") + name);
    }
}

static void ac2_ac3_echo_fiber_client() {
    std::println("\n--- #2771 AC2/AC3: listen/accept echo + fiber client ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;

    // Thread-backend fiber client + blocking accept (stdin denseness).
    const char* prog = R"AURA(
(begin
  (define L (tcp-listen 0))
  (define p (tcp-local-port L))
  (define f
    (fiber:spawn
      (lambda ()
        (let ((c (tcp-connect "127.0.0.1" p)))
          (tcp-send c "ping")
          (let ((reply (tcp-recv c 64)))
            (tcp-close c)
            reply)))))
  (define s (tcp-accept L))
  (define msg (tcp-recv s 64))
  (tcp-send s (string-append "echo:" msg))
  (tcp-close s)
  (tcp-close L)
  (equal? (fiber:join f) "echo:ping"))
)AURA";
    CHECK(eval_bool(cs, prog), "AC2/AC3: fiber client received echo:ping");
}

static void ac3b_accept_timeout_idle() {
    std::println("\n--- #2771 AC3b: accept-timeout idle → not integer ---");
    setenv("AURA_SANDBOX", "off", 1);
    CompilerService cs;
    CHECK(eval_bool(cs, "(begin (define L (tcp-listen 0)) "
                        "(define r (tcp-accept-timeout L 0)) "
                        "(tcp-close L) "
                        "(not (integer? r)))"),
          "AC3b: tcp-accept-timeout 0 on idle → not integer");
}

static void ac4_stdlib_docs() {
    std::println("\n--- #2771 AC4: std/socket + adaptive help ---");
    const auto sock = read_file("lib/std/socket.aura");
    CHECK(!sock.empty(), "AC4: socket.aura");
    CHECK(sock.find("tcp-listen") != std::string::npos, "AC4: exports tcp-listen");
    CHECK(sock.find("tcp-accept") != std::string::npos, "AC4: exports tcp-accept");
    CHECK(sock.find("#2771") != std::string::npos, "AC4: cites #2771");
    CHECK(sock.find("127.0.0.1") != std::string::npos || sock.find("loopback") != std::string::npos,
          "AC4: documents loopback bind");
    const auto typ = read_file("lib/std/socket.aura-type");
    CHECK(typ.find("tcp-listen") != std::string::npos, "AC4: aura-type tcp-listen");
    CHECK(typ.find("tcp-accept") != std::string::npos, "AC4: aura-type tcp-accept");
    const auto adaptive = read_file("lib/std/adaptive.aura");
    CHECK(adaptive.find("tcp-listen") != std::string::npos, "AC4: adaptive help tcp-listen");
    CHECK(adaptive.find("#2771") != std::string::npos, "AC4: adaptive cites #2771");
    const auto io = read_file("src/compiler/evaluator_primitives_io.cpp");
    CHECK(io.find("tcp-listen") != std::string::npos, "AC4: prim tcp-listen");
    CHECK(io.find("#2771") != std::string::npos, "AC4: prim cites #2771");
}

static void ac5_linter() {
    std::println("\n--- #2771 AC5: linter + budget + no docs/design ---");
    const auto build = read_file("build.py");
    CHECK(build.find("check_tcp_listen_accept_2771") != std::string::npos,
          "AC5: build.py wires linter");
    const auto lint = read_file("scripts/coverage/checks/check_tcp_listen_accept_2771.py");
    CHECK(!lint.empty(), "AC5: linter present");
    const auto surface = read_file("scripts/coverage/checks/check_primitive_surface.py");
    CHECK(surface.find("tcp-\": 8") != std::string::npos, "AC5: commercial budget tcp- = 8");
    CHECK(read_file("docs/design/2771-tcp-listen-accept.md").empty(),
          "AC5: no docs/design/2771-* per #1655");
}

} // namespace

int run_test_tcp_listen_accept() {
    std::println("=== Issue #2771: tcp-listen / tcp-accept multi-host denseness ===");
    ac1_prims_registered();
    ac2_ac3_echo_fiber_client();
    ac3b_accept_timeout_idle();
    ac4_stdlib_docs();
    ac5_linter();
    std::println("\n=== #2771: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tcp_listen_accept();
}
#endif
