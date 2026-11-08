/* Issue #5041: the line-editing state of the REPL, split out of linenoise.c
 * so word motion, the kill ring, transposition and undo can be tested key by
 * key without a terminal. linenoise keeps the rendering; every mutating key
 * routes its buffer/cursor arithmetic through these helpers.
 *
 * "Word" is a Scheme identifier: letters, digits and - ? ! * / > < = _ .
 */
#ifndef LINENOISE_CORE_H
#define LINENOISE_CORE_H

#include <stddef.h>
#include <string.h>

#define LN_CORE_MAX 4096
#define LN_KILL_SLOTS 16
#define LN_KILL_MAX 512

typedef struct ln_core {
    char buf[LN_CORE_MAX];
    size_t len;
    size_t pos;
    char kill[LN_KILL_SLOTS][LN_KILL_MAX];
    size_t kill_len[LN_KILL_SLOTS];
    int kill_head; /* slot of the newest kill, -1 when empty */
    int kill_count;
    int yank_active; /* a C-y inserted the last yank_len bytes at yank_pos */
    size_t yank_pos;
    size_t yank_len;
    int yank_index;
    char undo[LN_CORE_MAX];
    size_t undo_len;
    size_t undo_pos;
    int has_undo;
} ln_core;

static int ln_core_is_word(int c) {
    if (c >= '0' && c <= '9')
        return 1;
    if (c >= 'a' && c <= 'z')
        return 1;
    if (c >= 'A' && c <= 'Z')
        return 1;
    return c == '-' || c == '?' || c == '!' || c == '*' || c == '/' || c == '>' || c == '<' ||
           c == '=' || c == '_';
}

static void ln_core_init(ln_core* e) {
    memset(e, 0, sizeof *e);
    e->kill_head = -1;
    e->yank_index = -1;
}

static void ln_core_set(ln_core* e, const char* text) {
    size_t n = strlen(text);
    if (n >= LN_CORE_MAX)
        n = LN_CORE_MAX - 1;
    memcpy(e->buf, text, n);
    e->buf[n] = 0;
    e->len = n;
    e->pos = n;
}

/* One undo step: the state before the last buffer edit. */
static void ln_core_snapshot(ln_core* e) {
    memcpy(e->undo, e->buf, e->len);
    e->undo_len = e->len;
    e->undo_pos = e->pos;
    e->has_undo = 1;
}

static void ln_core_restore_undo(ln_core* e) {
    if (!e->has_undo)
        return;
    memcpy(e->buf, e->undo, e->undo_len);
    e->len = e->undo_len;
    e->pos = e->undo_pos < e->len ? e->undo_pos : e->len;
    e->buf[e->len] = 0;
    e->has_undo = 0;
}

static size_t ln_core_word_left(const ln_core* e, size_t pos) {
    if (pos > e->len)
        pos = e->len;
    while (pos > 0 && !ln_core_is_word((unsigned char)e->buf[pos - 1]))
        pos--;
    while (pos > 0 && ln_core_is_word((unsigned char)e->buf[pos - 1]))
        pos--;
    return pos;
}

static size_t ln_core_word_right(const ln_core* e, size_t pos) {
    if (pos > e->len)
        pos = e->len;
    while (pos < e->len && !ln_core_is_word((unsigned char)e->buf[pos]))
        pos++;
    while (pos < e->len && ln_core_is_word((unsigned char)e->buf[pos]))
        pos++;
    return pos;
}

static void ln_core_kill_push(ln_core* e, const char* text, size_t n) {
    if (n == 0)
        return;
    if (n >= LN_KILL_MAX)
        n = LN_KILL_MAX - 1;
    if (e->kill_head >= 0 && e->kill_len[e->kill_head] == n &&
        memcmp(e->kill[e->kill_head], text, n) == 0)
        return; /* readline does not duplicate the newest entry */
    e->kill_head = (e->kill_head + 1) % LN_KILL_SLOTS;
    memcpy(e->kill[e->kill_head], text, n);
    e->kill[e->kill_head][n] = 0;
    e->kill_len[e->kill_head] = n;
    if (e->kill_count < LN_KILL_SLOTS)
        e->kill_count++;
    e->yank_active = 0;
}

static void ln_core_kill_range(ln_core* e, size_t from, size_t to) {
    if (from > to) {
        size_t t = from;
        from = to;
        to = t;
    }
    if (to > e->len)
        to = e->len;
    if (from >= to)
        return;
    ln_core_snapshot(e);
    ln_core_kill_push(e, e->buf + from, to - from);
    memmove(e->buf + from, e->buf + to, e->len - to);
    e->len -= to - from;
    e->buf[e->len] = 0;
    e->pos = from;
}

static void ln_core_kill_to_end(ln_core* e) {
    ln_core_kill_range(e, e->pos, e->len);
}

