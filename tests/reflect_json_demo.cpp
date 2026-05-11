// C++26 Reflection (P2996) + JSON Serialization Demo
// Requires: GCC 16.1+ with -std=c++26 -freflection
// Check: g++ --version | grep "16\." && g++ -std=c++26 -freflection -dM -E - </dev/null | grep __cpp_impl_reflection
//
// Build: g++ -std=c++26 -freflection tests/reflect_json_demo.cpp -o build/reflect_demo
//
// The Aura --serve protocol uses lightweight hand-written JSON for now.
// When serialization needs grow (M3 IRModule reflection, M4 self-hosting),
// P2996 can auto-generate to_json() for any struct.

#include <meta>
#include <cstdio>
#include <string>
#include <format>
#include <vector>

// ── Feature detection ──────────────────────────────────────────
#ifndef __cpp_impl_reflection
#error "C++26 Reflection (P2996) not supported. Requires GCC 16.1+ with -std=c++26 -freflection"
#endif

// ── Structs for demo ───────────────────────────────────────────
struct Point { int x; int y; };

struct DiagnosticMsg {
    int kind;
    std::string msg;
    unsigned long node_id;
    bool recoverable;
};

// ── Compile-time member list ───────────────────────────────────
template <typename T>
consteval auto data_members_of() {
    constexpr auto members = std::meta::members_of(^T);
    std::vector<std::meta::info> result;
    for (auto m : members) {
        if (std::meta::is_data_member(m) && !std::meta::is_static_member(m))
            result.push_back(m);
    }
    return result;
}

template <typename T>
consteval std::string member_log() {
    auto members = data_members_of<T>();
    std::string out;
    for (auto m : members) {
        out += "  - " + std::string(std::meta::name_of(m))
             + " : " + std::string(std::meta::name_of(std::meta::type_of(m)))
             + "\n";
    }
    return out;
}

template <typename T>
consteval std::string json_schema() {
    auto members = data_members_of<T>();
    std::string s = "{\n";
    bool first = true;
    for (auto m : members) {
        if (!first) s += ",\n";
        s += "  \"" + std::string(std::meta::name_of(m)) + "\": <"
           + std::string(std::meta::name_of(std::meta::type_of(m))) + ">";
        first = false;
    }
    s += "\n}";
    return s;
}

// ── Runtime JSON (hand-written) ────────────────────────────────
// Current Aura approach: lightweight, zero deps for 4 message types.
// Future: expansion statements (P1306) auto-generate this.
std::string to_json(const DiagnosticMsg& d) {
    return std::format(
        R"({{"status":"error","kind":{},"msg":"{}","node_id":{},"recoverable":{}}})",
        d.kind, d.msg, d.node_id, d.recoverable);
}

// ── Demo ───────────────────────────────────────────────────────
int main() {
    std::printf("=== C++26 Reflection (P2996) + JSON Demo ===\n");
    std::printf("  __cpp_impl_reflection = %ld\n\n", (long)__cpp_impl_reflection);

    // 1. Compile-time member listing
    std::printf("[1] Point members:\n%s", member_log<Point>().c_str());
    std::printf("[2] DiagnosticMsg members:\n%s", member_log<DiagnosticMsg>().c_str());

    // 2. Compile-time JSON Schema
    std::printf("[3] Point JSON Schema (compile-time):\n%s\n", json_schema<Point>().c_str());
    std::printf("[4] DiagnosticMsg JSON Schema:\n%s\n", json_schema<DiagnosticMsg>().c_str());

    // 3. Runtime JSON
    std::printf("[5] Runtime DiagnosticMsg → JSON:\n");
    DiagnosticMsg d{3, "unbound variable: x", 42, true};
    std::printf("  %s\n", to_json(d).c_str());

    // 4. Future: expansion auto-serialize (P1306)
    std::printf("\n[6] Future: expansion auto-serialize (P1306)\n");
    std::printf("  template for (auto m : data_members_of<T>()) {\n");
    std::printf("    json += \"\\\"{}:\\\"{}, \"_fmt(name_of(m), obj.[:m:]);\n");
    std::printf("  }\n");

    std::printf("\n=== Demo complete ===\n");
    return 0;
}
