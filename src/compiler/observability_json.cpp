// observability_json.cpp — JSON serializer for CompilerSnapshot
// (Issue #62 Iter 3). Hand-rolled JSON to avoid the reflect
// framework's consteval/std::meta dependency, which requires
// full P2996 reflection support in GCC. The schema is small
// and stable, so the trade-off (manual vs reflect) is
// acceptable for now.

#include <string>
#include <cstdio>
#include <format>

#include "compiler/observability_snapshot.h"

namespace aura::compiler {

namespace {

void append_q(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
}

}  // namespace

std::string fn_metrics_to_json(const FnMetrics& f) {
    std::string out = "{";
    out += "\"name\":"; append_q(out, f.name); out += ",";
    std::format_to(std::back_inserter(out),
                   "\"total_calls\":{},", f.total_calls);
    std::format_to(std::back_inserter(out),
                   "\"deopt_count\":{},", f.deopt_count);
    std::format_to(std::back_inserter(out),
                   "\"hit_count\":{},", f.hit_count);
    std::format_to(std::back_inserter(out),
                   "\"miss_count\":{},", f.miss_count);
    std::format_to(std::back_inserter(out),
                   "\"hit_rate\":{},", f.hit_rate);
    out += "\"has_shape_map\":";
    out += f.has_shape_map ? "true" : "false";
    out += ",";
    std::format_to(std::back_inserter(out),
                   "\"specialized_for\":{}", f.specialized_for);
    out += "}";
    return out;
}

std::string snapshot_to_json(const CompilerSnapshot& s) {
    std::string out = "{";
    std::format_to(std::back_inserter(out),
                   "\"deopt_count\":{},", s.deopt_count);
    std::format_to(std::back_inserter(out),
                   "\"specialization_hits\":{},", s.specialization_hits);
    std::format_to(std::back_inserter(out),
                   "\"specialization_misses\":{},", s.specialization_misses);
    std::format_to(std::back_inserter(out),
                   "\"shape_changes_observed\":{},", s.shape_changes_observed);
    std::format_to(std::back_inserter(out),
                   "\"jit_compilations\":{},", s.jit_compilations);
    std::format_to(std::back_inserter(out),
                   "\"jit_compile_misses\":{},", s.jit_compile_misses);
    std::format_to(std::back_inserter(out),
                   "\"jit_cache_evictions\":{},", s.jit_cache_evictions);
    std::format_to(std::back_inserter(out),
                   "\"aot_emits\":{},", s.aot_emits);
    std::format_to(std::back_inserter(out),
                   "\"aot_fallbacks\":{},", s.aot_fallbacks);
    std::format_to(std::back_inserter(out),
                   "\"arena_bytes_used\":{},", s.arena_bytes_used);
    std::format_to(std::back_inserter(out),
                   "\"arena_bytes_peak\":{},", s.arena_bytes_peak);
    out += "\"functions\":[";
    for (std::size_t i = 0; i < s.functions.size(); ++i) {
        if (i > 0) out += ",";
        out += fn_metrics_to_json(s.functions[i]);
    }
    out += "]}";
    return out;
}

} // namespace aura::compiler
