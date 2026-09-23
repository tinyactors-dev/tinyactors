# Architecture decision records: tinyactors-napi

These ADRs record the design of the Node-API addon. The addon is being
rewritten from scratch against them. Where `../design.md` or
`../../ROADMAP.md` disagree with an ADR, the ADR wins.

Core decisions live in `tinyactors-core/docs/adr`. This addon depends in
particular on core ADRs 0011, 0029, 0040, 0047, 0050 and 0053–0057 and 0059,
and on a core ADR not yet written that gives `ta_system_visit_actors` a
cursor (ADR 0003).

| ADR | Title |
|---|---|
| [0001](0001-core-interprets-the-addon-hosts-javascript-scripts.md) | [Core interprets, the addon hosts, JavaScript scripts](0001-core-interprets-the-addon-hosts-javascript-scripts.md) |
| [0002](0002-actors-are-ids-handles-are-plain-javascript.md) | [Actors are IDs; handles are plain JavaScript](0002-actors-are-ids-handles-are-plain-javascript.md) |
| [0003](0003-singular-calls-are-primary-enumeration-is-paged.md) | [Singular calls are primary; enumeration is paged](0003-singular-calls-are-primary-enumeration-is-paged.md) |
| [0004](0004-event-names-are-interned-once.md) | [Event names are interned once](0004-event-names-are-interned-once.md) |
| [0005](0005-per-target-mailboxes-and-a-ready-queue.md) | [Per-target mailboxes and a ready queue](0005-per-target-mailboxes-and-a-ready-queue.md) |
| [0006](0006-the-addon-routes-actor-to-actor-events.md) | [The addon routes actor-to-actor events](0006-the-addon-routes-actor-to-actor-events.md) |
| [0007](0007-the-event-loop-drives-a-bounded-pump.md) | [The event loop drives a bounded pump](0007-the-event-loop-drives-a-bounded-pump.md) |
| [0008](0008-faults-are-per-actor.md) | [Faults are per actor](0008-faults-are-per-actor.md) |
| [0009](0009-one-native-toolkit-for-memory-and-containers.md) | [One native toolkit for memory and containers](0009-one-native-toolkit-for-memory-and-containers.md) |
| [0010](0010-definitions-are-normalized-in-javascript-and-built-in-one-arena.md) | [Definitions are normalized in JavaScript and built in one arena](0010-definitions-are-normalized-in-javascript-and-built-in-one-arena.md) (superseded by 0013) |
| [0011](0011-javascript-values-cross-as-reference-counted-boxes.md) | [JavaScript values cross as reference-counted boxes](0011-javascript-values-cross-as-reference-counted-boxes.md) |
| [0012](0012-lifecycle-close-is-authoritative-finished-actors-are-reclaimed.md) | [Lifecycle: close is authoritative, finished actors are reclaimed](0012-lifecycle-close-is-authoritative-finished-actors-are-reclaimed.md) |
| [0013](0013-the-control-plane-is-generated-the-plumbing-is-hand-written.md) | [The control plane is generated; the plumbing is hand-written](0013-the-control-plane-is-generated-the-plumbing-is-hand-written.md) |
| [0014](0014-memory-is-pooled-by-lifetime.md) | [Memory is pooled by lifetime](0014-memory-is-pooled-by-lifetime.md) |
