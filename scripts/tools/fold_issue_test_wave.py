#!/usr/bin/env python3
"""Fold issue-suffixed test_*.cpp into multi-TU thematic batches.

Wave consolidation helper (tests/HOMES.md). For each member:
  1. Ensure int run_test_<stem>() + #ifndef AURA_ISSUE_BATCH_MEMBER main
  2. Optionally strip trailing _NNNN from filename (unique names only)
  3. Emit batch driver + CMake multi-source target
  4. Remove standalone aura_add_issue_test / link / deps for members
  5. Rewrite coverage manifests + bulk path refs to new paths

Usage:
  python3 scripts/tools/fold_issue_test_wave.py --wave W1 --dry-run
  python3 scripts/tools/fold_issue_test_wave.py --wave W1 --apply
  python3 scripts/tools/fold_issue_test_wave.py --wave W2 --apply
  ...
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Wave → (batch_stem, dir under tests/, list of path globs or explicit relative paths)
# Paths are matched against existing issue-suffixed files.
WAVE_SPECS: dict[str, dict] = {
    "W1": {
        "batch": "test_flatast_atomic_lock_batch",
        "dir": "core",
        "title": "FlatAST / SoA atomic + lock ACs",
        # Prefer core FlatAST/lock family (exclude hot_pass / lock_order / rest_param noise)
        "include_re": re.compile(
            r"tests/core/test_("
            r"soa_column_atomic|macro_dirty_bits_lock|verification_dirty_bits_lock|"
            r"region_dense_atomic|flatast_add_node_lock|structural_metadata_lock_order|"
            r"incoming_parent_dirty_atomic|last_validated_generation_atomic|"
            r"restamp_lazy_align_atomic|binding_gens_atomic|subtree_gen_atomic|"
            r"dirty_column_lock|stringpool_bytes_total_lock|stringpool_buf_fragmentation_lock|"
            r"tag_arity_index_lock|tag_arity_key_hash|sandbox_mode_atomic|"
            r"gc_defer_overflow_policy_atomic|gc_defer_arm_fetch_or|gc_defer_reconcile_cas|"
            r"clear_macro_dirty_concurrent|node_meta_bounds|node_meta_gap|"
            r"param_data_mutation_contract|param_annot_mutation_contract|param_begin_count_publish|"
            r"add_node_builder_contract|defines_referencing_sym|get_nodeview_snapshot|"
            r"raii_guard_flatast_lifetime|restore_children_structural_lock|"
            r"summary_recompute_sym|summary_flags_guard|mutation_log_cow_copy|"
            r"flatast_soa_read_guard|subtree_uses_sym_template_bloat|"
            r"region_lambda_dense_race|region_sym_dense_race|region_sym_map_race"
            r")_\d{3,5}\.cpp$"
        ),
    },
    "W2": {
        "batch": "test_security_capability_batch",
        "dir": "compiler",
        "title": "Security / capability / grant / Restricted / audit",
        "include_re": re.compile(
            r"tests/(compiler|core)/test_("
            r"capability_|grant_|security_|require_effect_|tenant_scope|"
            r"restricted_|audit_mutation|audit_ring|audit_mid|side_effect|"
            r"hard_fiber|security_posture|cap_write|sandbox_"
            r").*_\d{3,5}\.cpp$"
        ),
    },
    "W3": {
        "batch": "test_densify_pin_batch",
        "dir": "core",
        "title": "Densify / pin / Moving / envframe ownership",
        "include_re": re.compile(
            r"tests/(core|compiler)/test_("
            r".*densify.*|.*_pin_.*|general_object_pin|root_remap|envframe_|"
            r"moving_|lifetime_|panic_residual|panic_defer|ownership_"
            r").*_\d{3,5}\.cpp$|"
            r"tests/(core|compiler)/test_("
            r"general_object_pin_\d+|root_remap_\d+|envframe_.*_\d+|"
            r"moving_.*_\d+|lifetime_.*_\d+|panic_.*_\d+|densify_.*_\d+"
            r")\.cpp$"
        ),
    },
    "W4": {
        "batch": "test_aot_jit_stamp_batch",
        "dir": "compiler",
        "title": "AOT / SpecJIT / stamp / relower / reload",
        "include_re": re.compile(
            r"tests/compiler/test_("
            r"aot_|specjit|.*pereval.*|.*reemit.*|.*relower.*|.*stamp.*|"
            r"exhausted_min|layout_stamp|env_gen|stale_probe|hot_update|reload_"
            r").*_\d{3,5}\.cpp$"
        ),
    },
    "W5": {
        "batch": "test_mailbox_fiber_batch",
        "dir": "serve",
        "title": "Mailbox / fiber / residual / steal / chaos",
        "include_re": re.compile(
            r"tests/(serve|compiler|orch)/test_("
            r"mailbox_|fiber_|residual_|steal_|chaos_|is_stealable|"
            r"join_drain|orphan|force_safepoint|agent_scope_concurrent|"
            r"hold_|reclaim_"
            r").*_\d{3,5}\.cpp$"
        ),
    },
    "W6": {
        "batch": "test_occurrence_coercion_batch",
        "dir": "compiler",
        "title": "Occurrence / cone / coercion / type gates",
        "include_re": re.compile(
            r"tests/compiler/test_("
            r"occurrence_|.*cone.*|coercion_|dead_coercion|partial_cone|"
            r"batch_dirty_|castop_|bidirectional_|type_system_|type_timeout|"
            r"type_freshness|type_linear|type_dep|instance_|adt_|solve_delta|"
            r"boundary_solve|blame_|composite_"
            r").*_\d{3,5}\.cpp$"
        ),
    },
    "W7": {
        "batch": "test_linear_misc_batch",
        "dir": "compiler",
        "title": "Linear ownership residual (non cross-closure)",
        "include_re": re.compile(
            r"tests/compiler/test_("
            r"linear_(?!cross_closure).*|ownership_.*linear.*"
            r")_\d{3,5}\.cpp$"
        ),
    },
    "W_orch": {
        "batch": "test_orch_agent_batch",
        "dir": "orch",
        "title": "Orch / agent / parallel-intend",
        "include_re": re.compile(r"tests/orch/test_.*_\d{3,5}\.cpp$"),
    },
    "W_obs": {
        "batch": "test_obs_misc_batch",
        "dir": "compiler",
        "title": "Obs / health / epoch invariant leftovers",
        "include_re": re.compile(
            r"tests/(compiler|core)/test_("
            r".*health.*|epoch_invariant|memo_goal|fillup_|obs_.*"
            r")_\d{3,5}\.cpp$"
        ),
    },
    "W_other": {
        "batch": "test_misc_issue_fold_batch",
        "dir": "compiler",
        "title": "Leftover issue-suffixed tests",
        "include_re": None,  # filled dynamically: all remaining
    },
}

ISSUE_SUFFIX = re.compile(r"^(?P<base>.+)_(?P<issue>\d{3,5})$")
STANDALONE_BLOCK = re.compile(
    r"(?:^# [^\n]*\n)?"
    r"aura_add_issue_test\((?P<name>test_[A-Za-z0-9_]+)\)\n"
    r"aura_issue_test_link_(?:light|llvm_jit|llvm_jit_minimal)\((?P=name)\)\n"
    r"add_dependencies\(all_test_issue_targets (?P=name)\)\n",
    re.M,
)


def list_issue_files() -> list[Path]:
    out: list[Path] = []
    for p in (ROOT / "tests").rglob("test_*.cpp"):
        if re.search(r"_\d{3,5}\.cpp$", p.name) or re.match(r"test_issue_\d+", p.name):
            out.append(p)
    return sorted(out)


def strip_issue_stem(stem: str) -> str:
    m = ISSUE_SUFFIX.match(stem)
    if not m:
        return stem
    return m.group("base")


def convert_to_batch_member(path: Path, run_name: str, dry: bool) -> bool:
    """Rewrite main() → run_test_* + optional main wrapper. Returns True if changed."""
    text = path.read_text(encoding="utf-8")
    if f"int {run_name}(" in text and "AURA_ISSUE_BATCH_MEMBER" in text:
        return False

    # Already has run_ but no guard
    m_run = re.search(rf"int {re.escape(run_name)}\s*\(", text)
    if m_run and "int main" in text:
        # ensure guard around main only
        if "AURA_ISSUE_BATCH_MEMBER" not in text:
            text2 = re.sub(
                r"(int main\s*\([^)]*\)\s*\{)",
                r"#ifndef AURA_ISSUE_BATCH_MEMBER\n\1",
                text,
                count=1,
            )
            # close #endif before EOF if not present
            if "#endif" not in text2[text2.find("int main") :]:
                text2 = text2.rstrip() + "\n#endif\n"
            if not dry:
                path.write_text(text2, encoding="utf-8")
            return text2 != text
        return False

    # Transform int main() { ... } into int run_name() { ... }
    # Prefer last int main in file
    mains = list(re.finditer(r"^int main\s*\([^)]*\)\s*\{", text, re.M))
    if not mains:
        print(f"  WARN: no main() in {path.relative_to(ROOT)} — skip convert", file=sys.stderr)
        return False
    m = mains[-1]
    # Replace signature
    new_sig = f"int {run_name}() {{"
    text2 = text[: m.start()] + new_sig + text[m.end() :]
    # Append wrapper
    wrapper = f"\n#ifndef AURA_ISSUE_BATCH_MEMBER\nint main() {{\n    return {run_name}();\n}}\n#endif\n"
    if "AURA_ISSUE_BATCH_MEMBER" not in text2:
        text2 = text2.rstrip() + "\n" + wrapper
    if not dry:
        path.write_text(text2, encoding="utf-8")
    return True


def emit_batch_driver(batch_name: str, title: str, members: list[str], path: Path, dry: bool) -> None:
    lines = [
        f"// {batch_name}.cpp — thematic multi-TU batch",
        f"// {title}",
        "// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.",
        "// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.",
        "",
        '#include "test_harness.hpp"',
        "",
        "#include <print>",
        "",
        "import std;",
        "",
    ]
    for stem in members:
        lines.append(f"extern int run_{stem}();")
    lines += [
        "",
        "int main() {",
        "    using aura::test::g_failed;",
        "    using aura::test::g_passed;",
        "    int members_failed = 0;",
        "    int members_passed = 0;",
        f'    std::println("=== {batch_name} ({len(members)} members) ===");',
        "",
    ]
    for stem in members:
        lines += [
            f'    std::println("\\n──── {stem} ────");',
            "    g_passed = 0;",
            "    g_failed = 0;",
            f"    if (run_{stem}() != 0 || g_failed != 0) {{",
            "        ++members_failed;",
            f'        std::println("FAIL member {stem} (checks: {{}} passed, {{}} failed)",',
            "                     g_passed, g_failed);",
            "    } else {",
            "        ++members_passed;",
            f'        std::println("OK member {stem} ({{}} checks)", g_passed);',
            "    }",
            "",
        ]
    lines += [
        '    std::println("\\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,',
        "                 members_passed, members_failed);",
        "    return members_failed ? 1 : 0;",
        "}",
        "",
    ]
    body = "\n".join(lines)
    if not dry:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")


def cmake_batch_block(batch_name: str, title: str, rel_sources: list[str]) -> str:
    srcs = "\n".join(f"    {s}" for s in rel_sources)
    return f"""
