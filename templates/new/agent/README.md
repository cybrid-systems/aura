# {{name}}

Two roles in an orch workflow: `workflow.aura` holds the proposer and the
checker, `main.aura` wires them stage by stage and prints the dialogue. The
checker's rejected definition never reaches the live AST.

    aura run
    aura test
