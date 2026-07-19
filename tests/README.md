# tests/

Layout: `domain/` (preferred) · `suite/` (Aura E2E) · `regression/` · `tasks/` · `fixtures/` · legacy `test_issue_*.cpp` (don't add new).

Run: `./build.py check` · `./build.py test {unit,integ,issues,issues-fast}` · `ninja -C build <domain-suite>`.

See [`docs/contributing.md`](../docs/contributing.md).