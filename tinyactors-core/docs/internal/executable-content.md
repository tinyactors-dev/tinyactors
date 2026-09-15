# Executable content

TA needs an internal executable-content IR rather than XML-shaped runtime objects.

The IR must be capable of representing the SCXML execution semantics for:

- raise/internal event;
- conditional branches;
- foreach/iteration as supported by the selected data model;
- assignment;
- script/data-model execution;
- log or equivalent host-visible diagnostic action;
- send and delayed send;
- cancel;
- invocation-related finalize content;
- extension actions.

Executable content can pause because actor execution is budgeted. Therefore nested constructs require resumable frames/program counters that become part of the actor continuation.

If one executable-content element raises an SCXML execution error, the interpreter must follow the standard's block-level error behavior rather than merely queueing an error and blindly executing the remainder of the same block.

Extensions may perform arbitrary host/data work but cannot directly mutate the active state configuration; state changes occur through transitions.
