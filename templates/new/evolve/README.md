# {{name}}

A program that evolves itself: each step proposes a new definition, rewrites
the live AST with `mutate:rebind`, keeps the change only if `gate.aura` says
it is better, and otherwise rolls back with `mutate :restore`. The terminal shows
each KEEP and ROLLBACK with the live source after the decision.

    aura run
    aura test
