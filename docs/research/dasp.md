# DASP: a session protocol for clients of durable actors, read for a tinyactors web app

**Date:** 2026-09-26
**Used by:** nothing yet. Related: [Durable Streams](durable-streams.md), [replay persistence](../proposals/replay-persistence.md), [distributed systems](../proposals/distributed-systems.md)
**Source:** the [DASP repository](https://github.com/dasp-protocol/dasp) at commit `980f806` (2026-09-25): `docs/specification/*.md`, `docs/guide/*.md`, `conformance/behavioral-cases.md`. Requirement IDs below (`DASP-CORE-004`) are the spec's own. The published pages are at <https://dasp-protocol.github.io/dasp/specification>.

The question: a web app built on tinyactors has browsers, other agents
and operators that talk to the same named actor, over connections that
drop. DASP (Durable Actor Session Protocol), by Mike Hostetler for the
Jido Coding Harness (Elixir), says what such clients and the server
must agree on: command admission, retries after a lost reply, saved
outcomes, ordered history and recovery. This note records what the
draft says and how its parts line up with tinyactors. It decides
nothing.

## Status

- **draft-01, "a working review draft. The core can change."** No
  transport binding, no production profile, and no host is released.
- **Young.** 17 commits; the oldest is from 2026-09-25, the day before
  this note.
- **No license yet.** `docs/project/about.md`: "The project license is
  being selected before the first specification release."
  `clients/typescript/package.json` says `UNLICENSED`.
- **Clients only.** Experimental Elixir (built on Jido Signal) and
  TypeScript clients. The conformance runtime cases are "specified, not
  executed"; there is no host harness.

## Summary

| DASP concept | Closest tinyactors piece | Fit |
|---|---|---|
| Actor: a logical target, not a process (MODEL-001) | a named address `(family, name)` (core ADRs 0037, 0065) | **same idea** |
| Host authority, can span processes (MODEL-001) | a napi system, or several processes sharing a log | ours is one process today |
| Command admission, saved before the receipt (CORE-004) | `ta_actor_deliver` is the admission test (core ADR 0055); napi's `send({ until: "accepted" })` | admission exists; **saving it is new** |
| Retry with the same command ID returns `duplicate` (CORE-002, CORE-003) | nothing | **new**: a command record per host |
| One immutable outcome per command: completed, failed, cancelled, uncertain (MSG-006, CORE-006) | `send({ until: "completed" })` ends with a macrostep, not a command | **the chart must say**; see below |
| `uncertain` after owner loss (CORE-007) | lost invocations become `error.communication` (core ADRs 0038, 0039) | **same honesty** |
| Commit before publish (CORE-009) | effects released after the turn's commit (replay proposal) | **same rule** |
| Keep all history for the session's life (CORE-012) | "history grows without bound" (replay proposal) | **same decision** |
| Session IDs are never reused (MODEL-002, CORE-012) | a released name that comes back | an open point for us too |
| Profile: the commands, updates and state a client sees | a chart family's events in and out | a contract over the chart |
| Binding: transport, auth, live delivery | none defined by DASP | we would write one |

DASP is about the client's side of an actor: what a client can rely on
after it loses a reply. That is the part tinyactors does not define
today. Durable Streams is a transport for ordered bytes; DASP is the
meaning on top. They fit together (see the last section).

## The protocol in brief

### Model

- **Actor:** "a logical target that performs application work. Its
  identity does not identify a process, machine, language object, or
  connection" (MODEL-001).
- **Host authority:** owns sessions, command retry records and saved
  updates; "can span multiple processes or machines" (MODEL-001).
- **Session:** binds one actor, one profile version and one ordered
  history, fixed for its life. Several clients may use it. An actor may
  have several sessions, with no order between them. The client chooses
  the session ID; reopening the same tuple reopens it, a changed actor or
  profile conflicts (MODEL-002, MSG-003).
- **Command lifecycle:** not admitted → admitted and pending → settled
  (exactly one of completed, failed, cancelled, uncertain). "A transport
  failure does not transition admitted work to cancelled" (MODEL-003).
- **Saved and temporary data:** an **update** is an immutable saved
  event with a sequence; a **view** is a projection at a saved cursor;
  **progress** is temporary and may be lost, repeated or reordered.
  Only applied updates or a coherent view advance a client's cursor
  (MODEL-004, MSG-008).

### Messages

CloudEvents 1.0 structured JSON, `datacontenttype: application/json`,
with one extension, `requestid` (ENV-001). Fourteen types, all
`dasp.<noun>.<verb>.v1` (MSG-002):

| Client → host | Host → client |
|---|---|
| `session.open` | `session.opened` (with the current head cursor) |
| `command` `{session_id, command_id, name, input}` | `receipt` `{disposition: accepted \| duplicate \| rejected, admission_sequence, error}` |
| `updates.read` `{after, limit ≤ 100}` | `updates` `{after, next, head, events}` |
| `view.read` | `view` `{cursor, state}` |
| `outcome.read` `{command_id}` | `outcome` `{state: pending \| settled, sequence, outcome}` |
| | `update` (push), `progress` (push), `resync.required` (push), `failure` |

- An **update** has a `kind`: `command.accepted`, `command.outcome` or
  `application` (a profile-named fact, with or without a command ID)
  (MSG-005).
- **Three identities** (ENV-002): `(source, id)` is one event;
  `command_id` is one durable intent in a host authority; `requestid` is
  one request attempt. A retry keeps the command ID and data, and gets a
  new request ID.

### Admission and recovery

- **Validate, then admit.** Authorize, check the core shape, and check
  the profile before admission (CORE-001).
- **Retry equality** compares session, command name and the complete
  input. Object key order is ignored; array order, exact strings, absent
  fields and explicit nulls are not. Profiles must not normalize retry
  data (CORE-002).
- **Concurrent equal submissions converge on one admission.** A changed
  input for an admitted ID is `conflict`. A rejected command does not
  reserve its ID (CORE-003).
- **Save before the receipt.** The command identity, its retry data and
  one `command.accepted` update are saved "as one recoverable decision"
  before acceptance is visible (CORE-004).
- **A lost reply does not cancel work** (CORE-005).
- **Outcomes are immutable.** A stale worker cannot replace one or add a
  second (CORE-006).
- **Uncertainty is explicit.** After owner loss, the host reconciles
  saved evidence before dispatch. If it cannot establish the effects, it
  saves `uncertain` and prevents an unsafe repeat. Uncertain is
  terminal; resolving it needs a separate profile (CORE-007).
- **No exactly-once effects.** "The core guarantees no second admission
  for an equal retry. It does not guarantee exactly-once external
  effects" (CORE-008).
- **Ordered facts.** Sequences start at 1 and are contiguous per
  session. Commit before publish. Replays keep the original CloudEvents
  identity (CORE-009).
- **The client applies `cursor + 1` before advancing,** saves the cursor
  with its state, and stops at a gap or an unknown event (CORE-010).
- **Recovery** (CORE-011): reopen the session; read outcomes or retry
  unresolved commands with the same identity; read updates after the
  last applied cursor; then hand over to live delivery without a gap.
  The binding must define that handover. A simple one subscribes first,
  buffers, replays and deduplicates; overflow means resync.
- **Storage lifetime** (CORE-012): keep every update, outcome and retry
  record for the session's life. No pruning, no snapshot-only recovery.
  Out of space means reject new work, not delete evidence. A durable
  host must declare its durability boundary (process restart, power
  loss, disk failure). Deleted IDs stay unavailable.

### Profiles, bindings, security

- **A profile** publishes command names and closed input schemas,
  outputs and "completion scope" per command, update names and state
  projection rules, progress, errors, and "execution concurrency,
  ordering, and cancellation behavior". A session binds exactly one
  profile version; a new version needs a new session (PROFILE-001).
- **A binding** defines endpoints, authentication, version and profile
  selection, framing, request routing, replay paging, live delivery and
  its handover, backpressure and a durability declaration
  (PROFILE-002). None exists yet.
- **Multiple clients** may read and submit. Membership, presence and
  concurrent edits are not defined (PROFILE-003).
- **Security:** every ID in a message is untrusted; permission is
  checked on every open, submit, read, replay and live delivery. Names
  from the wire must never become atoms, classes, code or paths
  (SEC-001).
- **Limits** (ENV-004): 1 MiB per message, 64 KiB per saved update and
  per string, nesting depth 16, 1024 members per object or array.
  **Numbers are safe integers only;** "fractions and larger exact
  quantities MUST use profile-defined strings" (ENV-003).

## How the parts line up with tinyactors

### Vocabulary

Two words collide. A DASP **session** is a client's channel onto an
actor, with its own history; a tinyactors (SCXML) session is the running
interpreter, with a session ID that outlives residency (core ADR 0066).
A DASP **host** is the server; our "host" is the embedder of core. A
tinyactors document about DASP should say "DASP session" and "DASP
host".

### Admission is ours already; saving it is not

Core ADR 0055 makes `ta_actor_deliver` the admission test, and napi's
`send({ until: "accepted" })` resolves when a message is queued. A DASP
receipt needs more:

- **A command record** per host authority: `command_id → (DASP
  session, name, input, admission sequence)`, with equality as CORE-002
  defines it. Nothing in napi keeps one.
- **Saved before the receipt,** together with the `command.accepted`
  update (CORE-004). This is the replay proposal's durable mailbox
  ("mail in flight must also be logged when it is accepted"), with a
  receipt on top. In layout 3 (one store per system) the record, the
  update and the actor's log entry commit in one transaction.
- **A comparison that ignores key order.** Values in napi are
  JavaScript values shared by reference (napi ADR 0011). Retry equality
  needs a canonical JSON form, which is the value codec both proposals
  leave open.

### An outcome is a fact the chart must state

`send({ until: "completed" })` resolves when the macrostep the message
started has ended. A command in a statechart often spans many
macrosteps: it starts an invocation, waits for `done.invoke`, retries on
a timer. And an SCXML event that no transition matches is discarded
without a word. So the runtime cannot know when a command is settled;
the chart has to say so.

SCXML already has the shape for it. If commands arrive through a `dasp`
I/O processor with a per-command origin (core ADR 0068), the chart
answers with the reply idiom:

```xml
<transition event="report.create">
  <!-- ... start the work ... -->
</transition>
<transition event="done.invoke.report">
  <send typeexpr="_event.origintype" targetexpr="_event.origin"
        event="dasp.completed"> ... </send>
</transition>
```

The processor turns that send into a `command.outcome` update. An
application update is a send to the DASP session's target; progress is
a send the processor does not save.

Open for a chart author:
- a command the chart ignores stays **pending forever**. A profile
  (DASP says it must define completion) or the processor needs a rule,
  such as a deadline after which the host saves `failed`;
- the origin must survive a reload so an outcome can still be sent
  after a restart. Images store origin text (core ADR 0068), but core
  ADR 0074 makes images process-local, so a restart rebuilds from the
  log.

### `uncertain` already has a counterpart

DASP-CORE-007 asks the host to admit when it does not know whether an
external effect happened. Tinyactors does this for invocations: an
invocation that was active when an image was captured comes back as
`error.communication` (core ADRs 0038, 0039), and replay persistence
suppresses effects while it replays. A chart that receives that error
for the invocation doing a command's external work has exactly the
evidence for `uncertain`, and can say so to the command's origin.

### Commit before publish, full history

- CORE-009 ("commit an update before publishing it") is the replay
  proposal's write-ahead rule: napi releases a turn's effects after the
  turn's commit. DASP updates are effects of that turn.
- CORE-012 (keep everything for the session's life, no snapshot-only
  recovery) agrees with the replay proposal's "history grows without
  bound". Durable Streams, by contrast, lets a server drop old data.
- A **view** must be coherent with a cursor (MSG-007). Between turns, an
  actor's inspection matches the head of its DASP sessions if the
  turn's updates commit with the turn. Mid-turn inspection is not
  coherent.

### Profile versions and chart revisions

A DASP session binds one profile version for life (PROFILE-001).
Tinyactors expects chart revisions to change under a running named
actor (replay re-derives state through the current revision). So a
profile is the public contract of a chart family, and revisions must
keep it; a revision that changes commands or updates needs a new
profile version, and so new DASP sessions. Definitions are inspectable
(core ADR 0042), so a profile's command names could be checked against
the events a chart handles.

### Multi-player

DASP's shared session (an editor, an agent and an operator on one
history, each with its own cursor) maps onto one named actor with
several clients. Permission checks on every read and live delivery
(SEC-001) belong to the binding and the web app, not to core.

