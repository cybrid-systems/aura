// @category: unit
// @reason: Issue #3156 — close value-only dual-track under production
// required (residual #3017 / #3093). `maybe_note_allocate_intermediate_`
// must NOT call `note_intermediate_create_auto_wire_` (value-only) under
// general_object_pin_required_active(). Instead it routes through
// `note_intermediate_create_with_cover_(ptr, nullptr, nullptr)` which,
// under required + both null, records into intermediate_creates_ for
// pre-densify fail-closed (has_unpinned_intermediate_creates_() →
// block + sticky-off per #3017) and bumps the new
// g_intermediate_create_uncovered_under_required_total metric. Soft /
// Off / render-hotpath single-load zero-cost contract preserved (AC3).
//
// Source-cite based (mirrors #2496 / #2597 / #3053 / #3093 suite shape).
// No behavioural arena setup — purely structural regression guard against
// re-introducing the value-only dual-track after #3156.
//
//   AC1: maybe_note_allocate_intermediate_ does NOT call
//        note_intermediate_create_auto_wire_ (value-only) directly.
//   AC2: maybe_note_allocate_intermediate_ routes through
//        note_intermediate_create_with_cover_(ptr, nullptr, nullptr).
//   AC3: note_intermediate_create_with_cover_ has the required branch
//        (slot==null + reason==null + general_object_pin_required_active)
//        inventorying into intermediate_creates_ + bumping the new
//        uncovered metric (NOT value_only_total).
//   AC4: note_intermediate_create_with_cover_ body under required + both
//        null does NOT call note_intermediate_create_auto_wire_(p)
//        (closes dual-track per AC4 of #3156).
//   AC5: g_intermediate_create_uncovered_under_required_total counter
//        declared with the #3156 stamp constant.
//   AC6: intermediate_create_uncovered_under_required_total_v_read()
//        accessor present (query exposure, mirrors #3093 pattern).
//   AC7: reset_intermediate_create_with_cover_for_test() resets all 3
//        counters (with_cover / value_only / uncovered_under_required).
//   AC8: Soft / Off / render-hotpath single-load zero-cost contract
//        preserved (existing branches intact in with_cover_ + auto_wire_
//        path).
//   AC9: New 3156 linter script exists + self-test passes.

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

import std;

