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

## Sentinel-terminated lists

Every child-list pointer addresses an array terminated by a literal zero
struct value, `{0}`. This includes state, transition, action, parameter, data,
invoke, and target lists. The authoring enums reserve zero for the terminator:
`TA_SCXML_STATE_NONE`, `TA_SCXML_TRANSITION_NONE`, and
`TA_SCXML_ACTION_NONE`; invoke lists use `TA_SCXML_INVOKE_NONE`. Parameter,
data, and target lists terminate when their required `name.ptr` or `ptr` is
null, which is exactly the result of `{0}`.

These are distinct from the runtime enums in `definition.h`, whose zero values
can be meaningful. The compiler rejects a terminator anywhere a real child is
required and stops scanning a list at its first zero element.

## Shape and ordering

The shape maps directly to SCXML:

- `ta_scxml_spec` owns root data, root initial, and root states.
- states own child states, an optional initial transition, transitions,
  entry/exit actions, data, and invokes;
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

State IDs, targets, transition event descriptors, and named send events are
resolved during compilation. Expressions and locations are `ta_expression`
values, never preassigned `ta_expr_id` values. An expression is either source
text, suitable for XML and JSON importers, or a retained host value, suitable
for language embeddings such as Node-API. A zero-initialized expression is
absent. The compiler assigns `ta_expr_id` values while it builds the immutable
definition and retains the corresponding expression table for
`prepare_definition()`.

Host expressions use `ta_expression_host_vtable` to retain and release their
values. A Node-API adapter retains a `napi_ref`, not a callback-scoped
`napi_value`; the selected datamodel later resolves that reference on its
owning JavaScript environment thread. `TA_SEND_NONE` and `TA_INVOKE_NONE`
represent absent optional identifiers.

## Lifetime

`ta_definition_compile_spec()` consumes the specification synchronously and
copies all data it retains. A specification may therefore be a nested compound
literal, including arrays created with compound literals. For dynamically
constructed specs, retain the `ta_tree` until compilation returns, then destroy
it. The compiled definition retains no pointer into either authoring form.

## Compiled IR

The source tree is discarded after compilation. The compiler emits the compact
`ta_definition` IR: normalized state and transition spans, flattened action
streams, initial transitions, data initializers, content, parameters, invokes,
and extensions. Compilation performs cross-tree validation including target
resolution, state-kind rules, duplicate IDs, transition semantics, and data
binding timing.