static void ln_core_kill_to_start(ln_core* e) {
    ln_core_kill_range(e, 0, e->pos);
}

static void ln_core_kill_word_back(ln_core* e) {
    ln_core_kill_range(e, ln_core_word_left(e, e->pos), e->pos);
}

static void ln_core_kill_word_forward(ln_core* e) {
    /* Kill the next word itself: the spaces before it stay. */
    size_t start = e->pos;
    while (start < e->len && !ln_core_is_word((unsigned char)e->buf[start]))
        start++;
    size_t end = start;
    while (end < e->len && ln_core_is_word((unsigned char)e->buf[end]))
        end++;
    ln_core_kill_range(e, start, end);
}

static void ln_core_insert(ln_core* e, const char* text) {
    size_t n = strlen(text);
    if (n == 0 || e->len + n >= LN_CORE_MAX)
        return;
    ln_core_snapshot(e);
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
    memcpy(e->buf + e->pos, text, n);
    e->len += n;
    e->pos += n;
    e->buf[e->len] = 0;
    e->yank_active = 0;
}

static void ln_core_backspace(ln_core* e) {
    if (e->pos == 0)
        return;
    ln_core_snapshot(e);
    memmove(e->buf + e->pos - 1, e->buf + e->pos, e->len - e->pos);
    e->len--;
    e->pos--;
    e->buf[e->len] = 0;
    e->yank_active = 0;
}

static void ln_core_delete(ln_core* e) {
    if (e->pos >= e->len)
        return;
    ln_core_snapshot(e);
    memmove(e->buf + e->pos, e->buf + e->pos + 1, e->len - e->pos - 1);
    e->len--;
    e->buf[e->len] = 0;
    e->yank_active = 0;
}

/* C-y: paste the newest kill at the cursor. */
static void ln_core_yank(ln_core* e) {
    if (e->kill_head < 0 || e->kill_len[e->kill_head] == 0)
        return;
    ln_core_insert(e, e->kill[e->kill_head]);
    e->yank_active = 1;
    e->yank_index = e->kill_head;
    e->yank_pos = e->pos - e->kill_len[e->kill_head];
    e->yank_len = e->kill_len[e->kill_head];
}

/* M-y right after C-y: swap the pasted text for the next older kill. */
static void ln_core_yank_prev(ln_core* e) {
    if (!e->yank_active || e->kill_count < 2)
        return;
    int older = (e->yank_index - 1 + LN_KILL_SLOTS) % LN_KILL_SLOTS;
    if (older < 0 || older >= LN_KILL_SLOTS || e->kill_len[older] == 0)
        return;
    const size_t n = e->kill_len[older];
    if (e->yank_pos + e->yank_len > e->len || n >= LN_CORE_MAX)
        return;
    memmove(e->buf + e->yank_pos + n, e->buf + e->yank_pos + e->yank_len,
            e->len - (e->yank_pos + e->yank_len));
    memcpy(e->buf + e->yank_pos, e->kill[older], n);
    e->len = e->len - e->yank_len + n;
    e->buf[e->len] = 0;
    e->pos = e->yank_pos + n;
    e->yank_len = n;
    e->yank_index = older;
}

/* M-t: exchange the word before the cursor with the word after it. */
static void ln_core_transpose_words(ln_core* e) {
    /* Swap the word before the cursor with the word after it, keeping the
     * text between them. */
    const size_t p = e->pos;
    size_t a_end = p;
    while (a_end > 0 && !ln_core_is_word((unsigned char)e->buf[a_end - 1]))
        a_end--;
    size_t a_start = a_end;
    while (a_start > 0 && ln_core_is_word((unsigned char)e->buf[a_start - 1]))
        a_start--;
    size_t b_start = p;
    while (b_start < e->len && !ln_core_is_word((unsigned char)e->buf[b_start]))
        b_start++;
    size_t b_end = b_start;
    while (b_end < e->len && ln_core_is_word((unsigned char)e->buf[b_end]))
        b_end++;
    if (a_start == a_end || b_start == b_end)
        return;
    const size_t alen = a_end - a_start, blen = b_end - b_start;
    const size_t gaplen = b_start - a_end;
    if (alen >= LN_KILL_MAX || blen >= LN_KILL_MAX || gaplen + blen + alen >= LN_CORE_MAX)
        return;
    char a[LN_KILL_MAX], b[LN_KILL_MAX], gap[LN_KILL_MAX];
    memcpy(a, e->buf + a_start, alen);
    memcpy(b, e->buf + b_start, blen);
    memcpy(gap, e->buf + a_end, gaplen);
    ln_core_snapshot(e);
    memcpy(e->buf + a_start, b, blen);
    memcpy(e->buf + a_start + blen, gap, gaplen);
    memcpy(e->buf + a_start + blen + gaplen, a, alen);
    e->pos = a_start + blen;
    e->buf[e->len] = 0;
}

#endif