## DASP over Durable Streams

DASP deliberately has no binding. Durable Streams would carry its
updates well, but not its commands.

| DASP need | Durable Streams | Notes |
|---|---|---|
| Ordered, saved updates per DASP session | one JSON-mode stream per DASP session | the update carries its own contiguous `sequence`; stream offsets stay opaque |
| Replay after a cursor, then live, with no gap (CORE-011) | catch-up read, then SSE from `Stream-Next-Offset` | race-free by construction: both use the same offset, so `resync.required` is not needed |
| Many clients with their own cursors | CDN-cached chunks | fits |
| Command dedupe by ID with a conflict on changed input | producer headers dedupe by `(producer, epoch, seq)` and never compare content | **not enough**: the DASP host still needs its own command records |
| Receipt before execution | none | a separate request–reply endpoint |
| Progress that is not saved | none: every append is durable | a separate channel, such as a second SSE stream |
| Session IDs never reused | deleted URLs SHOULD NOT be reused (§4) | same rule |
| Full history kept | retention MAY drop data | the server must be configured not to |

So a binding for a tinyactors web app could be: HTTP `POST` for
commands and reads, answered by the napi host from its command records;
one Durable Streams stream per DASP session for updates; and SSE for
progress.

## Open questions

- Will DASP choose a license and a first binding? Both are listed as
  release decisions (`docs/project/releases.md`, PROFILE-004).
- Safe integers only: the JavaScript data model has floats everywhere.
  A profile must turn fractions into strings, and the processor must
  refuse a float instead of rounding it.
- Where the command record lives when several napi processes share one
  log, and how it is fenced (the replay proposal's ownership question).
- Whether "command settled" can be declared per chart family (a
  deadline, a default outcome), or must be written in every chart.
- How the command ID relates to the SCXML send ID (core ADR 0048): a
  send ID is per sending session; a command ID is per host authority.
