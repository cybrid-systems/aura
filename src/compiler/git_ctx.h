// git_ctx.h — In-process Git operations via libgit2
//
// Replaces popen("git xxx") calls with direct libgit2 API calls. Much faster
// (no fork+exec per query) and safer (no shell escape pitfalls).
//
// Falls back to popen when libgit2 is not available at build time
// (controlled by AURA_HAVE_LIBGIT2 macro).

#pragma once

#include <string>
#include <vector>

#ifdef AURA_HAVE_LIBGIT2
#include <git2.h>
#endif

namespace aura::compiler {

// GitContext — owns a libgit2 repository handle for the current working dir.
// Thread-safety: libgit2 repositories are not thread-safe; one per thread.
class GitContext {
public:
    GitContext();
    ~GitContext();

    GitContext(const GitContext&) = delete;
    GitContext& operator=(const GitContext&) = delete;

    // Returns true if the repo is open and ready.
    bool is_open() const {
#ifdef AURA_HAVE_LIBGIT2
        return repo_ != nullptr;
#else
        return false;
#endif
    }

    // ── Read operations (return formatted text) ──────────────
    // Output formats match the previous popen-based output so the
    // existing Aura tests don't need updates.

    // Short status: "XY filename" per line, like "git status --short".
    // Returns empty string on error.
    std::string status_short() const;

    // Unified diff of working dir vs index (unstaged). If staged=true,
    // diff of index vs HEAD.
    // Returns "" on error or no diff.
    std::string diff(bool staged) const;

    // Last n commits, one-line format: "<sha> <subject>", like "git log --oneline -n".
    // n must be in [1, 1000].
    std::string log_oneline(int n) const;

    // Current branch name (e.g., "main"), or "" if detached.
    std::string branch_current() const;

    // Current HEAD short SHA (e.g., "abc1234").
    std::string rev_parse_short() const;

    // ── Write operations (return 0 on success, non-zero on error) ──

    // Commit the current index with a message.
    // Returns 0 on success.
    int commit(const std::string& message);

    // Stage files. Returns 0 on success.
    int stage(const std::vector<std::string>& paths);

private:
#ifdef AURA_HAVE_LIBGIT2
    git_repository* repo_ = nullptr;
#endif
};

} // namespace aura::compiler
