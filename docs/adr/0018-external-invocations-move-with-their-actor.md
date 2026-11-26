# ADR 0018: An external invocation has an id of its own, and moves with its actor

**Status:** Accepted (2026-11-26), by the maintainer; refined the same
day after a refutation (the id's range, delayed sends to a service, the
matching rule, where the `UNINVOKE` comes from).
**Date:** 2026-11-26

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the application developer who deploys a new version of a
  chart** while its actors wait on a service: a request in flight (an HTTP
  call the chart invokes), a payment, a shipment that runs for a week. Today
  a migration refuses every such actor (`TA_ERROR_STATE`) until its
  invocations finish, and one whose service never answers stays on the old
  version for good: a deploy cannot reach the actors that need it most.
- **Then: the operator, and an agent working for them** (the MCP spike of
  2026-11-26), who rehearse a migration on a snapshot and find the actors it
  left behind.
- **Then: the binding author.** A service is keyed by one number instead of
  a segment and a token.
- **Out of scope:** SCXML invocations (child sessions of this system): a
  tree still moves unchanged and is translated once its invocations
  finished. Hibernating an actor with running invocations: still refused.
  Restarting a service: a carried invocation keeps the params and src it was
  started with.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event | nothing |
  | per event from a service (`TA_EVENT_IN_FROM_INVOKE`, `_INVOKE_DONE`, a failure) | finding the invocation among its actor's invoke slots: at most the chart's `<invoke>` count, instead of one pool lookup |
  | memory | 8 B per request record (the id), 8 B per effect record, 8 B per segment (the next id) |
  | blob | an export blob carries each external invocation: its id, slot, type, src, invoke id and params |
  | API | `ta_event_in.token` becomes `ta_event_in.invocation` (64 bits); `ta_effect.invocation` and `TA_EFFECT_FIELD_INVOCATION` are added; `TA_EFFECT_FIELD_TOKEN` and `ta_effect.token` are the token of a `REQUEST` only |
  | every binding | its services map is keyed by `INVOCATION` instead of (`SEGMENT`, `TOKEN`) |

## Context

An external invocation is a request record in its segment's pool, and the
invoking state's slot holds the record's handle (`src/system/route.c`,
`ta_system_route_invoke`). The handle is the token core gives the embedder
in `TA_EFFECT_INVOKE`, and takes back in `ta_event_in.token` with
`TA_EVENT_IN_FROM_INVOKE` (`src/system/schedule.c`, `external_request`).
It is valid in that segment only. A migration exports an actor from one
segment and adopts it into another, so a running invocation's token would
name nothing there, and no record could follow it: an export refuses an
external invoke (`src/system/hibernate.c`, `stable_member`), and the host
fails or waits for an actor with invocations (`src/host/host.c`,
`migrate_invoking`, "an invocation may never finish"). The binding keyed
its service by (segment, token), so a moved invocation would have had to be
re-keyed in every binding.

The rule was written when invocations were short. Two cases showed it is
not: a request in flight (a chart that invokes an HTTP call, many in
parallel, each answered when its response comes) should not hold back a
deploy, and an order's week-long shipment service is the migration guide's
own motivating example. A spike (2026-11-26) also showed the same request
written as a `<send>` to an I/O processor migrates today and its answer
reaches the new version, so the difference is only how core names an
invocation.

A snapshot already carries invocations across a host: a restored host
announces each one again with its type, src, invoke id and params
(`TA_EFFECT_FLAG_RESTORED`). Nothing in the invocation itself is tied to
its segment but its token.

## Decisions

1. **An external invocation has an id**: 64 bits, unique in the host and
   never reused, given when it starts from a counter of its segment that
   begins at the segment's `first_serial` (each segment's range is 2^32
   wide). Once a segment has handed out every id of its range, an
   `<invoke>` of a service there is `error.execution`: an id is never given
   twice. A host and a copy restored from its snapshot go on giving the
   same ids, so an id is no idempotency key for a service outside the
   process. It is `ta_effect.invocation` and `TA_EFFECT_FIELD_INVOCATION` of
   `INVOKE`, `UNINVOKE` and a `SEND` to the service, and
   `ta_event_in.invocation` with `TA_EVENT_IN_FROM_INVOKE`,
   `TA_EVENT_IN_INVOKE_DONE` or a failure. With `ta_host_send` the event's
   `session` finds the actor wherever it lives, and the id finds the
   invocation among its invoke slots. A binding keys its services by the id
   alone. The token stays for `REQUEST` and `ta_resume`.
2. **A migration carries an actor's external invocations.** The export
   blob holds each one (id, slot, `ended`, type, src, invoke id, params as
   a value). The translation keeps one when its state is kept and the new
   version's state has an `<invoke>` with the same authored `id` (or, for
   one without, at the same position among the state's invokes) and the
   static type the invocation runs with (an `<invoke typeexpr>` in the new
   version never keeps one); it moves to that slot, and the new version's
   `<finalize>`, autoforward and transitions handle what the service sends
   from then on, and a delayed send to it (`#_<invokeid>`) follows it. An
   invocation without an authored id is kept only when its state has
   exactly one `<invoke>` without one in both versions: a position would
   bind the wrong service when an `<invoke>` is inserted before it. "Kept"
   means mapped and still active once the configuration is completed. The
   others are cancelled as leaving their state cancels them: the
   translation leaves them out of the blob, and the host appends an
   `UNINVOKE` with the id after the actor has moved (room for one per
   service reserved before the export, so a stop is never lost, and none
   appended when the adoption fails and the actor goes back). Their queues
   are empty: an export needs them so. A kept state whose running
   invocations were all cancelled starts the new version's invokes, as
   entering it would, so its service is replaced rather than lost. A
   carried invocation emits nothing: the service goes on, under the same
   id.
3. **The host migrates an actor with external invocations at once.** It
   waits (with a timeout) or fails only for SCXML invocations, which still
   move as a tree only, untranslated: `ta_actor_info.services` counts an
   actor's external invocations apart from `invocations`.
4. **Hibernation is unchanged**: an actor with running invocations is not
   hibernated.

## Consequences

- A request in flight survives a deploy, with either way of writing it: a
  `<send>` to an I/O processor, or an `<invoke>`.
- A service sees a migration only when the new version drops its invoke
  (`stop()`), as if its state was left.
- An export blob's format changes (version 4); blobs only cross a
  migration inside one host, so nothing stored is invalidated. Snapshots
  of the previous layout are refused, as for every layout change.
- Known gap: a datamodel without `payload_serialize` (none that ships:
  quickjs, the Node and Python bindings' own all have it) carries an
  invocation's params as a handle; when the translation cancels that
  invocation, nothing releases the handle.
- `tinyactors diff` keeps calling a changed `<invoke>` in a kept state a
  behaviour change; a removed one now cancels the running invocation at the
  migration, which it reports as behaviour too.