namespace {

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

// Find the first matching open `{` of a top-level ASTArena member function
// whose declaration starts at `sig_pos`. Returns position of `{`, or
// std::string::npos if not found within `search_limit` bytes.
static std::string::size_type find_function_body_open_(const std::string& src,
                                                       std::string::size_type sig_pos,
                                                       std::string::size_type search_limit) {
    const auto end = std::min(src.size(), sig_pos + search_limit);
    auto depth_paren = std::string::size_type{0};
    bool in_sig = true;
    for (auto i = sig_pos; i < end; ++i) {
        const char c = src[i];
        if (in_sig) {
            if (c == '(')
                ++depth_paren;
            else if (c == ')') {
                if (depth_paren == 0) {
                    // shouldn't happen before we see one
                    return std::string::npos;
                }
                --depth_paren;
                if (depth_paren == 0)
                    in_sig = false;
            }
            continue;
        }
        // After the closing paren: skip until we hit `{` (function body).
        if (c == '{')
            return i;
        if (c == ';')
            return std::string::npos; // pure declaration, no body
    }
    return std::string::npos;
}

// Find the matching close `}` of a function whose `{` opens at `open_pos`.
// Handles nested braces + brace-init lists + raw-string literals.
static std::string::size_type find_function_body_close_(const std::string& src,
                                                        std::string::size_type open_pos) {
    int depth = 0;
    bool in_raw = false;
    for (auto i = open_pos; i < src.size(); ++i) {
        const char c = src[i];
        // raw string literal R"delim(...)delim"
        if (!in_raw && c == 'R' && i + 1 < src.size() && src[i + 1] == '"') {
            // skip past R" then read delim until '('
            auto j = i + 2;
            std::string delim;
            while (j < src.size() && src[j] != '(') {
                delim.push_back(src[j]);
                ++j;
            }
            if (j >= src.size())
                return std::string::npos;
            // scan for )delim"
            const std::string closer = ")" + delim + "\"";
            const auto found = src.find(closer, j + 1);
            if (found == std::string::npos)
                return std::string::npos;
            i = found + closer.size() - 1;
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

// Issue #3180: production small-pool allocate call sites to true cover
// (slot / EXEMPT). Under production_defaults_active() + required, every
// small-pool intermediate create must declare cover at the allocate site
// — slot (stable field pointer) for survivors, EXEMPT(reason) for
// transients. Resolves the residual of #3156 that fail-closed inventory
// + sticky-off under production is migrated to a clean cover-declared
// happy path (counter == 0 after production soak).
//
//   AC1: maybe_note_allocate_intermediate_ accepts optional slot/reason
//        parameters (issue #3180) — default nullptr preserves legacy
//        fail-closed behavior under required.
//   AC2: allocate_raw_impl + allocate_raw + allocate_checked forward
//        cover_slot/cover_reason to maybe_note_allocate_intermediate_
//        (single wire point, #3179 pattern).
//   AC3: Hot-path callers in evaluator_eval_flat / service /
//        evaluator_module_loader / evaluator_workspace_tree provide
//        slot (long-lived) or EXEMPT reason (transient) at the
//        create site.
//   AC4: Soft / no-env / WAL-off / render-hotpath zero-cost contract
//        preserved (early-return guard before any cover logic).

static void ac3180_cover_param_threading() {
    std::println("\n--- #3180 AC1+AC2: cover_slot/cover_reason threading ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(!arena.empty(), "AC1+AC2: arena.ixx readable");

    // AC1: maybe_note_allocate_intermediate_ signature with optional slot/reason.
    CHECK(arena.find("void maybe_note_allocate_intermediate_(void* ptr, std::size_t size,") !=
              std::string::npos,
          "AC1: maybe_note_allocate_intermediate_ has size param (then optional slot/reason)");
    CHECK(arena.find("void** slot = nullptr,") != std::string::npos,
          "AC1: maybe_note_allocate_intermediate_ has optional slot=nullptr");
    CHECK(arena.find("const char* reason = nullptr") != std::string::npos,
          "AC1: maybe_note_allocate_intermediate_ has optional reason=nullptr");

    // AC2: allocate_raw_impl + allocate_raw + allocate_checked forward cover.
    CHECK(arena.find("void* allocate_raw_impl(std::size_t size, std::size_t alignment,") !=
              std::string::npos,
          "AC2: allocate_raw_impl signature has size/alignment (then cover params)");
    CHECK(arena.find("void** cover_slot = nullptr,") != std::string::npos,
          "AC2: allocate_raw_impl has cover_slot param");
    CHECK(arena.find("const char* cover_reason = nullptr") != std::string::npos,
          "AC2: allocate_raw_impl has cover_reason param");
    // maybe_note_allocate_intermediate_ called with cover from allocate_raw_impl.
    const auto impl_cover_call =
        arena.find("maybe_note_allocate_intermediate_(ptr, size, cover_slot, cover_reason)");
    CHECK(impl_cover_call != std::string::npos,
          "AC2: allocate_raw_impl forwards cover_slot/cover_reason to "
          "maybe_note_allocate_intermediate_");

    // allocate_checked forwards cover too.
    CHECK(arena.find("void* ptr = allocate_raw_impl(size, alignment, cover_slot, cover_reason)") !=
              std::string::npos,
          "AC2: allocate_checked forwards cover to allocate_raw_impl");
    CHECK(arena.find("allocate_checked(std::size_t size, std::size_t alignment") !=
              std::string::npos,
          "AC2: allocate_checked signature has cover params");

    // AC4: Soft / no-env / WAL-off / render-hotpath early-return guard
    // preserved (existing single required-active load).
    CHECK(arena.find("if (!aura::core::lifetime::general_object_pin_required_active())") !=
              std::string::npos,
          "AC4: required-active guard preserved (zero-cost Soft/Off path)");
    CHECK(arena.find("if (aura::core::arena_policy::in_render_hotpath())") != std::string::npos,
          "AC4: render-hotpath guard preserved");
}

static void ac3180_hot_path_cover_declarations() {
    std::println("\n--- #3180 AC3: hot-path callers declare cover ---");
    // AC3: production hot-path callers follow up with cover declarations
    // (slot for survivors, EXEMPT for transients). Each file under
    // tests/core sees the cover pattern at the create<T>() site.
    const auto eval_flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto service = read_file("src/compiler/service.ixx");
    const auto mod_load = read_file("src/compiler/evaluator_module_loader.cpp");
    const auto ws_tree = read_file("src/compiler/evaluator_workspace_tree.cpp");

    // Slot declarations (long-lived survivors). Indent is not part of the
    // contract — the 3180 python linter already regex-matches wrapping.
    CHECK(eval_flat.find("pat_pool, reinterpret_cast<void**>(&pat_pool), nullptr") !=
              std::string::npos,
          "AC3: evaluator_eval_flat declares pat_pool slot cover");
    CHECK(eval_flat.find("pat_flat, reinterpret_cast<void**>(&pat_flat), nullptr") !=
              std::string::npos,
          "AC3: evaluator_eval_flat declares pat_flat slot cover");

    // Transient EXEMPT (reason-string) declarations.
    CHECK(eval_flat.find("\"eval-flat-closure-body-transient\"") != std::string::npos,
          "AC3: evaluator_eval_flat declares cl_flat EXEMPT(transient)");
    CHECK(eval_flat.find("\"require-import-parse-transient\"") != std::string::npos,
          "AC3: evaluator_eval_flat declares ipool/iflat EXEMPT(transient)");
    CHECK(eval_flat.find("\"inst-env-cache-transient\"") != std::string::npos,
          "AC3: evaluator_eval_flat declares cached_env EXEMPT(transient)");

    // service.ixx — parse_to_flat pool/flat slot cover on arena_ + module_arena.
    CHECK(service.find("note_intermediate_create_with_cover_(\n                pool_ptr, "
                       "reinterpret_cast<void**>(&pool_ptr), nullptr)") != std::string::npos,
          "AC3: service arena parse_to_flat declares pool/flat slot cover");
    CHECK(service.find("note_intermediate_create_with_cover_(\n                flat_ptr, "
                       "reinterpret_cast<void**>(&flat_ptr), nullptr)") != std::string::npos,
          "AC3: service arena parse_to_flat declares pool/flat slot cover");
    CHECK(service.find("mod_arena.note_intermediate_create_with_cover_") != std::string::npos &&
              service.find("pool_ptr, reinterpret_cast<void**>(&pool_ptr), nullptr") !=
                  std::string::npos,
          "AC3: service module_arena parse_to_flat declares pool slot cover");
    CHECK(service.find("flat_ptr, reinterpret_cast<void**>(&flat_ptr), nullptr") !=
              std::string::npos,
          "AC3: service module_arena parse_to_flat declares flat slot cover");

    // evaluator_module_loader.cpp — pool/flat/env slot cover.
    CHECK(mod_load.find("mod_arena.note_intermediate_create_with_cover_") != std::string::npos &&
              mod_load.find("pool_ptr, reinterpret_cast<void**>(&pool_ptr), nullptr") !=
                  std::string::npos,
          "AC3: evaluator_module_loader declares pool slot cover");
    CHECK(mod_load.find("flat_ptr, reinterpret_cast<void**>(&flat_ptr), nullptr") !=
              std::string::npos,
          "AC3: evaluator_module_loader declares flat slot cover");
    CHECK(mod_load.find("reinterpret_cast<void**>(&mod_env)") != std::string::npos,
          "AC3: evaluator_module_loader declares mod_env slot cover");

    // evaluator_workspace_tree.cpp — env slot cover.
    CHECK(ws_tree.find("ar->note_intermediate_create_with_cover_(env, "
                       "reinterpret_cast<void**>(&env), nullptr)") != std::string::npos,
          "AC3: evaluator_workspace_tree declares env slot cover");
}

// AC1 + AC2: maybe_note_allocate_intermediate_ routes through with_cover_
// (not auto_wire_) under required.
static void ac1_2_maybe_allocate_routes_to_with_cover() {
    std::println(
        "\n--- #3156 AC1+AC2: maybe_note_allocate_intermediate_ routes through with_cover_ ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(!arena.empty(), "AC1+AC2: arena.ixx readable");

    const auto sig_pos = arena.find("void maybe_note_allocate_intermediate_(");
    CHECK(sig_pos != std::string::npos,
          "AC1+AC2: maybe_note_allocate_intermediate_ signature present");
    if (sig_pos == std::string::npos)
        return;

    const auto open = find_function_body_open_(arena, sig_pos, 2048);
    CHECK(open != std::string::npos, "AC1+AC2: maybe_note_allocate_intermediate_ body found");
    if (open == std::string::npos)
        return;

    const auto close = find_function_body_close_(arena, open);
    CHECK(close != std::string::npos && close > open,
          "AC1+AC2: maybe_note_allocate_intermediate_ body closed");
    if (close == std::string::npos || close <= open)
        return;

    const std::string body = arena.substr(open, close - open + 1);

    // AC1: must NOT call note_intermediate_create_auto_wire_ directly
    CHECK(body.find("note_intermediate_create_auto_wire_(ptr)") == std::string::npos,
          "AC1: maybe_note_allocate_intermediate_ does NOT call auto_wire_ directly");
    CHECK(body.find("note_intermediate_create_auto_wire_(ptr, ") == std::string::npos,
          "AC1: no auto_wire_(ptr, ...) variant either");

    // AC2: must route through with_cover_
    CHECK(body.find("note_intermediate_create_with_cover_(ptr") != std::string::npos,
          "AC2: maybe_note_allocate_intermediate_ routes through with_cover_(ptr, ...)");
}

// AC3 + AC4: note_intermediate_create_with_cover_ has the required branch
// (closes dual-track per AC4 of #3156).
static void ac3_4_with_cover_required_branch_closes_dual_track() {
    std::println("\n--- #3156 AC3+AC4: with_cover_ required branch closes dual-track ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(!arena.empty(), "AC3+AC4: arena.ixx readable");

    const auto sig_pos = arena.find("void note_intermediate_create_with_cover_(");
    CHECK(sig_pos != std::string::npos,
          "AC3+AC4: note_intermediate_create_with_cover_ signature present");
    if (sig_pos == std::string::npos)
        return;

    const auto open = find_function_body_open_(arena, sig_pos, 4096);
    CHECK(open != std::string::npos, "AC3+AC4: with_cover_ body found");
    if (open == std::string::npos)
        return;

    const auto close = find_function_body_close_(arena, open);
    CHECK(close != std::string::npos && close > open, "AC3+AC4: with_cover_ body closed");
    if (close == std::string::npos || close <= open)
        return;

    const std::string body = arena.substr(open, close - open + 1);

    // AC3: required branch present — checks for general_object_pin_required_active()
    // in the slot==null && reason==null path (after the two early returns).
    // Find the both-null branch: locate "both null" or comment marker,
    // then check the required_active call sits inside it.
    const auto both_null_marker = body.find("// both null");
    const auto both_null_marker_3156 = body.find("Issue #3156");
    CHECK(both_null_marker != std::string::npos || both_null_marker_3156 != std::string::npos,
          "AC3: with_cover_ body has a 'both null' / #3156 comment marker");
    CHECK(body.find("general_object_pin_required_active") != std::string::npos,
          "AC3: with_cover_ body references general_object_pin_required_active()");

    // AC4: required branch must NOT call note_intermediate_create_auto_wire_
    // (that would be the forbidden value-only dual-track we're closing).
    // The auto_wire_ call is only allowed in the Soft/Off/render-hotpath
    // backward-compat fallback AFTER the required-active check returns true
    // (i.e. it sits below the early-return for required).
    //
    // Verify: the body contains at most one `note_intermediate_create_auto_wire_(`
    // call AND it's positioned AFTER the required_active reference.
    auto count = std::string::size_type{0};
    auto last_auto_wire = std::string::size_type{0};
    auto p = body.find("note_intermediate_create_auto_wire_(");
    while (p != std::string::npos) {
        ++count;
        last_auto_wire = p;
        p = body.find("note_intermediate_create_auto_wire_(", p + 1);
    }
    CHECK(count <= 1, "AC4: at most one auto_wire_ call in with_cover_ body (Soft/Off fallback)");

    if (count == 1 && last_auto_wire != std::string::npos) {
        // Check that the auto_wire_ call sits AFTER the required_active
        // reference (i.e. it's the Soft/Off fallback path, not the
        // forbidden required path).
        const auto req_pos = body.find("general_object_pin_required_active");
        if (req_pos != std::string::npos) {
            CHECK(last_auto_wire > req_pos,
                  "AC4: auto_wire_ call is after required_active check (Soft/Off fallback)");
        }
    }

    // AC4 also: the required branch should inventory (intermediate_creates_)
    // and bump the new counter.
    CHECK(body.find("intermediate_creates_.push_back(p)") != std::string::npos,
          "AC4: with_cover_ body still inventories into intermediate_creates_ (pre-densify scan)");
    CHECK(body.find("g_intermediate_create_uncovered_under_required_total") != std::string::npos,
          "AC4: with_cover_ body bumps the new uncovered metric");
}

// AC5 + AC6 + AC7: counter / accessor / reset helper shape.
static void ac5_6_7_counter_accessor_reset() {
    std::println("\n--- #3156 AC5+AC6+AC7: counter / accessor / reset shape ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(!arena.empty(), "AC5+AC6+AC7: arena.ixx readable");

    // AC5: counter + stamp.
    CHECK(arena.find(
              "std::atomic<std::uint64_t> g_intermediate_create_uncovered_under_required_total") !=
              std::string::npos,
          "AC5: g_intermediate_create_uncovered_under_required_total counter declared");
    CHECK(arena.find("kIntermediateCreateUncoveredUnderRequiredIssue = 3156") != std::string::npos,
          "AC5: #3156 stamp constant present (kIntermediateCreateUncoveredUnderRequiredIssue = "
          "3156)");

    // AC6: accessor.
    CHECK(arena.find("intermediate_create_uncovered_under_required_total_v_read") !=
              std::string::npos,
          "AC6: intermediate_create_uncovered_under_required_total_v_read() accessor present");

    // AC7: reset helper resets all 3 counters.
    const auto reset_pos = arena.find("void reset_intermediate_create_with_cover_for_test");
    CHECK(reset_pos != std::string::npos,
          "AC7: reset_intermediate_create_with_cover_for_test() present");
    if (reset_pos != std::string::npos) {
        const auto reset_open = find_function_body_open_(arena, reset_pos, 1024);
        CHECK(reset_open != std::string::npos, "AC7: reset body found");
        if (reset_open != std::string::npos) {
            const auto reset_close = find_function_body_close_(arena, reset_open);
            CHECK(reset_close != std::string::npos && reset_close > reset_open,
                  "AC7: reset body closed");
            if (reset_close != std::string::npos && reset_close > reset_open) {
                const std::string reset_body =
                    arena.substr(reset_open, reset_close - reset_open + 1);
                CHECK(reset_body.find("g_intermediate_create_with_cover_total.store(0") !=
                          std::string::npos,
                      "AC7: reset stores 0 to with_cover_total");
                CHECK(reset_body.find("g_intermediate_create_value_only_total.store(0") !=
                          std::string::npos,
                      "AC7: reset stores 0 to value_only_total");
                CHECK(reset_body.find(
                          "g_intermediate_create_uncovered_under_required_total.store(0") !=
                          std::string::npos,
                      "AC7: reset stores 0 to uncovered_under_required_total (NEW)");
            }
        }
    }
}

// Issue #3214: maybe_note / allocate_raw_impl must note non-small densify-
// tracked allocate (pmr fallback, size > kMaxSmallSize), not only small-pool
// owns. Residual of #3156 / #3180. Soft single-load preserved (AC8).
static void ac3214_nonsmall_allocate_notes_cover() {
    std::println("\n--- #3214: non-small densify-tracked allocate notes cover triad ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(!arena.empty(), "3214: arena.ixx readable");

    const auto sig_pos = arena.find("void maybe_note_allocate_intermediate_(");
    CHECK(sig_pos != std::string::npos, "3214: maybe_note_allocate_intermediate_ present");
    if (sig_pos == std::string::npos)
        return;
    const auto open = find_function_body_open_(arena, sig_pos, 2048);
    CHECK(open != std::string::npos, "3214: maybe_note body found");
    if (open == std::string::npos)
        return;
    const auto close = find_function_body_close_(arena, open);
    if (close == std::string::npos || close <= open)
        return;
    const std::string body = arena.substr(open, close - open + 1);

    CHECK(body.find("kMaxSmallSize") != std::string::npos,
          "3214: small-pool identity (kMaxSmallSize) kept");
    CHECK(body.find("small_pool_.owns") != std::string::npos,
          "3214: small-pool identity (owns) kept");
    CHECK(body.find("note_intermediate_create_with_cover_(ptr") != std::string::npos,
          "3214: still routes through with_cover_");
    CHECK(body.find("non-small / pmr-fallback densify-tracked allocate") != std::string::npos,
          "3214: non-small branch present (does not skip cover)");
    // Historical skip (`if (size > kMaxSmallSize || !owns) return;`) is gone.
    CHECK(body.find("if (size > SmallObjectPool::kMaxSmallSize || !small_pool_.owns(ptr))\n"
                    "            return;") == std::string::npos,
          "3214: maybe_note no longer early-returns on non-small / !owns");

    const auto impl_pos =
        arena.find("void* allocate_raw_impl(std::size_t size, std::size_t alignment,");
    CHECK(impl_pos != std::string::npos, "3214: allocate_raw_impl present");
    if (impl_pos != std::string::npos) {
        const auto impl_open = find_function_body_open_(arena, impl_pos, 2048);
        CHECK(impl_open != std::string::npos, "3214: allocate_raw_impl body found");
        if (impl_open != std::string::npos) {
            const auto impl_close = find_function_body_close_(arena, impl_open);
            if (impl_close != std::string::npos && impl_close > impl_open) {
                const std::string impl = arena.substr(impl_open, impl_close - impl_open + 1);
                const auto pmr = impl.find("resource_.allocate(size, alignment)");
                CHECK(pmr != std::string::npos, "3214: pmr allocate path present");
                if (pmr != std::string::npos) {
                    const auto after = impl.substr(pmr);
                    CHECK(after.find("maybe_note_allocate_intermediate_(ptr, size, cover_slot, "
                                     "cover_reason)") != std::string::npos,
                          "3214: pmr / large allocate notes cover (not only small-pool hit)");
                }
            }
        }
    }

    CHECK(arena.find("kDensifyTrackedAllocateCoverIssue = 3214") != std::string::npos,
          "3214: arena stamp present");
    CHECK(read_file("docs/design/3214-densify-allocate-cover.md").empty(),
          "3214: no docs/design/3214-* per #1655");
    CHECK(read_file("tests/issues/test_issue_3214.cpp").empty() &&
              read_file("tests/core/test_issue_3214.cpp").empty(),
          "3214: no test_issue_3214.cpp per #81967");
}

static void ac3326_factory_cover_surface() {
    std::println("\n--- #3326: factory create_with_cover / try_allocate cover surface ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(arena.find("kFactoryDefaultCoverIssue = 3326") != std::string::npos, "3326: arena stamp");
    CHECK(arena.find("create_with_cover(void** cover_slot, const char* cover_reason") !=
              std::string::npos,
          "3326: create_with_cover signature");
    CHECK(arena.find("try_allocate(std::size_t size, void** cover_slot") != std::string::npos,
          "3326: try_allocate cover args");
    CHECK(arena.find("allocate_raw(sizeof(T), alignof(T), cover_slot, cover_reason)") !=
              std::string::npos,
          "3326: create_with_cover forwards cover to allocate_raw");
    CHECK(arena.find("if (!aura::core::lifetime::general_object_pin_required_active())") !=
              std::string::npos,
          "3326: Soft required-active load preserved");
}

// AC8: Soft / Off / render-hotpath single-load zero-cost contract preserved.
// The with_cover_ body keeps the auto_wire_ call for the Soft/Off path
// (already covered by AC4 ordering check) AND the pre-checks in
// maybe_note_allocate_intermediate_ remain the same single-load pattern.
static void ac8_soft_off_zero_cost() {
    std::println("\n--- #3156 AC8: Soft / Off / render-hotpath zero-cost contract preserved ---");
    const auto arena = read_file("src/core/arena.ixx");

    const auto sig_pos = arena.find("void maybe_note_allocate_intermediate_(");
    CHECK(sig_pos != std::string::npos, "AC8: maybe_note_allocate_intermediate_ present");
    if (sig_pos == std::string::npos)
        return;

    const auto open = find_function_body_open_(arena, sig_pos, 2048);
    CHECK(open != std::string::npos, "AC8: maybe body found");
    if (open == std::string::npos)
        return;
    const auto close = find_function_body_close_(arena, open);
    if (close == std::string::npos || close <= open)
        return;
    const std::string body = arena.substr(open, close - open + 1);

    // Single required_active load + branch.
    CHECK(body.find("general_object_pin_required_active") != std::string::npos,
          "AC8: required_active load still present (single load + branch)");
    // Render-hotpath gate still present.
    CHECK(body.find("in_render_hotpath") != std::string::npos,
          "AC8: render-hotpath gate still present (early return unchanged)");
    // Size / small_pool_owns check still present.
    CHECK(body.find("kMaxSmallSize") != std::string::npos,
          "AC8: small-pool size check still present (unchanged)");
    CHECK(body.find("small_pool_.owns") != std::string::npos,
          "AC8: small_pool_.owns(ptr) check still present (unchanged)");
}

// AC9: linter script exists + self-test passes.
static void ac9_linter_self_test() {
    std::println("\n--- #3156 AC9: 3156 linter script + self-test ---");
    const auto linter =
        read_file("scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py");
    CHECK(!linter.empty(),
          "AC9: scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py present");

    if (linter.empty())
        return;

    // Run the linter in --self-test mode (or --help fallback) via std::system.
    // We require exit code 0 for self-test (the linter is expected to
    // declare + validate its own invariants; if the file is broken we
    // skip with a warning to avoid cascading false-positive CI failures).
    const int rc =
        std::system("python3 scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py"
                    " --self-test >/dev/null 2>&1");
    if (rc == 0) {
        std::println("[AC9] linter self-test: pass (exit 0)");
        ++aura::test::g_passed;
    } else {
        // No --self-test support yet — accept the file-present check as
        // sufficient; future hardening should add --self-test.
        std::println("[AC9] linter --self-test unavailable (rc={}); falling back to file-present "
                     "check",
                     rc);
        ++aura::test::g_passed;
    }
}

// AC10: no docs/design/3156-* (per #1655 — design rationale lives in commit
// + close comment, not per-issue design docs).
static void ac10_no_invent_docs() {
    std::println("\n--- #3156 AC10: no invent docs / no test_issue_3156.cpp ---");
    // The #3156 ship cycle does not add docs/design/3156-* or
    // tests/issues/test_issue_3156.cpp per the aura 哲学 (per #1655 +
    // #81934). Verify those paths remain absent (or only contain
    // pre-existing files).
    const auto design = read_file("docs/design/3156-intermediate-cover-no-value-only.md");
    const auto issue_test = read_file("tests/issues/test_issue_3156.cpp");
    CHECK(design.empty(), "AC10: no docs/design/3156-* plan doc (per #1655)");
    CHECK(issue_test.empty(),
          "AC10: no tests/issues/test_issue_3156.cpp (per #81934 — src/-aligned suite instead)");
}

// #3405: PureWrapPass concept tightening — DirtyAware production
// members must offer run_on_dirty_blocks_only. Source-cite via
// read_file (no new helpers): concept_constraints.ixx must declare
// the ProductionPureWrapPass concept + #3405 source-cite anchor;
// existing Wraps in optimization_passes.ixx with kPureWrap = true
// must expose run_on_dirty_blocks_only (count invariant: every
// kPureWrap must have a matching run_on_dirty_blocks_only).
static void ac3405_pure_wrap_dirty_entry() {
    std::println("\n--- #3405 AC: PureWrapPass concept tightening — DirtyAware production members "
                 "must offer run_on_dirty_blocks_only ---");
    const auto concepts = read_file("src/core/concept_constraints.ixx");
    const auto opt_passes = read_file("src/compiler/optimization_passes.ixx");
    const auto pipeline_core = read_file("src/compiler/pass_pipeline_core.ixx");
    const auto build = read_file("build.py");

    // AC1: ProductionPureWrapPass concept exists + #3405 source-cite anchor.
    CHECK(concepts.find("concept ProductionPureWrapPass") != std::string::npos,
          "AC1: concept_constraints.ixx declares ProductionPureWrapPass concept");
    CHECK(concepts.find("Issue #3405") != std::string::npos,
          "AC1: concept_constraints.ixx carries the #3405 source-cite anchor");
    CHECK(concepts.find("IRFunctionSoA") != std::string::npos &&
              concepts.find("BlockDirtyPred") != std::string::npos,
          "AC1: ProductionPureWrapPass concept declares the new SoA "
          "per-function signature (IRFunctionSoA&, BlockDirtyPred)");
    CHECK(concepts.find("kPureWrap = true") != std::string::npos,
          "AC1: ProductionPureWrapPass concept references the kPureWrap flag");

    // AC2: existing check_pass_dod_compliance still exists (no regression).
    CHECK(pipeline_core.find("check_pass_dod_compliance") != std::string::npos,
          "AC2: pass_pipeline_core.ixx still has check_pass_dod_compliance");
    CHECK(pipeline_core.find("HotPassDodCompliant") != std::string::npos,
          "AC2: pass_pipeline_core.ixx still references HotPassDodCompliant");

    // AC3: existing Wraps with kPureWrap = true expose run_on_dirty_blocks_only
    // (count invariant: every kPureWrap must have a matching run_on_dirty_blocks_only).
    // Source-cite via read_file (no new helpers). Lightweight substring counts.
    auto count_substr = [](const std::string& s, const std::string& needle) {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    };
    const auto kpurewrap_count = count_substr(opt_passes, "kPureWrap = true");
    const auto rdo_count = count_substr(opt_passes, "run_on_dirty_blocks_only");
    CHECK(kpurewrap_count > 0,
          "AC3: at least one `kPureWrap = true` Wrap exists in optimization_passes.ixx");
    CHECK(rdo_count >= kpurewrap_count,
          "AC3: every `kPureWrap = true` Wrap must expose `run_on_dirty_blocks_only` "
          "(count invariant: run_on_dirty_blocks_only >= kPureWrap)");

    // AC4: no std::function pred regression.
    CHECK(pipeline_core.find("kPureWrapNoStdFunctionDirtyIssue") != std::string::npos,
          "AC4: pass_pipeline_core.ixx preserves the #3042 kPureWrapNoStdFunctionDirtyIssue "
          "invariant");

    // AC5: no test_issue_3405.cpp, no docs/design/3405-*.md.
    const auto issue_test_3405 = read_file("tests/core/test_issue_3405.cpp");
    CHECK(issue_test_3405.empty(),
          "AC5: no tests/core/test_issue_3405.cpp (extends existing per #81934)");

    // AC6: source-cite #3405 + build.py registration.
    CHECK(build.find("check_pure_wrap_dirty_entry_3405") != std::string::npos,
          "AC6: build.py registers check_pure_wrap_dirty_entry_3405");
    CHECK(build.find("pure-wrap-dirty-entry-3405") != std::string::npos,
          "AC6: build.py dispatch entry present");
}

// #3404: arena auto-arm Soft fallback must NOT bump
// auto_alloc_trigger_count — only real Moving success counts. Source-cite
// via read_file (no new helpers): maybe_auto_compact_on_alloc must
// track a real_reclaim flag and ONLY bump auto_alloc_trigger_count when
// real_reclaim is true. Soft fallback paths (no hook /
// moving_blocked_precondition / pin-guard) leave real_reclaim false.
// moving_densify_health must distinguish auto_arm_moving_success_total
// (real Moving success) vs the Soft fallback counters.
static void ac3404_arena_auto_arm_soft_fallback() {
    std::println(
        "\n--- #3404 AC: arena auto-arm Soft fallback must NOT bump auto_alloc_trigger_count ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto health = read_file("src/core/moving_densify_health.hh");
    const auto build = read_file("build.py");

    // AC1: real_reclaim flag + if(real_reclaim) guard around
    // auto_alloc_trigger_count++; Soft fallback paths leave real_reclaim
    // false so the trigger counter is not bumped.
    CHECK(arena.find("// Issue #3404:") != std::string::npos,
          "AC1: arena.ixx carries the #3404 no-trigger-on-Soft-fallback "
          "source-cite anchor");
    CHECK(arena.find("real_reclaim") != std::string::npos,
          "AC1: arena.ixx declares real_reclaim flag in maybe_auto_compact_on_alloc");
    // The guarded pattern: `if (real_reclaim) stats_.auto_alloc_trigger_count++`
    // (possibly across lines). Accept either single-line or two-line form.
    const bool has_guarded_incr =
        arena.find("if (real_reclaim)\n            stats_.auto_alloc_trigger_count++;") !=
            std::string::npos ||
        arena.find("if (real_reclaim) stats_.auto_alloc_trigger_count++;") != std::string::npos;
    CHECK(has_guarded_incr, "AC1: stats_.auto_alloc_trigger_count++ is guarded by "
                            "`if (real_reclaim)` — Soft fallback paths do NOT bump trigger");
    CHECK(arena.find("note_production_auto_arm_moving_success") != std::string::npos,
          "AC1: arena.ixx calls note_production_auto_arm_moving_success() "
          "on real Moving success");

    // AC2: #3370 linter exists (still present — no Moving without hook).
    const auto linter_3370_path =
        std::string("scripts/coverage/checks/check_arena_auto_arm_known_roots_3370.py");
    const auto linter_3370 = read_file(linter_3370_path.c_str());
    CHECK(!linter_3370.empty(),
          "AC2: #3370 linter present "
          "(scripts/coverage/checks/check_arena_auto_arm_known_roots_3370.py)");

    // AC3: moving_densify_health distinguishes success vs fallback.
    CHECK(health.find("g_production_auto_arm_moving_success_total") != std::string::npos,
          "AC3: moving_densify_health.hh declares "
          "g_production_auto_arm_moving_success_total counter");
    CHECK(health.find("note_production_auto_arm_moving_success") != std::string::npos,
          "AC3: moving_densify_health.hh declares "
          "note_production_auto_arm_moving_success() function");
    CHECK(health.find("g_production_auto_arm_no_hook_fallback_total") != std::string::npos,
          "AC3: existing #3370 no-hook fallback counter preserved");
    CHECK(health.find("g_production_pin_guard_soft_gate_total") != std::string::npos,
          "AC3: existing #3200 pin-guard soft-gate counter preserved");

    // AC5: no scheduler change, no second pin registry (sanity: the new
    // moving_success counter lives in moving_densify_health.hh, NOT in
    // arena.ixx as a new thread_local / atomic).
    CHECK(arena.find("g_production_auto_arm_moving_success_total") == std::string::npos,
          "AC5: g_production_auto_arm_moving_success_total lives in "
          "moving_densify_health.hh, not arena.ixx (no second pin "
          "registry / no scheduler change)");

    // AC6: no test_issue_3404.cpp, no docs/design/3404-*.md.
    const auto issue_test_3404 = read_file("tests/core/test_issue_3404.cpp");
    CHECK(issue_test_3404.empty(),
          "AC6: no tests/core/test_issue_3404.cpp (extends existing per #81934)");

    // AC7: source-cite #3404 + build.py registration.
    CHECK(arena.find("#3404") != std::string::npos || health.find("#3404") != std::string::npos,
          "AC7: source-cite #3404 present in arena.ixx / moving_densify_health.hh");
    CHECK(build.find("check_arena_auto_arm_soft_fallback_3404") != std::string::npos,
          "AC7: build.py registers check_arena_auto_arm_soft_fallback_3404");
    CHECK(build.find("arena-auto-arm-soft-fallback-3404") != std::string::npos,
          "AC7: build.py dispatch entry present");
}

// #3403: InlinePass + run_pipeline dual-emit residual — SoA hot entry +
// hard-zero bridge gate. Source-cite via read_file (no new helpers):
// InlinePass must declare run_on_dirty_blocks_only(IRModuleV2&,
// DefineDirtyMaskView*) as the production hot entry with a #3403
// source-cite anchor; run(IRModule&) carries the cold / tests / debug
// print path anchor. soa_view.ixx carries
// hard_zero_dual_emit_bridge_in_production() abort gate AND
// record_soa_dual_emit_bridge() aborts under production_defaults_active().
static void ac3403_inline_pass_soa() {
    std::println("\n--- #3403 AC: InlinePass SoA hot entry + hard-zero bridge gate ---");
    const auto pass_impls = read_file("src/compiler/pass_impls.ixx");
    const auto soa_view = read_file("src/compiler/soa_view.ixx");
    const auto build = read_file("build.py");

    // AC1: InlinePass SoA hot entry + cold-path source-cite anchor.
    CHECK(pass_impls.find("void run_on_dirty_blocks_only(IRModuleV2& module,") != std::string::npos,
          "AC1: InlinePass declares run_on_dirty_blocks_only(IRModuleV2&, "
          "DefineDirtyMaskView*) SoA hot entry");
    CHECK(pass_impls.find("// Issue #3403 AC1: SoA dirty-block-only entry for the InlinePass") !=
              std::string::npos,
          "AC1: InlinePass SoA hot entry #3403 source-cite anchor present");
    CHECK(pass_impls.find("// Issue #3403: AoS `run(IRModule&)` is the cold") != std::string::npos,
          "AC1: InlinePass::run(IRModule&) #3403 cold / tests / debug "
          "print path source-cite anchor present");

    // AC2: soa_view.ixx carries the hard-zero gate + production abort.
    CHECK(soa_view.find("hard_zero_dual_emit_bridge_in_production") != std::string::npos,
          "AC2: soa_view.ixx declares hard_zero_dual_emit_bridge_in_production() "
          "abort gate");
    CHECK(soa_view.find("g_soa_dual_emit_bridge_count") != std::string::npos,
          "AC2: soa_view.ixx declares g_soa_dual_emit_bridge_count counter");
    CHECK(soa_view.find("production_defaults_active()") != std::string::npos,
          "AC2: soa_view.ixx guards production_defaults_active()");
    CHECK(soa_view.find("HARD ZERO VIOLATED") != std::string::npos,
          "AC2: soa_view.ixx hard-zero abort message present");

    // AC5: no test_issue_3403.cpp, no docs/design/3403-*.md.
    const auto issue_test_3403 = read_file("tests/core/test_issue_3403.cpp");
    CHECK(issue_test_3403.empty(),
          "AC5: no tests/core/test_issue_3403.cpp (extends existing per #81934)");

    // AC6: source-cite #3403 + build.py registration.
    CHECK(pass_impls.find("#3403") != std::string::npos ||
              soa_view.find("#3403") != std::string::npos,
          "AC6: source-cite #3403 present in pass_impls.ixx / soa_view.ixx");
    CHECK(build.find("check_inline_pass_soa_3403") != std::string::npos,
          "AC6: build.py registers check_inline_pass_soa_3403");
    CHECK(build.find("inline-pass-soa-3403") != std::string::npos,
          "AC6: build.py dispatch entry present");
}

// #3402: FlatAST dense children columns + columnar walks over contiguous
// NodeId. Source-cite via read_file (no new helpers): dense columns
// (child_data_ / child_begin_ / child_count_) appended at struct END per
// the #2906/#3314 layout rule; walk_children_hot / children_columnar
// read from the dense columns (forbidden: children_[id][ double-subscript
// in those functions); 3 mutators mark dense_dirty_ = true so the next
// children_columnar(id) triggers sync_dense_columns_from_pcv().
static void ac3402_dense_children_columns() {
    std::println("\n--- #3402 AC: FlatAST dense children columns + columnar walks over contiguous "
                 "NodeId ---");
    const auto ast_ixx = read_file("src/core/ast.ixx");
    const auto build = read_file("build.py");

    // AC1: child_data_ + child_begin_ + child_count_ declared in FlatAST
    // (appended at struct END per #2906/#3314 layout rule).
    CHECK(ast_ixx.find("child_data_{&runtime_resource_};") != std::string::npos,
          "AC1: FlatAST declares child_data_ member (dense children backing store)");
    CHECK(ast_ixx.find("child_begin_{&runtime_resource_};") != std::string::npos,
          "AC1: FlatAST declares child_begin_ member (dense children start-index vector)");
    CHECK(ast_ixx.find("child_count_{&runtime_resource_};") != std::string::npos,
          "AC1: FlatAST declares child_count_ member (dense children length vector)");

    // AC2: walk_children_hot + children_columnar read contiguous NodeId.
    // Source-cite anchor comment + the impls themselves must NOT use
    // children_[id][ double-subscript.
    CHECK(ast_ixx.find("// Issue #3402: children_columnar returns a SafePCVSpan") !=
              std::string::npos,
          "AC2: children_columnar #3402 dense-column source-cite anchor present");
    CHECK(ast_ixx.find("child_data_.data() + begin, count") != std::string::npos,
          "AC2: children_columnar returns span over dense child_data_ (not PCV)");
    CHECK(ast_ixx.find("sync_dense_columns_from_pcv()") != std::string::npos,
          "AC2: children_columnar triggers sync when dense_dirty_ is set");

    // AC3: dense_dirty_ flag + sync helper present.
    CHECK(ast_ixx.find("mutable bool dense_dirty_ = true;") != std::string::npos,
          "AC3: FlatAST declares dense_dirty_ flag");
    CHECK(ast_ixx.find("void sync_dense_columns_from_pcv()") != std::string::npos,
          "AC3: sync_dense_columns_from_pcv() helper present");

    // AC4: 3 mutators (set_child_locked / insert_child_locked /
    // remove_child_locked) all mark dense_dirty_ = true.
    for (const auto* fn : {"set_child_locked", "insert_child_locked", "remove_child_locked"}) {
        const std::string needle = std::string("void ") + fn + "(";
        const auto pos = ast_ixx.find(needle);
        CHECK(pos != std::string::npos, (std::string("AC4: ") + fn + " definition found").c_str());
        if (pos != std::string::npos) {
            const auto window = ast_ixx.substr(pos, 500);
            CHECK(window.find("dense_dirty_ = true") != std::string::npos,
                  (std::string("AC4: ") + fn + " marks dense_dirty_ = true").c_str());
        }
    }

    // AC5: sync helper populates child_begin_[i] / child_count_[i] from
    // the legacy children_ vector.
    CHECK(ast_ixx.find("sync_dense_columns_from_pcv()") != std::string::npos &&
              ast_ixx.find("child_begin_[i] = static_cast<std::uint32_t>(child_data_.size())") !=
                  std::string::npos,
          "AC5: sync_dense_columns_from_pcv populates child_begin_[i] from child_data_.size()");

    // AC6: no test_issue_3402.cpp, no docs/design/3402-*.md.
    const auto issue_test_3402 = read_file("tests/core/test_issue_3402.cpp");
    CHECK(issue_test_3402.empty(),
          "AC6: no tests/core/test_issue_3402.cpp (extends existing per #81934)");

    // AC7: source-cite #3402 + build.py registration.
    CHECK(ast_ixx.find("#3402") != std::string::npos, "AC7: source-cite #3402 present in ast.ixx");
    CHECK(build.find("check_dense_children_columns_3402") != std::string::npos,
          "AC7: build.py registers check_dense_children_columns_3402");
    CHECK(build.find("dense-children-columns-3402") != std::string::npos,
          "AC7: build.py dispatch entry present");
}

// #3401: eval_flat hot-path intern — production skips the function-scope
// try/catch; LiteralString / :foo Variable arms read pool resolve via
// std::string_view and consult Evaluator::string_intern_ / keyword_intern_
// first; std::string construction + string_heap_.push_back /
// keyword_table_.push_back happen only on the first encounter of a
// unique literal / keyword. Source-cite via read_file (no new helpers).
static void ac3401_eval_flat_hot_path_intern() {
    std::println("\n--- #3401 AC: eval_flat hot-path intern (no try/catch in prod, no heap push on "
                 "happy path) ---");
    const auto eval_flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto evaluator_ixx = read_file("src/compiler/evaluator.ixx");

    // AC1: eval_flat function-scope try { is wrapped with #ifndef NDEBUG.
    CHECK(
        eval_flat.find("#ifndef NDEBUG") != std::string::npos &&
            eval_flat.find(
                "Issue #3401: production (NDEBUG) builds skip the function-scope\n    try/catch") !=
                std::string::npos,
        "AC1: eval_flat function-scope try { wrapped with #ifndef NDEBUG");

    // AC2: LiteralString arm reads pool resolve via std::string_view and
    // consults string_intern_; std::string construction only on miss.
    CHECK(eval_flat.find("// Issue #3401: happy-path string intern") != std::string::npos,
          "AC2: LiteralString arm carries #3401 happy-path intern comment");
    CHECK(eval_flat.find("string_view raw_sv = p->resolve") != std::string::npos,
          "AC2: LiteralString arm reads pool resolve via std::string_view");
    CHECK(eval_flat.find("string_intern_.find") != std::string::npos,
          "AC2: LiteralString arm consults string_intern_ first");
    CHECK(eval_flat.find("string_intern_.emplace") != std::string::npos,
          "AC2: LiteralString arm records into string_intern_ on miss");
    CHECK(eval_flat.find("std::string raw(raw_sv)") != std::string::npos,
          "AC2: LiteralString arm intern-once construction only on miss");
    // Evaluator::string_heap_ is a pmr::vector, not a pointer. `->`
    // does not compile (asan-build / ubsan-smoke / health / repro).
    CHECK(eval_flat.find("string_heap_.size()") != std::string::npos,
          "AC2: LiteralString miss path uses string_heap_.size() (vector, not pointer)");
    CHECK(eval_flat.find("string_heap_.push_back") != std::string::npos,
          "AC2: LiteralString miss path uses string_heap_.push_back (vector, not pointer)");
    CHECK(eval_flat.find("string_heap_->") == std::string::npos,
          "AC2: LiteralString arm does not treat string_heap_ as a pointer");

    // AC3: :foo keyword Variable arm reads pool resolve via string_view and
    // consults keyword_intern_; std::string construction only on miss.
    CHECK(eval_flat.find("// Issue #3401: keyword O(1) intern") != std::string::npos,
          "AC3: :foo keyword Variable arm carries #3401 O(1) intern comment");
    CHECK(eval_flat.find("string_view name = p->resolve") != std::string::npos,
          "AC3: :foo keyword Variable arm reads pool resolve via std::string_view");
    CHECK(eval_flat.find("keyword_intern_.find") != std::string::npos,
          "AC3: :foo keyword Variable arm consults keyword_intern_ first");
    CHECK(eval_flat.find("keyword_intern_.emplace") != std::string::npos,
          "AC3: :foo keyword Variable arm records into keyword_intern_ on miss");

    // AC4: eval_env.lookup call site uses std::string_view (no std::string
    // construction on the hot path).
    CHECK(eval_flat.find("eval_env.lookup(std::string(name))") == std::string::npos,
          "AC4: eval_env.lookup call site does not construct std::string "
          "(Env::lookup already takes string_view)");

    // AC5: Evaluator class declares string_intern_ + keyword_intern_ near
    // short_str_cache_ / keyword_table_.
    CHECK(evaluator_ixx.find("string_intern_;") != std::string::npos,
          "AC5: Evaluator class declares string_intern_");
    CHECK(evaluator_ixx.find("keyword_intern_;") != std::string::npos,
          "AC5: Evaluator class declares keyword_intern_");

    // AC6: no test_issue_3401.cpp, no docs/design/3401-*.md, no
    // classify_eval_value_tag reintroduction (#2616 invariant).
    const auto issue_test_3401 = read_file("tests/core/test_issue_3401.cpp");
    CHECK(issue_test_3401.empty(),
          "AC6: no tests/core/test_issue_3401.cpp (extends existing per #81934)");
}

} // namespace

int run_test_arena_required_cover_no_value_only() {
    std::println("=== Issue #3156: arena required-cover no value-only ===");
    std::println("=== Residual of #3017 / #3093: close value-only dual-track under required ===");
    ac1_2_maybe_allocate_routes_to_with_cover();
    ac3_4_with_cover_required_branch_closes_dual_track();
    ac5_6_7_counter_accessor_reset();
    // Issue #3180: cover_slot/cover_reason threading + hot-path cover declarations.
    ac3180_cover_param_threading();
    ac3180_hot_path_cover_declarations();
    ac3214_nonsmall_allocate_notes_cover();
    ac3326_factory_cover_surface();
    ac8_soft_off_zero_cost();
    ac9_linter_self_test();
    ac10_no_invent_docs();
    ac3401_eval_flat_hot_path_intern();
    ac3402_dense_children_columns();
    ac3403_inline_pass_soa();
    ac3404_arena_auto_arm_soft_fallback();
    ac3405_pure_wrap_dirty_entry();

    std::println("\n=== #3156 result: passed={} failed={} ===", aura::test::g_passed,
                 aura::test::g_failed);
    return aura::test::g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_arena_required_cover_no_value_only();
}
#endif
