// offload/offload_job.cpp — thread-private compiler job bodies (#4887/#4888)
//
// Every job constructs its own CompilerService on the executing pool thread:
// the session CompilerService is never touched, so a parked fiber holding the
// session eval/body mutexes cannot deadlock the worker.
#include "offload_job.h"

#include "compiler/messaging_bridge.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace aura::offload {
namespace {

    using aura::compiler::CompilerService;
    namespace types = aura::compiler::types;

    // Issue #4887/#4888: this worker thread evaluates a private service; it must
    // not block on the session CLI body mutex the parked fiber holds.
    struct PrivateEvalScope {
        bool enabled = false;
        PrivateEvalScope() {
            aura::messaging::set_offload_private_eval(true);
            enabled = true;
        }
        ~PrivateEvalScope() {
            if (enabled)
                aura::messaging::set_offload_private_eval(false);
        }
        PrivateEvalScope(const PrivateEvalScope&) = delete;
        PrivateEvalScope& operator=(const PrivateEvalScope&) = delete;
    };

    std::string escape_lisp_string(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += c;
                    break;
            }
        }
        return out;
    }

    std::string value_to_string(CompilerService& cs, const types::EvalValue& v) {
        auto& ev = cs.evaluator();
        return aura::compiler::format_value(v, ev.primitives().string_heap(), ev.pairs(), 0,
                                            &ev.primitives(), ev.keyword_table(), ev.vector_heap(),
                                            ev.symbol_table());
    }

    std::string eval_result_string(CompilerService& cs, const types::EvalValue& v) {
        if (types::is_string(v)) {
            const auto idx = types::as_string_idx(v);
            return cs.evaluator().string_at(idx);
        }
        return value_to_string(cs, v);
    }

    bool load_snapshot(CompilerService& cs, const std::string& source, std::string& error) {
        if (source.empty())
            return true;
        std::string program = "(set-code \"";
        program += escape_lisp_string(source);
        program += "\")\n(eval-current)";
        auto loaded = cs.eval(program);
        if (!loaded) {
            error = loaded.error().format();
            return false;
        }
        return true;
    }

    void collect_diagnostics(const std::string& report, Result& out) {
        if (report.empty() || report.find("no errors") != std::string::npos)
            return;
        std::string line;
        for (char c : report) {
            if (c == '\n') {
                if (!line.empty())
                    out.diagnostics.push_back(Diagnostic{std::move(line)});
                line.clear();
            } else if (c != '\r') {
                line += c;
            }
        }
        if (!line.empty())
            out.diagnostics.push_back(Diagnostic{std::move(line)});
    }

    void parse_test_summary(const std::string& summary, Result& out) {
        unsigned long long suites = 0, passed = 0, failed = 0;
        if (std::sscanf(summary.c_str(), "%llu suites: %llu passed, %llu failed", &suites, &passed,
                        &failed) >= 3) {
            out.tests.passed = passed;
            out.tests.failed = failed;
        }
    }

} // namespace

Result run_stages(const SnapshotInput& input, const std::vector<std::string>& stages,
                  std::stop_token st) {
    const auto started = std::chrono::steady_clock::now();
    PrivateEvalScope private_eval;
    Result out;
    CompilerService cs;
    std::string error;
    if (!load_snapshot(cs, input.source, error)) {
        out.diagnostics.push_back(Diagnostic{"snapshot-load: " + error});
        return out;
    }

    for (const auto& stage : stages) {
        if (st.stop_requested())
            break;
        if (stage == "typecheck") {
            auto r = cs.eval("(typecheck-current)");
            if (!r) {
                out.diagnostics.push_back(Diagnostic{"typecheck: " + r.error().format()});
                break;
            }
            collect_diagnostics(eval_result_string(cs, *r), out);
        } else if (stage == "test") {
            auto r = cs.eval("(run-tests)");
            if (!r) {
                out.diagnostics.push_back(Diagnostic{"test: " + r.error().format()});
                break;
            }
            parse_test_summary(eval_result_string(cs, *r), out);
        } else if (stage == "compile") {
            // The private service is tree-walker ready after snapshot load;
            // compiled artifacts stay out of scope until the fiber explicitly
            // asks for hot-swap / aot:reload (#4888 keeps verify and commit
            // separate).
        } else {
            out.diagnostics.push_back(Diagnostic{"offload: unsupported stage: " + stage});
            break;
        }
    }

    out.timing.exec_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());
    return out;
}

std::string eval_code(const SnapshotInput& input, std::string_view code) {
    PrivateEvalScope private_eval;
    CompilerService cs;
    std::string error;
    if (!load_snapshot(cs, input.source, error))
        return "snapshot-load: " + error;
    auto r = cs.eval(code);
    if (!r)
        return r.error().format();
    return value_to_string(cs, *r);
}

} // namespace aura::offload
