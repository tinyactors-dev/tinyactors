# `interpreter.h`

The interpreter advances exactly one resident actor.

It must implement:

- eventless transition selection;
- named-event prefix matching;
- condition evaluation and `error.execution` behavior;
- hierarchical transition lookup from every active atomic leaf;
- SCXML transition priority/conflict removal;
- history capture and restoration;
- ordered exits;
- transition executable content;
- ordered entries, initial/default entry, and parallel-region expansion;
- final/completion event generation;
- internal-event FIFO processing;
- invocation lifecycle and autoforward/finalize semantics;
- resumable budgeting without changing semantic ordering.

The continuation's ordered sets and executable-content frame stack are the
resumption boundary. A budget may stop between semantic operations, but never
inside a host callback or at a point not fully described by that continuation.
