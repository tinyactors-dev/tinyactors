# Embedder manual

TA is an in-process actor runtime built around hierarchical statecharts. The library owns statechart semantics. **You own the process:** threads, event loops, storage, clocks, network I/O, and when execution receives CPU time.

The smallest useful mental model is:

- a **definition** is an immutable statechart shared by many actors;
- an **actor** is one running statechart session;
- an **event** is delivered to one actor;
- `ta_actor_resume()` advances that actor for a caller-supplied execution budget;
- data models, I/O processors, and invoked services are host-provided interfaces;
- logical time is supplied by the embedder;
- named actors can be unloaded and restored without changing their statechart-visible behavior.

Read `concepts.md`, then `embedding.md`. The ADRs contain the normative architectural rationale.
