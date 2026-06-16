// test_issues_main.cpp — Single-binary entry point for all
// test_issue_*.cpp tests (Issue #226 cycle 3).
//
// Background: before this file, each test_issue_*.cpp was
// its own ninja target and binary. That was 85 targets,
// 85 binaries, 85 link steps. The refactor consolidates
// them into ONE binary `test_issues` that links all
// the test sources together. The per-file `int main()` has
// been renamed to `int run_issue_NNN()` (see
// tests/migrate_to_single_target.py), and this wrapper
// provides the real `int main()` that calls them all in
// sequence.
//
// Output: aggregated pass/fail counts, similar to the
// run_issue_tests.py runner but inside one process.
// The runner (run_issue_tests.py) is still the canonical
// way to run from CI; this binary exists for:
//   - Faster local runs (no per-binary startup overhead)
//   - One process to attach a debugger to
//   - Easier to add a new test_issue_NNN.cpp (no CMake
//     changes — just create the file and add an extern
//     declaration below)
//
// Adding a new issue test:
//   1. Create tests/test_issue_NNN.cpp with
//        int run_issue_NNN() { ... return RUN_ALL_TESTS(); }
//   2. Add `extern int run_issue_NNN();` below
//   3. Add a `RUN(115, "test_issue_115")` line in main()
//   4. Done. No CMake change needed (the source file is
//      auto-discovered by the glob in CMakeLists.txt).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Per-issue test entry points. Each test_issue_NNN.cpp
// exports a function with this signature. Auto-generated
// list of all 85 issue numbers below.
//
// If you add a new test_issue_NNN.cpp, add the extern
// here AND add a call below. The CMake glob in
// aura_add_issues_target() will pick up the .cpp file
// automatically.

extern int run_issue_115();
extern int run_issue_116();
extern int run_issue_117();
extern int run_issue_118();
extern int run_issue_119();
extern int run_issue_120();
extern int run_issue_121();
extern int run_issue_122();
extern int run_issue_123();
extern int run_issue_124();
extern int run_issue_125();
extern int run_issue_126();
extern int run_issue_127();
extern int run_issue_128();
extern int run_issue_130();
extern int run_issue_131();
extern int run_issue_132();
extern int run_issue_134();
extern int run_issue_135();
extern int run_issue_136();
extern int run_issue_137();
extern int run_issue_138();
extern int run_issue_139();
extern int run_issue_140();
extern int run_issue_141();
extern int run_issue_142();
extern int run_issue_143();
extern int run_issue_144();
extern int run_issue_145();
extern int run_issue_146();
extern int run_issue_147();
extern int run_issue_148();
extern int run_issue_149();
extern int run_issue_150();
extern int run_issue_151();
extern int run_issue_152();
extern int run_issue_153();
extern int run_issue_154();
extern int run_issue_155();
extern int run_issue_156();
extern int run_issue_157();
extern int run_issue_158();
extern int run_issue_159();
extern int run_issue_160();
extern int run_issue_161();
extern int run_issue_162();
extern int run_issue_163();
extern int run_issue_164();
extern int run_issue_165();
extern int run_issue_166();
extern int run_issue_167();
extern int run_issue_168();
extern int run_issue_169();
extern int run_issue_170();
extern int run_issue_171();
extern int run_issue_172();
extern int run_issue_173();
extern int run_issue_174();
extern int run_issue_175();
extern int run_issue_176();
extern int run_issue_177();
extern int run_issue_178();
extern int run_issue_179();
extern int run_issue_180();
extern int run_issue_181();
extern int run_issue_182();
extern int run_issue_183();
extern int run_issue_184();
extern int run_issue_185();
extern int run_issue_186();
extern int run_issue_187();
extern int run_issue_188();
extern int run_issue_189();
extern int run_issue_190();
extern int run_issue_191();
extern int run_issue_192();
extern int run_issue_193();
extern int run_issue_194();
extern int run_issue_195();
extern int run_issue_196();
extern int run_issue_197();
extern int run_issue_214();
extern int run_issue_215();
extern int run_issue_216();
extern int run_issue_217();
extern int run_issue_218();
extern int run_issue_219();
extern int run_issue_220();
extern int run_issue_221();
extern int run_issue_222();
extern int run_issue_223();
extern int run_issue_224();

// Mapping from issue number to test function. Used by main()
// to iterate all tests in order. (A std::vector is used
// instead of a static array to make it easy to add/remove
// entries without changing the array type.)
struct IssueTest {
    int number;
    int (*fn)();
    const char* name;
};

