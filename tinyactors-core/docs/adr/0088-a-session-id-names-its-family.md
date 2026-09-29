# ADR 0088: A session ID names its family

**Status:** Accepted and implemented (2026-09-29); see the implementation notes. Would amend ADR 0066 (a session ID's text carries its system's incarnation and its family), ADR 0065 (a runtime reference names its family) and ADR 0044 (the session URI's grammar). ADR 0089 depends on it.
**Date:** 2026-09-29

## Context

Every actor has a family: the family of its definition (ADR 0077). A named
actor is addressed as `#_actor_<family>/<name>` (ADR 0037, ADR 0044), and
a host brings it back through its family. napi's loader is per family
(napi ADR 0020): when mail arrives for a name that is not resident, the
family's loader decides what happens. It can bring the actor back, start
a new instance, or answer `not-found`, and the mail waits while it
decides. Because loading an actor of a family can simply start a new
instance, the family's chart has to be written for that case. That is
intended. granary relies on it: a message for an `issue/<id>` actor that
has gone creates a new instance, and the chart handles the message in its
initial state.

An actor without a name is addressed by its session: `#_scxml_<session
id>` is its origin, and so the target of every reply (SCXML C.1, ADR
0066). Its session ID is the runtime ID it was spawned with,
`generation << 32 | slot` (ADR 0059, ADR 0066), written in decimal. Once
the actor is gone, that number says nothing about its family:
- napi's router can only look the number up (`tna_router_unreachable`,
  `src/router.c`). A missing or finished session is ERROR, which raises
  `error.communication` in the sender (napi ADR 0022).
- The family's loader is never asked, and could not be: core keeps a
  session in the session table past residency only for a named actor
  (ADR 0066), and the origin is a bare `ta_actor_ref { RUNTIME, id }`.

The number is not unique over time either:
- runtime IDs start again in every system;
- a slot's generation wraps to 1 after 2³² reuses (ADR 0059).

So a session URI written into a log or into data by one process can name
an unrelated live actor in a later process.

This matters for replay (ADR 0089). A persisted actor replies to an
anonymous requester in the macrostep that received the request. If the
requester has finished, the router answers ERROR synchronously, and the
persisted actor's state depends on when another actor finished. Nothing
in its own history records that. If a session is addressed through its
family, a family with a loader never refuses synchronously: its loader
decides, and a failure is a dead letter (napi ADR 0022). Mail is then
refused synchronously only for sessions whose family has no loader, and
for sessions that do not exist. SCXML requires those refusals to happen
in the macrostep that sends (C.1, IRP tests 496 and 521).

SCXML's `_sessionid` is a string. Nothing requires it to be a number, only
that it identifies the session and that `#_scxml_<sessionid>` reaches it
(5.10, C.1). So the text can carry what the number cannot.

## Decision

### 1. A session ID's text is incarnation, runtime ID and family

```
<incarnation>.<runtime id>.<family>
```

- `<runtime id>` is the session's numeric ID (ADR 0066), in decimal.
- `<incarnation>` is the system's incarnation (section 3), in decimal.
- `<family>` is the session's definition family, and comes last, so it
  may contain `.` and `/`. An invoked child's family is
  `<parent's family>/invoke` (napi `docs/scxml.md`).

This is the text of `_sessionid` for **every** session, named or not, and
the numbers stay numbers everywhere else. `ta_session_id`,
`ta_actor_inspection.session_id`, session images and the session table
are unchanged. Only the text changes.

