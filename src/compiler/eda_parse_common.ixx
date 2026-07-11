// eda_parse_common.ixx — Issue #1228 Phase 1: shared SV/netlist parse helpers.

module;

export module aura.compiler.eda_parse_common;

import std;

export namespace aura::compiler::eda_parse {

inline constexpr int kEdaParseCommonPhase = 1;

struct ParseStats {
    std::uint64_t lines_seen = 0;
    std::uint64_t tokens = 0;
    std::uint64_t modules = 0;
};

// Phase 1: shared line splitter / comment strip used by eda:parse-netlist paths.
inline void strip_line_comment(std::string& line) {
    auto pos = line.find("//");
    if (pos != std::string::npos)
        line.resize(pos);
    // Verilog block comment start — leave remainder for multi-line peel
    auto b = line.find("/*");
    if (b != std::string::npos) {
        auto e = line.find("*/", b + 2);
        if (e != std::string::npos)
            line.erase(b, e + 2 - b);
        else
            line.resize(b);
    }
}

inline std::vector<std::string_view> split_tokens(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i >= line.size())
            break;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t')
            ++j;
        out.emplace_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

inline ParseStats g_eda_parse_stats{};

} // namespace aura::compiler::eda_parse
