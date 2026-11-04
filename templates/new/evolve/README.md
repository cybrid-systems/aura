# {{name}}

A program that evolves itself: each step proposes new `params`, rewrites the
live AST with `mutate:rebind`, keeps the change only if `gate.aura` says it is
better, and otherwise rolls back with `ast:restore`.

    aura run     # writes out/report.html and out/audit.tsv
    aura test
