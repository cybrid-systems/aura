// git_ctx.cpp — libgit2-backed Git operations

#include "git_ctx.h"


import std;
#ifdef AURA_HAVE_LIBGIT2
#include <git2/diff.h>
#include <git2/commit.h>
#include <git2/branch.h>
#include <git2/index.h>
#include <git2/refs.h>
#include <git2/revwalk.h>
#include <git2/strarray.h>
#include <git2/errors.h>
#include <git2/pathspec.h>
#endif

namespace aura::compiler {

namespace {

#ifdef AURA_HAVE_LIBGIT2
    bool ensure_repo_open(git_repository** repo) {
        if (*repo)
            return true;
        return git_repository_open(repo, ".") == 0;
    }
#endif

} // namespace

GitContext::GitContext() {
#ifdef AURA_HAVE_LIBGIT2
    (void)ensure_repo_open(&repo_);
#endif
}

GitContext::~GitContext() {
#ifdef AURA_HAVE_LIBGIT2
    if (repo_) {
        git_repository_free(repo_);
        repo_ = nullptr;
    }
#endif
}

#ifdef AURA_HAVE_LIBGIT2

std::string GitContext::status_short() const {
    if (!repo_)
        return "";
    git_status_list* status = nullptr;
    if (git_status_list_new(&status, repo_, nullptr) != 0)
        return "";
    std::string out;
    size_t count = git_status_list_entrycount(status);
    for (size_t i = 0; i < count; ++i) {
        const git_status_entry* e = git_status_byindex(status, i);
        if (e->status == GIT_STATUS_CURRENT)
            continue;

        char ix = '?', ix2 = ' ';
        if (e->status & GIT_STATUS_INDEX_NEW)
            ix = 'A';
        else if (e->status & GIT_STATUS_INDEX_MODIFIED)
            ix = 'M';
        else if (e->status & GIT_STATUS_INDEX_DELETED)
            ix = 'D';
        else if (e->status & GIT_STATUS_INDEX_RENAMED)
            ix = 'R';
        else if (e->status & GIT_STATUS_INDEX_TYPECHANGE)
            ix = 'T';

        char wt = '?', wt2 = ' ';
        if (e->status & GIT_STATUS_WT_NEW)
            wt = '?';
        else if (e->status & GIT_STATUS_WT_MODIFIED)
            wt = 'M';
        else if (e->status & GIT_STATUS_WT_DELETED)
            wt = 'D';
        else if (e->status & GIT_STATUS_WT_RENAMED)
            wt = 'R';
        else if (e->status & GIT_STATUS_WT_TYPECHANGE)
            wt = 'T';
        else if (e->status & GIT_STATUS_WT_UNREADABLE)
            wt = '!';

        const char* path =
            e->head_to_index ? e->head_to_index->old_file.path : e->index_to_workdir->old_file.path;
        if (!path)
            path = "?";

        char buf[1024];
        std::snprintf(buf, sizeof(buf), "%c%c %c%c %s\n", ix, ix2, wt, wt2, path);
        out += buf;
    }
    git_status_list_free(status);
    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

std::string GitContext::diff(bool staged) const {
    if (!repo_)
        return "";
    git_diff* d = nullptr;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.context_lines = 3;
    opts.interhunk_lines = 0;

    if (staged) {
        git_tree* head_tree = nullptr;
        git_reference* head_ref = nullptr;
        if (git_repository_head(&head_ref, repo_) == 0) {
            git_object* head_obj = nullptr;
            if (git_reference_peel(&head_obj, head_ref, GIT_OBJECT_COMMIT) == 0) {
                git_commit* head_commit = nullptr;
                if (git_commit_lookup(&head_commit, repo_, git_object_id(head_obj)) == 0) {
                    git_commit_tree(&head_tree, head_commit);
                    git_commit_free(head_commit);
                }
                git_object_free(head_obj);
            }
            git_reference_free(head_ref);
        }
        git_index* index = nullptr;
        git_repository_index(&index, repo_);
        if (index) {
            git_diff_tree_to_index(&d, repo_, head_tree, index, &opts);
            git_index_free(index);
        }
        if (head_tree)
            git_tree_free(head_tree);
    } else {
        git_diff_index_to_workdir(&d, repo_, nullptr, &opts);
    }

    if (!d)
        return "";

    std::string out;
    size_t count = git_diff_num_deltas(d);
    for (size_t i = 0; i < count; ++i) {
        git_patch* patch = nullptr;
        if (git_patch_from_diff(&patch, d, i) == 0) {
            git_buf buf{};
            if (git_patch_to_buf(&buf, patch) == 0 && buf.ptr) {
                out.append(buf.ptr, buf.size);
                git_buf_dispose(&buf);
            }
            git_patch_free(patch);
        }
    }
    git_diff_free(d);
    return out;
}

std::string GitContext::log_oneline(int n) const {
    if (!repo_ || n < 1)
        return "";
    if (n > 1000)
        n = 1000;

    git_revwalk* walker = nullptr;
    if (git_revwalk_new(&walker, repo_) != 0)
        return "";
    git_revwalk_sorting(walker, GIT_SORT_TIME);
    git_revwalk_push_head(walker);

    std::string out;
    git_oid oid;
    int count = 0;
    while (count < n && git_revwalk_next(&oid, walker) == 0) {
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, repo_, &oid) == 0) {
            const char* msg = git_commit_message(commit);
            std::string subject = msg ? msg : "";
            auto nl = subject.find('\n');
            if (nl != std::string::npos)
                subject = subject.substr(0, nl);

            char short_oid[GIT_OID_HEXSZ + 1] = {0};
            git_oid_tostr(short_oid, 8, &oid);

            out += short_oid;
            out += ' ';
            out += subject;
            out += '\n';
            git_commit_free(commit);
        }
        ++count;
    }
    git_revwalk_free(walker);

    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

std::string GitContext::branch_current() const {
    if (!repo_)
        return "";
    git_reference* head = nullptr;
    if (git_repository_head(&head, repo_) != 0)
        return "";
    const char* name = git_reference_shorthand(head);
    std::string result = name ? name : "";
    git_reference_free(head);
    return result;
}

std::string GitContext::rev_parse_short() const {
    if (!repo_)
        return "";
    git_object* head_obj = nullptr;
    if (git_revparse_single(&head_obj, repo_, "HEAD") != 0)
        return "";
    char buf[GIT_OID_HEXSZ + 1] = {0};
    git_oid_tostr(buf, 8, git_object_id(head_obj));
    git_object_free(head_obj);
    return std::string(buf);
}

int GitContext::commit(const std::string& message) {
    if (!repo_)
        return -1;
    git_index* index = nullptr;
    git_oid tree_oid, commit_oid;
    git_tree* tree = nullptr;
    git_object* head_obj = nullptr;
    git_commit* parent = nullptr;
    git_signature* sig = nullptr;
    git_reference* head_ref = nullptr;
    int err = 0;

    do {
        if (git_repository_index(&index, repo_) != 0) {
            err = -1;
            break;
        }
        if (git_index_write_tree(&tree_oid, index) != 0) {
            err = -1;
            break;
        }
        if (git_tree_lookup(&tree, repo_, &tree_oid) != 0) {
            err = -1;
            break;
        }

        if (git_repository_head(&head_ref, repo_) == 0) {
            git_reference_peel(&head_obj, head_ref, GIT_OBJECT_COMMIT);
            git_commit_lookup(&parent, repo_, git_object_id(head_obj));
        }

        if (git_signature_default(&sig, repo_) != 0) {
            err = -1;
            break;
        }

        const git_commit* parents[1] = {parent};
        err = git_commit_create(&commit_oid, repo_, "HEAD", sig, sig, "UTF-8", message.c_str(),
                                tree, parent ? 1 : 0, parents);
    } while (false);

    if (sig)
        git_signature_free(sig);
    if (parent)
        git_commit_free(parent);
    if (head_obj)
        git_object_free(head_obj);
    if (head_ref)
        git_reference_free(head_ref);
    if (tree)
        git_tree_free(tree);
    if (index)
        git_index_free(index);
    return err ? -1 : 0;
}

int GitContext::stage(const std::vector<std::string>& paths) {
    if (!repo_ || paths.empty())
        return -1;
    git_index* index = nullptr;
    int err = 0;

    do {
        if (git_repository_index(&index, repo_) != 0) {
            err = -1;
            break;
        }
        // Build strarray from paths (libgit2 takes char**)
        std::vector<std::string> path_storage = paths; // keep alive
        std::vector<char*> path_ptrs;
        for (auto& p : path_storage)
            path_ptrs.push_back(p.data());
        git_strarray paths_arr;
        paths_arr.strings = path_ptrs.data();
        paths_arr.count = path_ptrs.size();

        err = git_index_add_all(index, &paths_arr, 0, nullptr, nullptr);
        git_index_write(index);
    } while (false);

    if (index)
        git_index_free(index);
    return err ? -1 : 0;
}

#else // !AURA_HAVE_LIBGIT2

std::string GitContext::status_short() const {
    return "";
}
std::string GitContext::diff(bool) const {
    return "";
}
std::string GitContext::log_oneline(int) const {
    return "";
}
std::string GitContext::branch_current() const {
    return "";
}
std::string GitContext::rev_parse_short() const {
    return "";
}
int GitContext::commit(const std::string&) {
    return -1;
}
int GitContext::stage(const std::vector<std::string>&) {
    return -1;
}

#endif // AURA_HAVE_LIBGIT2

} // namespace aura::compiler
