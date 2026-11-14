#!/usr/bin/env python3
"""Copy an asciinema v2 cast, dropping only the trailing tmux teardown.

The recorder detaches the tmux client at the end; tmux then leaves the
alternate screen and prints "[detached ...]". Everything from the first
event containing the alt-screen exit (ESC[?1049l) onward is dropped, and
the last kept frame is held for 4 s. All earlier events are byte-identical.
"""

import json
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, encoding="utf-8") as f:
    lines = f.read().splitlines()
out = [lines[0]]
last_t = 0.0
for line in lines[1:]:
    t, kind, data = json.loads(line)
    if kind == "o" and "\x1b[?1049l" in data:
        break
    out.append(line)
    last_t = t
out.append(json.dumps([last_t + 4.0, "o", ""]))
with open(dst, "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
print(f"kept {len(out) - 2} of {len(lines) - 1} events; cut at {last_t:.2f}s")
