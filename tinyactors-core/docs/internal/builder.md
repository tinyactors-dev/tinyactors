# `builder.h`

`builder.h` is a declarative, typed SCXML authoring API. A caller describes the
whole statechart as nested C struct values, then
`ta_definition_compile_spec()` validates and compiles that tree into an
immutable `ta_definition`.

It is intentionally not a mutable node API. There are no temporary node IDs,
parent pointers, builder lifetime rules, or call-order requirements.

## Dynamic construction with `ta_tree`

`ta_tree` is the public companion for importers and other embedders that build
a specification dynamically. It owns arena-backed storage for strings,
temporary child lists, and the sealed arrays that appear in a
`ta_scxml_spec`. It is generic: `ta_tree_list` accepts an element size and
alignment, `ta_tree_put()` copies one element, and `ta_tree_close()` returns a
contiguous array with one trailing all-zero element.

`ta_tree` records its first allocation failure. Subsequent `ta_tree_alloc()`
and `ta_tree_put()` calls are inert, so an importer checks
`ta_tree_result()` once after parsing rather than propagating an allocation
result through every XML, JSON, or JavaScript-value reader. The tree must stay
alive through `ta_definition_compile_spec()` and is destroyed afterwards.

The generic construction API is public because every dynamic embedder needs to
materialize the sentinel-terminated arrays required by `ta_scxml_spec`; it is
not SCXML-specific and has no knowledge of states, transitions, or actions.

`src/tree.c` implements it. Lists nest — an importer closes a state's
transitions while the state list is still open — so a list cannot bump its
elements straight into the arena: the inner list's elements would land among
the outer list's. Each open list stages into a chain of chunks of its own,
each chunk twice the last, and `ta_tree_close()` copies the chain into one
contiguous array with the trailing zero element. The chunks come from the same
arena as everything else the tree hands out, so `ta_tree_destroy()` returns
all of it and an abandoned, never-closed list needs no cleanup of its own.

## Sentinel-terminated lists

Every child-list pointer addresses an array terminated by a literal zero
struct value, `{0}`. This includes state, transition, action, parameter, data,
invoke, and target lists. The authoring enums reserve zero for the terminator:
`TA_SCXML_STATE_NONE`, `TA_SCXML_TRANSITION_NONE`, and
`TA_SCXML_ACTION_NONE`; invoke lists use `TA_SCXML_INVOKE_NONE`. Parameter,
data, and target lists terminate when their required `name.ptr` or `ptr` is
null, and handler lists terminate when `actions` is null. That is exactly the
result of `{0}`. An empty handler (`<onentry/>`) therefore points to an empty
action list, `(const ta_scxml_action_spec[]){{0}}`, or is left out.

These are distinct from the runtime enums in `definition.h`, whose zero values
can be meaningful. The compiler rejects a terminator anywhere a real child is
required and stops scanning a list at its first zero element.

## Shape and ordering

The shape maps directly to SCXML:

- `ta_scxml_spec` owns root data, root initial, root states, and the optional
  top-level `<script>`.
- states own child states, an optional initial transition, transitions,
  entry and exit handlers, data, and invokes;
- `onentry` and `onexit` are lists of `ta_scxml_handler_spec`, one per
  `<onentry>` or `<onexit>` element, in document order. Each handler owns its
  own action list and is a separate block of executable content (SCXML 3.8,
  3.9): an error skips the rest of that handler, and the next handler still
  runs. Merging a state's handlers into one handler changes that behavior;
- initial transitions and transitions own target lists and ordered actions;
- `if`, `foreach`, and invokes own their respective action lists;
- action order is the source order in each list.

## Motivating example: HTTP OAuth sessions

This is one application of the API, not a constraint on statechart design. In
an HTTP server, an actor can represent a durable login session rather than a
single request. The server sends request events to that actor and each
transition sends a response to the request's reply target. The OAuth example
in `builder.h` uses that model:

```text
idle -- login.requested --> authorizing -- valid oauth.callback --> exchanging
  ^                              |                                  |
  |                              +-- invalid callback: 400 ----------+
  |                                                                 |
  +-- token failure: 502 <------------------------------------------+
authenticated <--------------------------- token success: store token, redirect
```

`idle` returns an HTTP 401 for `http.resource`; `authorizing` returns an error
response for an invalid callback; and `exchanging` returns an error response
when the token endpoint fails. These are `send` actions to the HTTP I/O
processor, not `raise` actions: `raise` creates an internal SCXML event and
cannot answer an HTTP request. The authenticated state handles protected
requests and can clear the session token on logout.

## Authoring values, not runtime identifiers

The spec carries what an SCXML author writes. It never carries identifiers
that only exist inside a compiled definition or a running system:
`ta_expr_id`, `ta_send_id`, `ta_invoke_id`, `ta_atom`, `ta_event_name_id`, or
`ta_state_id`. The compiler derives all of those, so an importer needs no live
`ta_system` to build a tree.

- State IDs, targets, transition event descriptors, and named send events are
  `ta_str` values resolved during compilation.
- I/O processor and invoker types are `ta_str` names; the compiler interns
  them. A host vocabulary's actions are sends to one of those processors
  (ADR 0046), so they need no type of their own.
