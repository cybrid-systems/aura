// Upper bound for "does the workspace have any match site" scans.
// FlatAST::has_match_info(id) is false for every id >= match_info_extent(),
// so the scan may stop there instead of walking every node (dead slots
// included), which grew with the rebind count.
#pragma once
#include <algorithm>
#include <cstdlib>

namespace aura::compiler {
template <class Flat> inline auto match_scan_bound(const Flat& flat) {
    const auto n = flat.size();
    return std::min<decltype(n)>(n, static_cast<decltype(n)>(flat.match_info_extent()));
}
} // namespace aura::compiler
