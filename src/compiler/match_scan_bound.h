// Upper bound for "does the workspace have any match site" scans.
// FlatAST::has_match_info(id) is false for every id >= match_info_extent(),
// so the scan may stop there instead of walking every node (dead slots
// included), which grew with the rebind count. AURA_MATCH_SCAN_EXTENT=0
// restores the full-size walk.
#pragma once
#include <algorithm>
#include <cstdlib>

namespace aura::compiler {
template <class Flat> inline auto match_scan_bound(const Flat& flat) {
    static const bool extent_on = [] {
        const char* e = std::getenv("AURA_MATCH_SCAN_EXTENT");
        return !(e && e[0] == '0');
    }();
    const auto n = flat.size();
    if (!extent_on)
        return n;
    return std::min<decltype(n)>(n, static_cast<decltype(n)>(flat.match_info_extent()));
}
} // namespace aura::compiler