static const std::vector<IssueTest> kIssueTests = {
    {115, run_issue_115, "test_issue_115"},
    {116, run_issue_116, "test_issue_116"},
    {117, run_issue_117, "test_issue_117"},
    {118, run_issue_118, "test_issue_118"},
    {119, run_issue_119, "test_issue_119"},
    {120, run_issue_120, "test_issue_120"},
    {121, run_issue_121, "test_issue_121"},
    {122, run_issue_122, "test_issue_122"},
    {123, run_issue_123, "test_issue_123"},
    {124, run_issue_124, "test_issue_124"},
    {125, run_issue_125, "test_issue_125"},
    {126, run_issue_126, "test_issue_126"},
    {127, run_issue_127, "test_issue_127"},
    {128, run_issue_128, "test_issue_128"},
    {130, run_issue_130, "test_issue_130"},
    {131, run_issue_131, "test_issue_131"},
    {132, run_issue_132, "test_issue_132"},
    {134, run_issue_134, "test_issue_134"},
    {135, run_issue_135, "test_issue_135"},
    {136, run_issue_136, "test_issue_136"},
    {137, run_issue_137, "test_issue_137"},
    {138, run_issue_138, "test_issue_138"},
    {139, run_issue_139, "test_issue_139"},
    {140, run_issue_140, "test_issue_140"},
    {141, run_issue_141, "test_issue_141"},
    {142, run_issue_142, "test_issue_142"},
    {143, run_issue_143, "test_issue_143"},
    {144, run_issue_144, "test_issue_144"},
    {145, run_issue_145, "test_issue_145"},
    {146, run_issue_146, "test_issue_146"},
    {147, run_issue_147, "test_issue_147"},
    {148, run_issue_148, "test_issue_148"},
    {149, run_issue_149, "test_issue_149"},
    {150, run_issue_150, "test_issue_150"},
    {151, run_issue_151, "test_issue_151"},
    {152, run_issue_152, "test_issue_152"},
    {153, run_issue_153, "test_issue_153"},
    {154, run_issue_154, "test_issue_154"},
    {155, run_issue_155, "test_issue_155"},
    {156, run_issue_156, "test_issue_156"},
    {157, run_issue_157, "test_issue_157"},
    {158, run_issue_158, "test_issue_158"},
    {159, run_issue_159, "test_issue_159"},
    {160, run_issue_160, "test_issue_160"},
    {161, run_issue_161, "test_issue_161"},
    {162, run_issue_162, "test_issue_162"},
    {163, run_issue_163, "test_issue_163"},
    {164, run_issue_164, "test_issue_164"},
    {165, run_issue_165, "test_issue_165"},
    {166, run_issue_166, "test_issue_166"},
    {167, run_issue_167, "test_issue_167"},
    {168, run_issue_168, "test_issue_168"},
    {169, run_issue_169, "test_issue_169"},
    {170, run_issue_170, "test_issue_170"},
    {171, run_issue_171, "test_issue_171"},
    {172, run_issue_172, "test_issue_172"},
    {173, run_issue_173, "test_issue_173"},
    {174, run_issue_174, "test_issue_174"},
    {175, run_issue_175, "test_issue_175"},
    {176, run_issue_176, "test_issue_176"},
    {177, run_issue_177, "test_issue_177"},
    {178, run_issue_178, "test_issue_178"},
    {179, run_issue_179, "test_issue_179"},
    {180, run_issue_180, "test_issue_180"},
    {181, run_issue_181, "test_issue_181"},
    {182, run_issue_182, "test_issue_182"},
    {183, run_issue_183, "test_issue_183"},
    {184, run_issue_184, "test_issue_184"},
    {185, run_issue_185, "test_issue_185"},
    {186, run_issue_186, "test_issue_186"},
    {187, run_issue_187, "test_issue_187"},
    {188, run_issue_188, "test_issue_188"},
    {189, run_issue_189, "test_issue_189"},
    {190, run_issue_190, "test_issue_190"},
    {191, run_issue_191, "test_issue_191"},
    {192, run_issue_192, "test_issue_192"},
    {193, run_issue_193, "test_issue_193"},
    {194, run_issue_194, "test_issue_194"},
    {195, run_issue_195, "test_issue_195"},
    {196, run_issue_196, "test_issue_196"},
    {197, run_issue_197, "test_issue_197"},
    {214, run_issue_214, "test_issue_214"},
    {215, run_issue_215, "test_issue_215"},
    {216, run_issue_216, "test_issue_216"},
    {217, run_issue_217, "test_issue_217"},
    {218, run_issue_218, "test_issue_218"},
    {219, run_issue_219, "test_issue_219"},
    {220, run_issue_220, "test_issue_220"},
    {221, run_issue_221, "test_issue_221"},
    {222, run_issue_222, "test_issue_222"},
    {223, run_issue_223, "test_issue_223"},
    {224, run_issue_224, "test_issue_224"},
};

int main(int argc, char** argv) {
    // Optional: filter to a specific issue number via argv.
    // E.g. `./test_issues 196` runs only test_issue_196.
    int filter = 0;
    if (argc >= 2) {
        filter = std::atoi(argv[1]);
    }

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    int total = 0;
    for (const auto& t : kIssueTests) {
        if (filter != 0 && t.number != filter) {
            ++skipped;
            continue;
        }
        ++total;
        std::printf("--- %s ---\n", t.name);
        int rc = t.fn();
        if (rc == 0) {
            ++passed;
        } else {
            ++failed;
        }
    }

    std::printf("\n════════════════════════════════════════\n");
    if (filter == 0) {
        std::printf("All issues: %d ran, %d passed, %d failed, %d skipped\n",
                    total, passed, failed, skipped);
    } else {
        std::printf("Issue #%d: %s\n", filter,
                    failed == 0 ? "PASSED" : "FAILED");
    }
    return failed == 0 ? 0 : 1;
}