# Domain batch: {title} ({len(rel_sources) - 1} members + driver).
add_executable({batch_name}
{srcs}
)
set_property(TARGET {batch_name} PROPERTY CXX_MODULE_STD ON)
target_include_directories({batch_name} PRIVATE src tests)
target_compile_definitions({batch_name} PRIVATE
    AURA_SOURCE_DIR="${{CMAKE_SOURCE_DIR}}"
    AURA_ISSUE_BATCH_MEMBER=1)
aura_test_compile_options({batch_name})
target_link_libraries({batch_name} PRIVATE
    aura_test_objects pthread stdc++ stdc++exp aura-reflect)
aura_issue_test_link_light({batch_name})
add_test(NAME {batch_name}_verification COMMAND ./{batch_name})
if(TARGET all_test_issue_targets)
    add_dependencies(all_test_issue_targets {batch_name})
endif()
"""


def remove_standalone_cmake(cmake: str, names: set[str]) -> tuple[str, int]:
    removed = 0

    def repl(m: re.Match) -> str:
        nonlocal removed
        if m.group("name") in names:
            removed += 1
            return ""
        return m.group(0)

    return STANDALONE_BLOCK.sub(repl, cmake), removed


def rewrite_paths(old_to_new: dict[str, str], dry: bool) -> int:
    """Rewrite path strings in manifests, coverage checks, tests, cmake."""
    if not old_to_new:
        return 0
    nfiles = 0
    roots = [
        ROOT / "scripts" / "coverage",
        ROOT / "tests",
        ROOT / "CMakeLists.txt",
        ROOT / "cmake",
        ROOT / "docs" / "generated",
        ROOT / "src",
    ]
    exts = {".json", ".py", ".cpp", ".h", ".hh", ".ixx", ".md", ".cmake", ".txt", ".inc"}
    for root in roots:
        paths = [root] if root.is_file() else list(root.rglob("*"))
        for p in paths:
            if not p.is_file() or p.suffix not in exts:
                continue
            try:
                t = p.read_text(encoding="utf-8")
            except Exception:
                continue
            orig = t
            for old, new in old_to_new.items():
                t = t.replace(old, new)
            if t != orig:
                nfiles += 1
                if not dry:
                    p.write_text(t, encoding="utf-8")
    return nfiles


def select_wave_files(wave: str, claimed: set[Path]) -> list[Path]:
    all_files = list_issue_files()
    if wave == "W_other":
        return [p for p in all_files if p not in claimed]
    spec = WAVE_SPECS[wave]
    rx = spec["include_re"]
    out = []
    for p in all_files:
        if p in claimed:
            continue
        rel = str(p.relative_to(ROOT)).replace("\\", "/")
        if rx.search(rel):
            out.append(p)
    return out


def apply_wave(wave: str, dry: bool, rename: bool) -> None:
    if wave not in WAVE_SPECS:
        raise SystemExit(f"unknown wave {wave}; choose from {list(WAVE_SPECS)}")

    # Compute claimed by earlier waves for W_other
    order = ["W1", "W2", "W3", "W4", "W5", "W6", "W7", "W_orch", "W_obs", "W_other"]
    claimed: set[Path] = set()
    for w in order:
        if w == wave:
            break
        claimed.update(select_wave_files(w, claimed))

    files = select_wave_files(wave, claimed)
    if not files:
        print(f"{wave}: no files")
        return

    spec = WAVE_SPECS[wave]
    batch = spec["batch"]
    batch_dir = ROOT / "tests" / spec["dir"]
    driver_path = batch_dir / f"{batch}.cpp"

    print(f"{wave}: {len(files)} files → {batch} ({spec['dir']}/)")

    old_to_new: dict[str, str] = {}
    member_stems: list[str] = []
    member_rel: list[str] = []
    used_stems: set[str] = set()

    for p in files:
        stem = p.stem  # test_foo_2410
        run_name = f"run_{stem}"
        new_path = p
        if rename:
            base = strip_issue_stem(stem)
            if base == stem:
                pass
            else:
                # uniqueness
                candidate = p.with_name(base + ".cpp")
                if candidate.exists() and candidate.resolve() != p.resolve():
                    print(f"  keep issue suffix (collision): {p.name}")
                else:
                    # also avoid stem collision across wave
                    if base in used_stems:
                        print(f"  keep issue suffix (stem clash): {p.name}")
                    else:
                        old_rel = str(p.relative_to(ROOT)).replace("\\", "/")
                        new_rel = str(candidate.relative_to(ROOT)).replace("\\", "/")
                        old_to_new[old_rel] = new_rel
                        old_to_new[p.name] = candidate.name
                        # run_ name keeps old stem for less churn in convert, then rename symbol?
                        # Prefer run_test_<new_stem> aligned with file
                        stem = base
                        run_name = f"run_{stem}"
                        if not dry:
                            p.rename(candidate)
                        new_path = candidate
                        used_stems.add(base)
                        print(f"  rename {Path(old_rel).name} → {candidate.name}")

        used_stems.add(stem)
        changed = convert_to_batch_member(new_path, run_name, dry)
        print(f"  {'convert' if changed else 'ready '} {new_path.relative_to(ROOT)} ({run_name})")
        member_stems.append(stem)
        member_rel.append(str(new_path.relative_to(ROOT)).replace("\\", "/"))

    # Driver
    emit_batch_driver(batch, spec["title"], member_stems, driver_path, dry)
    print(f"  driver {driver_path.relative_to(ROOT)}")

    # CMake: remove old flatast batch block if replacing W1, remove standalones, insert new block
    cmake_path = ROOT / "CMakeLists.txt"
    cmake = cmake_path.read_text(encoding="utf-8")

    # Drop previous definition of this batch executable if present
    old_batch_re = re.compile(
        rf"\n# Domain batch:.*?add_executable\({re.escape(batch)}\n.*?"
        rf"add_dependencies\(all_test_issue_targets {re.escape(batch)}\)\nendif\(\)\n",
        re.S,
    )
    cmake2, n_old = old_batch_re.subn("\n", cmake, count=1)
    if n_old:
        print(f"  removed prior CMake block for {batch}")
        cmake = cmake2

    # Also remove the original W1 hardcoded flatast block when doing W1
    if wave == "W1":
        old_w1 = re.compile(
            r"\n# Domain batch: FlatAST / SoA atomic\+lock ACs.*?"
            r"add_dependencies\(all_test_issue_targets test_flatast_atomic_lock_batch\)\nendif\(\)\n",
            re.S,
        )
        cmake2, n = old_w1.subn("\n", cmake, count=1)
        if n:
            cmake = cmake2
            print("  removed legacy flatast_atomic_lock_batch CMake block")

    set(member_stems)
    # stems may be without test_ prefix
    {s if s.startswith("test_") else f"test_{s}" if False else s for s in member_stems}
    # member_stems are full file stems like test_foo_2410 or test_foo
    cmake, n_rm = remove_standalone_cmake(cmake, set(member_stems))
    print(f"  removed {n_rm} standalone CMake target blocks")

    sources = [str(driver_path.relative_to(ROOT)).replace("\\", "/")] + member_rel
    block = cmake_batch_block(batch, spec["title"], sources)

    # Insert before first aura_add_issue_test after a marker, or after arena_batch block
    anchor = "aura_add_issue_test(test_arena_batch)"
    cmake = cmake.replace(anchor, block + "\n" + anchor, 1) if anchor in cmake else cmake + "\n" + block

    if not dry:
        cmake_path.write_text(cmake, encoding="utf-8")

    n_path = rewrite_paths(old_to_new, dry)
    print(f"  path rewrites in {n_path} files (rename map size {len(old_to_new)})")
    print(f"{wave}: done ({'dry-run' if dry else 'applied'})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wave", default="all", help="W1..W7, W_orch, W_obs, W_other, or all")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--rename", action="store_true", help="strip _NNNN from filenames when unique")
    ap.add_argument("--list", action="store_true", help="only list files per wave")
    args = ap.parse_args()
    dry = not args.apply
    if args.dry_run:
        dry = True

    all_waves = ["W1", "W2", "W3", "W4", "W5", "W6", "W7", "W_orch", "W_obs", "W_other"]
    waves = all_waves if args.wave == "all" else [args.wave]

    claimed: set[Path] = set()
    for w in all_waves:
        files = select_wave_files(w, claimed)
        if args.list or (args.wave == "all" and dry and not args.apply):
            print(f"{w}: {len(files)}")
            if args.list:
                for p in files[:12]:
                    print(f"  {p.relative_to(ROOT)}")
                if len(files) > 12:
                    print(f"  ... +{len(files) - 12}")
        claimed.update(files)

    if args.list:
        return 0

    # Real apply path — don't print list-only summary as substitute
    for w in waves:
        apply_wave(w, dry=dry, rename=args.rename)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