Core renders the text in one place. `ta_session_id_format()` takes the
session, its family and a buffer in one parameter struct, and hosts use it wherever they
show a session ID: napi's `sessionID`, OTLP's `scxml.session_id`, and the
test host's DAP transcripts. A record or reference that names a session
also names its family (section 2, and the trace record's definition), so
a host can always render the text.

### 2. The origin, the location and references name the family

- The origin of an actor without a name, and the SCXML processor's
  `location` in its `_ioprocessors`, are `#_scxml_<sessionid>` with the
  new text. So C.1's rule that the origin match the location holds. The
  location is still `'#_scxml_' + _sessionid`, which is how SCXML
  documents, and IRP tests 190 and 350, build it.
- A named actor's origin stays its address (ADR 0065).
- `ta_actor_ref` gains `ta_atom family`, set on every runtime reference
  core makes for an actor: its `self`, and origins. It fills the four bytes
  of padding after `kind`, so the struct stays 16 bytes.
- The family atom is interned once per definition, when the definition is
  registered.

No name is reserved, and no address form is added.

### 3. A system has an incarnation

`ta_system_config` gains `uint64_t incarnation`, chosen by the host to be
distinct from the systems and processes before it: random bytes, or a
restart counter. Tests use 0.

### 4. Resolving a session URI

`ta_scxml_target_parse()` reads `#_scxml_<i>.<n>.<family>` into a runtime
reference whose family is the family named (interned, as a named address
is). When `<i>` is this system's incarnation, the reference names session
`<n>`. When it is another incarnation, the session belonged to another
system, so the reference names session 0, which is none.

A bare `#_scxml_<n>` still parses, as today, into a reference to session
`<n>` with no family (atom 0).

The router receives the reference, with its family, and routes it:
- **the session is resident, has not finished, and is of that family:** the
  mail goes to it, as today;
- **otherwise, if the family has a loader:** the mail goes to the loader, as
  a request for an instance of the family. The loader can start a new
  instance, or answer `not-found`, which is a dead letter. A new instance
  is a new session, so each stale message to a gone session is its own
  request;
- **otherwise, including a reference with no family:** ERROR, as today
  (napi ADR 0022). **A family with no loader keeps refusing.** Mail to a
  gone session raises `error.communication` in the sender, synchronously.

A family that wants all mail for a gone actor to reach one instance names
its actors.

## Consequences

- A reply to an actor that has gone goes to that actor's family, as mail to
  a named actor does. With a loader, the router never refuses mail to a
  family synchronously. ADR 0089 relies on this.
- A session URI in a log or in stored data names its system and family, so
  it never reaches an unrelated actor in a later process or after a
  generation wraps.
- `_sessionid` text changes for every session, and so do the origin and
  location text of actors without a name. These change with it:
  - napi's `sessionID`, its OTLP attributes, and its tests;
  - the test host's DAP transcripts and scenarios;
  - any chart that parses `_sessionid` as a number.
- The router's contract gains the family on a runtime destination.
  napi's `tna_router_unreachable` asks the family's loader for a gone
  session. Its loader request gains a form for "an instance of this
  family" with no name. Both changes belong to a napi ADR.
- Costs:
  - per actor: 0 bytes. The family fills padding in `ta_actor_ref`, and the
    session table is unchanged;
  - per definition: one atom;
  - per system: the 8-byte incarnation;
  - per event: 0 bytes;
  - rendering a session ID writes the incarnation and family as well: tens
    of bytes more text where `_sessionid`, an origin or a trace attribute
    is rendered;
  - the image and offload formats are unchanged. An offloaded reference
    copies the whole struct, and images are valid only in their own process
    (ADR 0074).

## Considered and rejected

- **A generated name in the family, `#_actor_<family>/~<incarnation>.<session id>`**
  (this ADR's first draft). It needed a reserved name prefix and a second
  address form for unnamed actors, and it broke the location's
  `'#_scxml_' + _sessionid` shape. It also could not address an invoked
  child: `#_actor_<family>/<name>` splits at the first `/`, and a child's
  family contains one.
- **Encoding the family or incarnation in the number.** The 64 bits are
  already taken by slot and generation (ADR 0059).
- **A default loader for every family.** A family without a loader answers
  ERROR, as today. Starting a new instance is the family's choice, made by
  installing a loader.

## Implementation notes

- **Where:** `include/tinyactors.h` (`ta_actor_ref.family`,
  `ta_system_config.incarnation`, `ta_session_id_format()`,
  `ta_system_incarnation()`), `src/scxml_uri.{c,h}` (the text's parser and
  writer, `ta_session_ref_write`, the session URI's parse),
  `src/datamodel.c` (`_sessionid`), `src/actor.{c,h}` (`self` names its
  family; `ta_actor_family_of`, `ta_actor_runtime_ref`; re-armed pending
  sends), `src/interpreter.c` and `src/invoke.c` (`#_parent`, `#_<invokeid>`,
  autoforwarding and a child's completion name their destination's family),
  `src/session.c` (`ta_session_route`), `src/atom.c`, `src/system.{c,h}`,
  `src/definition.h`, `src/builder.c`.
- **Atom 0 is the empty text (not in the decision).** Section 2 makes a
  runtime reference name its family by atom, and every host builds
  references with designated initializers, so a zeroed `family` must mean
  "unknown". The atom table now interns the empty text first, at index 0,
  and no family is empty (a chart without a family does not compile), so 0
  names none. Every other atom moved up by one; nothing depended on their
  numbers.
- **The family atom already existed.** The compiler interned the family as
  `ta_definition.name`; it is renamed `family`. No atom is added per
  definition.
- **Core checks the family when it routes (section 4 put it in the
  router).** `ta_session_route()`, which every SCXML submission goes
  through, gives a runtime destination whose ID another family's actor
  holds ID 0, keeping the family, so a router sees "gone" without
  comparing families itself. napi's and the test host's routers are
  unchanged: resident and not finished is delivered, anything else is
  ERROR. Asking a family's loader for a gone session is napi ADR 0032.
- **What a reference with no family writes.** A runtime reference with
  family 0 whose actor is alive writes the actor's family; one whose actor
  is gone (a bare `#_scxml_<n>` a document wrote) writes the bare number,
  as before. A reference parsed from another incarnation is session 0 with
  its family, and writes as this system's `<incarnation>.0.<family>`: the
  other incarnation is not kept.
- **Pending sends a host re-arms (core ADR 0074 section 8)** must carry a
  valid atom as a runtime destination's family (`TA_ERR_INVALID`
  otherwise); a send to the new session itself (runtime ID 0) takes its
  family. napi rebuilds destinations from `ActorID`s and addresses, so its
  re-armed references carry family 0.
- **Hosts, migrated in the same change:** the test host's scenarios compare
  `Actor#session_text` / `#session_uri` (built with `ta_session_id_format`
  through the generated bindings), and its debug adapter's `_sessionid`,
  `$_sessionid` and `_ioprocessors` use the text. napi's debug adapter
  (`_sessionid` in the System scope, `sessionID` in evaluated expressions)
  uses `sessionText()` in `lib/system.js`, which calls the formatter; its
  tests expect the text, with incarnation 0, which napi passes until napi
  ADR 0032 adds `createSystem({ incarnation })`. **Not migrated here:**
  napi's OTLP `scxml.session_id` and `scxml.cause.session_id` stay decimal,
  and `ActorInspection.sessionID` stays the number. Both are napi ADR 0032
  section 1; a cause's family is not in the trace record, so OTLP needs a
  decision of its own there.
- **Tests:** `tests/scxml_uri_test.c` (a session's text parses with its
  family, a family holding `.` and `/`, another incarnation's session is
  session 0, text that is not the form is inaccessible, the bare form names
  no family, `ta_session_id_format()` and its refusals, the text
  round-trips); `tests/session_id_test.c` (an unnamed actor's `self` names
  its family, its URI and `ta_session_id_format()` agree, another family's
  reference routes to session 0, and once the actor is destroyed its
  reference still writes the same text, which parses back to it).
- **Costs:** per actor 0 bytes: `ta_actor_ref` is 16 bytes before and after
  (a static assertion in `src/actor.c` checks it), and `ta_actor` is
  unchanged. Per system 8 bytes for the incarnation and one atom entry (24
  bytes on a 64-bit host) for the empty text. Per definition and per event 0 bytes. The
  image and offload formats are unchanged. Rendering a session ID writes
  the incarnation and family as well (`0.4294967296.messenger` in place of
  `4294967296`).
- **A step key names its family (2026-09-29, with napi ADR 0032 section
  1).** Section 1's "a record or reference that names a session also names
  its family" did not hold for a trace record's `cause`, whose session may be
  another family's and gone. `ta_step_key` gained `ta_atom family` (in its
  padding: 24 bytes before and after, static-asserted), set by
  `ta_actor_step()`, `ta_trace_lifecycle_key()` and the RAISED record's key;
  a host's cause and a definition record keep 0. It is not part of a span
  ID. `tests/session_id_test.c` (`test_a_step_key_names_its_family`) checks
  every record's key and an inspection's step and cause.
