# tests/

Layout: `domain/` (preferred) · `suite/` (Aura E2E) · `regression/` · `tasks/` · `fixtures/` · legacy `test_issue_*.cpp` (don't add new).

Run: `./build.py check` · `./build.py test {unit,integ,issues,issues-fast}` · `ninja -C build <domain-suite>`.

Legacy issue-test inventory & domain migration roadmap (#1957):
[`legacy_test_inventory.md`](legacy_test_inventory.md) — regenerate with
`python3 scripts/inventory_legacy_tests.py`.

See [`docs/contributing.md`](../docs/contributing.md).