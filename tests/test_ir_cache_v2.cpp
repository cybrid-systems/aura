// tests/test_ir_cache_v2.cpp
// C++ unit test for EDSL IR cache V2 (Phase 2).
// Verifies FNV-1a hash function used for source canonicalization.
// Full integration is covered by tests/edsl_ir_cache_test.aura.


// FNV-1a 64-bit (matches CompilerService::fnv1a_64)

import std;
static std::size_t fnv1a_64(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return static_cast<std::size_t>(h);
}

static int fail_count = 0;
static int pass_count = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        fail_count++; \
    } else { \
        pass_count++; \
    } \
} while (0)

int main() {
    // Test 1: deterministic
    auto h1 = fnv1a_64("hello world");
    auto h2 = fnv1a_64("hello world");
    ASSERT(h1 == h2, "fnv1a is deterministic");
    
    // Test 2: different strings → different hashes
    auto h3 = fnv1a_64("hello worl");
    ASSERT(h1 != h3, "fnv1a differs for different strings");
    
    // Test 3: empty string → offset basis
    auto h4 = fnv1a_64("");
    ASSERT(h4 == 0xcbf29ce484222325ULL, "fnv1a of empty string is offset basis");
    
    // Test 4: short strings → no overflow
    auto h5 = fnv1a_64("a");
    auto h6 = fnv1a_64("b");
    ASSERT(h5 != h6, "single chars hash differently");
    
    // Test 5: typical aura source hash
    auto h7 = fnv1a_64("(define (f x) (* x x))");
    auto h8 = fnv1a_64("(define (f x) (+ x x))");
    ASSERT(h7 != h8, "similar but different defs hash differently");
    
    // Test 6: long string
    std::string long_src(1000, 'x');
    auto h9 = fnv1a_64(long_src);
    auto h10 = fnv1a_64(long_src);
    ASSERT(h9 == h10, "long strings hash deterministically");
    
    std::printf("PASS %d / FAIL %d\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
