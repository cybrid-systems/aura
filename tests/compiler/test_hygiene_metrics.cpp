// @category: unit
// @reason: Issue #4936 — constexpr hygiene names and scheduler metrics print.
//
//   AC1: the reserved-name table is sorted, has no duplicates, and matches
//        the historical membership (if / car / monotonic-ms present, foo absent)
//   AC2: WorkerMetrics::dump and GlobalMetrics::dump write the counters

#include "compiler/hygiene_builtins.hh"
#include "serve/metrics.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

int g_failed = 0;

void expect(bool ok, const char* label) {
    if (!ok) {
        std::println(std::cerr, "FAIL {}", label);
        ++g_failed;
    } else {
        std::println("PASS {}", label);
    }
}

std::string read_tmp(std::FILE* file) {
    std::fflush(file);
    std::rewind(file);
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), file) != nullptr)
        out += buf;
    return out;
}

} // namespace

int main() {
    using aura::compiler::macro_exp::detail::hygiene_builtins_contains;
    using aura::compiler::macro_exp::detail::kHygieneBuiltinNames;
    using aura::compiler::macro_exp::detail::kHygieneBuiltins;
    expect(kHygieneBuiltinNames.size() == 108, "108 reserved names");
    expect(kHygieneBuiltins.size() == kHygieneBuiltinNames.size(), "sorted table same length");
    bool sorted = true;
    bool unique = true;
    for (std::size_t i = 1; i < kHygieneBuiltins.size(); ++i) {
        if (kHygieneBuiltins[i - 1] > kHygieneBuiltins[i])
            sorted = false;
        if (kHygieneBuiltins[i - 1] == kHygieneBuiltins[i])
            unique = false;
    }
    expect(sorted && unique, "sorted and unique");
    expect(hygiene_builtins_contains("if") && hygiene_builtins_contains("car") &&
               hygiene_builtins_contains("monotonic-ms") && hygiene_builtins_contains("+"),
           "historical names present");
    expect(!hygiene_builtins_contains("foo") && !hygiene_builtins_contains(""),
           "unknown names absent");

    aura::serve::metrics::WorkerMetrics worker;
    worker.fibers_executed.store(3, std::memory_order_relaxed);
    worker.steal_attempts.store(4, std::memory_order_relaxed);
    worker.steal_successes.store(1, std::memory_order_relaxed);
    worker.busy_ns.store(2'000'000, std::memory_order_relaxed);
    std::FILE* worker_out = std::tmpfile();
    expect(worker_out != nullptr, "worker tmpfile");
    worker.dump(2, worker_out);
    const auto worker_text = read_tmp(worker_out);
    std::fclose(worker_out);
    expect(worker_text.find("[W-2]") != std::string::npos &&
               worker_text.find("fibers=3") != std::string::npos &&
               worker_text.find("steals=1/4") != std::string::npos &&
               worker_text.find("25.0%") != std::string::npos,
           "worker dump text");

    aura::serve::metrics::GlobalMetrics global(1);
    global.fibers_spawned.store(5, std::memory_order_relaxed);
    global.fibers_completed.store(4, std::memory_order_relaxed);
    global.worker(0).steal_attempts.store(8, std::memory_order_relaxed);
    global.worker(0).steal_successes.store(2, std::memory_order_relaxed);
    std::FILE* global_out = std::tmpfile();
    expect(global_out != nullptr, "global tmpfile");
    global.dump(global_out);
    const auto global_text = read_tmp(global_out);
    std::fclose(global_out);
    expect(global_text.find("fibers: 5 spawned, 4 completed") != std::string::npos &&
               global_text.find("steal:  2/8 (25.0% success)") != std::string::npos &&
               global_text.find("[W-0]") != std::string::npos,
           "global dump text");

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
