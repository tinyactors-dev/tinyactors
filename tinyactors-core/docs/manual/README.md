# Embedder manual

Tinyactors is an in-process actor runtime built around hierarchical statecharts. The library owns statechart/session semantics. **You own the process:** threads, event loops, application objects, storage, clocks, network I/O, and when execution receives CPU time.

The smallest useful mental model is:

- a **definition** is one immutable statechart revision shared by many actors;
- an **actor** is one running statechart session, optionally reachable by a logical named address;
- an **event** is delivered to one actor;
- `ta_actor_resume()` advances that actor for a caller-supplied execution budget;
- data models, I/O processors, and invoked services are host-provided interfaces;
- the host data-model object graph remains host-owned;
- every resident actor can be inspected;
- `ta_actor_capture()` / `ta_actor_reconstruct()` operate on Tinyactors interpreter session images, not serialized application objects;
- portable durability is optional and layers on top of session images.

Read `concepts.md`, `embedding.md`, `inspection.md`, `persistence.md`, and
`virtual-actors.md`. The ADRs contain the normative architectural rationale.
