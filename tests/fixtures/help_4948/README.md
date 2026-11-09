# help_4948/

Golden stdout for `tests/python/test_issue_4948_help.py` (#4948): one file
per v0.2 module `(xxx:help)` page and per CLI `--help` surface that
introduces them.

Regenerate after an intentional help change, then review the diff:

    AURA_UPDATE_HELP_SNAPSHOTS=1 python3 tests/python/test_issue_4948_help.py

The first line of `cli-top.txt` is normalised to `aura <version>` because
the banner carries the build version (dev / nightly / X.Y.Z).
