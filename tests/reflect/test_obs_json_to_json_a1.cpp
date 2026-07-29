// Wave A1: snapshot_to_json / fn_metrics_to_json via reflect to_json.

#include <cstdio>
#include <string>

#include "compiler/observability_logger.h"
#include "compiler/observability_snapshot.h"

int main() {
    int failed = 0;
    auto check = [&](bool c, const char* m) {
        if (!c) {
            ++failed;
            std::printf("FAIL: %s\n", m);
        }
    };

    aura::compiler::FnMetrics f;
    f.name = "main";
    f.total_calls = 10;
    f.deopt_count = 1;
    f.hit_count = 7;
    f.miss_count = 3;
    f.hit_rate = 0.7;
    f.has_shape_map = true;
    f.specialized_for = 2;
    const std::string fj = aura::compiler::fn_metrics_to_json(f);
    check(fj.find("\"name\":\"main\"") != std::string::npos, "fn name");
    check(fj.find("\"total_calls\":10") != std::string::npos, "fn calls");
    check(fj.find("\"has_shape_map\":true") != std::string::npos, "fn shape");
    check(fj.find("\"specialized_for\":2") != std::string::npos, "fn spec");

    aura::compiler::CompilerSnapshot s;
    s.deopt_count = 4;
    s.specialization_hits = 9;
    s.arena_bytes_peak = 1024;
    s.marker_user_count = 3;
    s.functions.push_back(f);
    const std::string sj = aura::compiler::snapshot_to_json(s);
    check(sj.find("\"deopt_count\":4") != std::string::npos, "snap deopt");
    check(sj.find("\"specialization_hits\":9") != std::string::npos, "snap hits");
    check(sj.find("\"arena_bytes_peak\":1024") != std::string::npos, "snap arena");
    check(sj.find("\"marker_user_count\":3") != std::string::npos, "snap marker (full POD)");
    check(sj.find("\"functions\"") != std::string::npos, "snap functions key");
    check(sj.find("\"name\":\"main\"") != std::string::npos, "snap nested fn");

    std::printf("test_obs_json_to_json_a1: %s (failed=%d)\n", failed ? "FAIL" : "PASS", failed);
    return failed == 0 ? 0 : 1;
}
