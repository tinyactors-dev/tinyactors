# Executable content

Tinyactors needs an internal executable-content IR rather than XML-shaped runtime objects.

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

A block is the content of one `<onentry>`, `<onexit>`, `<transition>`, `<initial>` transition, `<finalize>`, or top-level `<script>` (SCXML 4.9). A state can have several `<onentry>` and `<onexit>` handlers, and each one is a separate block (3.8, 3.9). The public spec therefore keeps them apart (`ta_scxml_handler_spec`), and the compiled definition stores one action block per handler (`ta_definition.handlers`). The interpreter runs a state's handlers in document order, each with its own error boundary.

Extensions may perform arbitrary host/data work but cannot directly mutate the active state configuration; state changes occur through transitions.