- Expressions and locations are `ta_expression` values. An expression is
  either source text, suitable for XML and JSON importers, or a retained host
  value, suitable for language embeddings such as Node-API. The compiler
  assigns `ta_expr_id` values while it builds the immutable definition and
  retains the corresponding expression table for `prepare_definition()`.
- SCXML `x` / `xexpr` attribute pairs are `ta_scxml_string_spec`: literal
  text or an expression. This covers `<send>` `type`, `target`, and `delay`,
  `<cancel>` `sendid`, and `<invoke>` `type` and `src`. A literal delay is an
  SCXML duration (`"500ms"`, `"5s"`) compiled to logical milliseconds.
- `<send>` and `<invoke>` `id` / `idlocation` are `ta_scxml_id_spec`: the
  author's literal ID or a location that receives the ID generated on each
  execution. Because the pair is a union, specifying both is unrepresentable.
  The runtime `ta_send_id` / `ta_invoke_id` values are session-local handles
  and are never authored.
- The datamodel binding is `ta_scxml_binding_kind`, whose zero value selects
  the SCXML default rather than a runtime enum value.
- `datamodel` is the `<scxml datamodel>` attribute: the name of the data model
  the document requires, such as `"null"` or `"ecmascript"`. It is compared
  with the adapter's `ta_datamodel_vtable.name`; a mismatch fails compilation
  with `TA_ERR_UNSUPPORTED`. Absent means no requirement. The adapter itself is
  `ta_definition_compile_params.datamodel`.

Every optional field is absent when zero-initialized. There are no special
sentinel constants to remember; omitting a field is always correct.

Host expressions use `ta_expression_host_vtable` to retain and release their
values. A Node-API adapter retains a `napi_ref`, not a callback-scoped
`napi_value`; the selected datamodel later resolves that reference on its
owning JavaScript environment thread.

## Lowered SCXML features

Some SCXML features have no dedicated field because an importer can express
them exactly with existing ones:

- `namelist` on `<send>` and `<invoke>` becomes `params`: each name is a
  parameter whose value is a `TA_SCXML_VALUE_LOCATION` of that name.
- `<elseif>` becomes a nested `if` in `else_actions`.
- `<data src>` is fetched by the importer and passed as
  `TA_SCXML_VALUE_CONTENT`; the core performs no I/O.
- `<script src>`, top-level or as an action, is fetched by the importer and
  passed as `TA_EXPRESSION_SOURCE`. A fetch failure rejects the document in
  the importer (SCXML 5.8.2).

## Top-level script

`ta_scxml_spec.script` is the `<scxml>` element's `<script>` child, which
SCXML allows at most once (3.2.2). It is a `ta_expression`, like the `script`
action, and is absent when zero.

Semantics, from the SCXML recommendation:

- **When it runs.** The processor evaluates a `<script>` that is a child of
  `<scxml>` "at document load time" (5.8.2). The interpretation algorithm
  places it per session, in `interpret()` (Appendix D): early-bound data is
  initialized, then `executeGlobalScriptElement`, then the root initial
  transition is taken. Tinyactors therefore runs it once per session during
  actor bootstrap (`ta_actor_spawn`, `ta_actor_prepare_replacement`), after
  data declarations and early initializers and before the root initial
  transition and any `<onentry>`. It is part of the bootstrap macrostep, so a
  budgeted spawn can pause around it like any other block.
- **Late binding.** Declarations exist from initialization (5.3.3, ADR 0032),
  but late initializers run on first entry of their state, which is after the
  script. With late binding the script sees declared but uninitialized data;
  what it reads is defined by the data model (5.9).
- **Errors.** A runtime failure places `error.execution` in the internal queue
  (3.12.2, 5.9). The queued error is processed in the first macrostep, after
  the initial states are entered, like any internal event raised before
  `mainEventLoop`. A data model may instead reject an ill-formed script when
  the definition is prepared (5.9.4). The Null data model has no scripting
  language (B.1.5), so a script fails there.
- **`src`.** `<script src>` must be downloaded at load time, and a document
  whose script cannot be downloaded is non-conformant and must be rejected
  (5.8.2). The importer performs that I/O and passes the fetched text as
  `TA_EXPRESSION_SOURCE`; a failed fetch is an import error, never a core
  result. `src` and inline content are mutually exclusive, so an importer
  never has both.
- **Reconstruction.** The script is bootstrap work, not replayable state:
  reconstructing or loading a session image never runs it again.

The compiled definition stores it as a block of zero or one script actions
(`global_script_actions`), so it shares the block-level error behavior of
4.9 and the continuation frames of all other executable content.

## Lifetime

`ta_definition_compile_spec()` consumes the specification synchronously and
copies all data it retains. A specification may therefore be a nested compound
literal, including arrays created with compound literals. For dynamically
constructed specs, retain the `ta_tree` until compilation returns, then destroy
it. The compiled definition retains no pointer into either authoring form.

## Compiled IR

The source tree is discarded after compilation. The compiler emits the compact
`ta_definition` IR: normalized state and transition spans, flattened action
streams, initial transitions, data initializers, content, parameters, and
invokes. Compilation performs cross-tree validation including target
resolution, state-kind rules, duplicate IDs, transition semantics, and data
binding timing.
