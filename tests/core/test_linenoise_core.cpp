// Issue #5041: direct key-by-key tests of the REPL line editor. No terminal.
#include "linenoise/linenoise_core.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void check(bool ok, const char* what, const char* got, const char* want) {
    if (ok)
        return;
    std::fprintf(stderr, "FAIL %s: got [%s] want [%s]\n", what, got ? got : "(null)",
                 want ? want : "(null)");
    ++failures;
}

static void expect(const ln_core& e, const char* text, size_t pos, const char* what) {
    check(std::strcmp(e.buf, text) == 0, what, e.buf, text);
    check(e.pos == pos, what, std::to_string(e.pos).c_str(), std::to_string(pos).c_str());
}

static void test_words() {
    ln_core e;
    ln_core_init(&e);
    ln_core_set(&e, "map foo-bar? baz");
    check(ln_core_word_left(&e, e.len) == 13, "word_left over baz",
          std::to_string(ln_core_word_left(&e, e.len)).c_str(), "13");
    check(ln_core_word_left(&e, 13) == 4, "word_left over foo-bar?",
          std::to_string(ln_core_word_left(&e, 13)).c_str(), "4");
    check(ln_core_word_right(&e, 0) == 3, "word_right over map",
          std::to_string(ln_core_word_right(&e, 0)).c_str(), "3");
    check(ln_core_is_word('?') && ln_core_is_word('-') && !ln_core_is_word(' '), "identifier set",
          "", "");
}

static void test_kill_and_yank() {
    ln_core e;
    ln_core_init(&e);
    ln_core_set(&e, "alpha beta");
    ln_core_kill_word_back(&e);
    expect(e, "alpha ", 6, "C-w kills the word before the cursor");
    check(e.kill_count == 1 && std::strcmp(e.kill[e.kill_head], "beta") == 0, "C-w pushes the kill",
          e.kill[e.kill_head], "beta");
    ln_core_yank(&e);
    expect(e, "alpha beta", 10, "C-y pastes it back");

    ln_core_set(&e, "one two");
    e.pos = 3;
    ln_core_kill_word_forward(&e);
    expect(e, "one ", 4, "M-d kills the word after the cursor");

    ln_core_set(&e, "keep drop");
    ln_core_kill_word_back(&e);
    ln_core_set(&e, "x");
    e.pos = 0;
    ln_core_kill_to_start(&e);
    expect(e, "x", 0, "C-u at column 0 keeps the line");
    e.pos = 1;
    ln_core_kill_to_start(&e);
    expect(e, "", 0, "C-u kills to the line start, not the whole line");
}

static void test_kill_ring_walk() {
    ln_core e;
    ln_core_init(&e);
    ln_core_set(&e, "aa bb");
    ln_core_kill_word_back(&e); /* "bb" */
    ln_core_set(&e, "cc dd");
    ln_core_kill_word_back(&e); /* "dd" */
    ln_core_set(&e, "");
    ln_core_yank(&e);
    expect(e, "dd", 2, "C-y pastes the newest kill");
    ln_core_yank_prev(&e);
    expect(e, "bb", 2, "M-y swaps in the older kill");
    ln_core_yank_prev(&e);
    expect(e, "bb", 2, "M-y stops at the oldest entry");
}

static void test_transpose_words() {
    ln_core e;
    ln_core_init(&e);
    ln_core_set(&e, "one two");
    e.pos = 3;
    ln_core_transpose_words(&e);
    expect(e, "two one", 3, "M-t exchanges the words around the cursor");
    ln_core_set(&e, "one two");
    e.pos = 4;
    ln_core_transpose_words(&e);
    expect(e, "two one", 3, "M-t inside the gap swaps the same pair");
}

static void test_history_search() {
    char* h[] = {(char*)"alpha", (char*)"beta", (char*)"car list", (char*)"gamma"};
    check(ln_core_search_prev(h, 4, 3, "car") == 2, "search finds the newest match",
          std::to_string(ln_core_search_prev(h, 4, 3, "car")).c_str(), "2");
    check(ln_core_search_prev(h, 4, 3, "zzz") == -1, "search misses",
          std::to_string(ln_core_search_prev(h, 4, 3, "zzz")).c_str(), "-1");
    check(ln_core_search_prev(h, 4, 3, "") == -1, "empty query matches nothing",
          std::to_string(ln_core_search_prev(h, 4, 3, "")).c_str(), "-1");
    check(ln_core_search_prev(h, 4, 1, "car") == -1, "search only walks older entries",
          std::to_string(ln_core_search_prev(h, 4, 1, "car")).c_str(), "-1");
    check(ln_core_search_prev(h, 4, 3, "alpha") == 0, "search reaches the oldest entry",
          std::to_string(ln_core_search_prev(h, 4, 3, "alpha")).c_str(), "0");
}

static void test_undo() {
    ln_core e;
    ln_core_init(&e);
    ln_core_set(&e, "alpha");
    ln_core_insert(&e, " beta");
    expect(e, "alpha beta", 10, "insert");
    ln_core_restore_undo(&e);
    expect(e, "alpha", 5, "C-_ restores the last edit");
    ln_core_backspace(&e);
    expect(e, "alph", 4, "backspace");
    ln_core_restore_undo(&e);
    expect(e, "alpha", 5, "C-_ after a backspace restores it");
}

static void test_delete_and_insert_middle() {
    ln_core e;
    ln_core_init(&e);
    ln_core_set(&e, "abc");
    e.pos = 1;
    ln_core_insert(&e, "XY");
    expect(e, "aXYbc", 3, "insert at the cursor");
    ln_core_delete(&e);
    expect(e, "aXYc", 3, "C-d deletes at the cursor");
    e.pos = 2;
    ln_core_backspace(&e);
    expect(e, "aYc", 1, "backspace before the cursor");
}

int main() {
    test_words();
    test_kill_and_yank();
    test_kill_ring_walk();
    test_transpose_words();
    test_history_search();
    test_undo();
    test_delete_and_insert_middle();
    if (failures) {
        std::fprintf(stderr, "%d linenoise core check(s) failed\n", failures);
        return 1;
    }
    std::printf("linenoise core: all checks passed\n");
    return 0;
}
