# Why these checks stay

`fold_simple_checks.py` moves a script into `../manifests/` when every
assertion is a substring, a path that must not exist, or a glob that must
match nothing. A script stays in this directory when a JSON row cannot
express it: statement order, a count, a regex, an AST walk, or a
subprocess. A fixed character window is not a reason to stay. Scope the
needle with `_source_scan.source_block_after`, or with a manifest
`symbol` (the brace-matched body, not a character budget).

Do not add a name to `../simple_check_grandfather.txt`. A new substring
contract is a manifest. A new filename here must not carry an issue
number.
