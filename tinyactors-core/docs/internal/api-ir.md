# API description (`tinyactors.api.json`)

`include/tinyactors.api.json` describes the public API in
`include/tinyactors.h` completely enough that a binding generator never reads
the header or relies on a naming convention (ADR 0041). It is the contract for
every host in ADR 0024. The test host's RPC generator is its first consumer and
reads nothing else about the C API; when a generator needs a fact, the fact
belongs here.

Its format is published as `include/tinyactors.api.schema.json` (JSON Schema,
draft 2020-12). The format name is `tinyactors-api/2`. Consumers check it and
refuse other versions.

## Workflow

```text
mise run api           # describe the header; writes include/tinyactors.api.json
mise run api --check   # fail if the checked-in description is out of date (run by mise run build)
mise run api-test      # test the extractor against tests/api/*.h
```

After changing `include/tinyactors.h`, annotate what C cannot say (see
[Annotations](#annotations)), run `mise run amalgamate` and `mise run api`,
and commit the description with the header. A diff of the description is an
API change to review.

The task runs every stage in memory and writes only the description:

1. **Parse.** clang parses the header with `-ffreestanding` for pinned
   targets (`x86_64-unknown-linux-gnu` and `wasm32-unknown-unknown`), so the
   result does not depend on the machine that runs it. `TA_API_EXTRACT` turns
   the annotation macros into attributes that clang records on each
   declaration. `CLANG` selects the compiler.
2. **Build.** Structural rules and annotations give every type, member, and
   parameter its facts. Anything missing, contradictory, unused, or
   unsupported is an error. All errors are reported together, each with its
   path, for example `ta_actor_spawn_params.address`.
3. **Measure.** clang evaluates the size, alignment, and member offsets of
   every type, and the value and type of every macro constant, for each
   target. "Implemented" comes from the symbols the compiled amalgamation
   exports (`nm`), not from reading the sources.
4. **Validate.** The result is checked against the JSON Schema.
5. **Compile back.** The description is rendered as C and compiled with the
   real header for each target under `-Werror`. The C consists of typedef and
   function redeclarations and static assertions on sizes, offsets, member
   types, and constant values. Positional initializers check that every
   member is described, in order: one value per described member, so a
   missing member trips `-Wmissing-field-initializers` and an invented one
   fails to compile. A description that says anything false about the header
   does not compile.

## Annotations

C types say what a declaration *is*, not how to use it. The annotation macros
in `include/tinyactors.h` state the rest. Outside extraction they expand to
nothing, so they never change the compiled API.

### Rules the extractor applies on its own

These follow from C types alone and need no annotation. An annotation that
repeats them is an error.

| C type | Shape | Direction |
|---|---|---|
| pointer to a `TA_HANDLE` type | `handle` | in |
| `T **` where `T` is a `TA_HANDLE` type | `handle_out` (creates a handle) | out |
| pointer to a `TA_VIEW` type | `view` | in |
| pointer to a `TA_VTABLE` type | `vtable` | in |
| `void *` / `void **` | `raw` | in / out |
| pointer to a function | `callback` | in |

Every other pointer needs exactly one shape annotation. The members of
`TA_STRING`, `TA_BYTES`, and `TA_HOST_VALUE` types are internal and take no
annotations.

### Types

| Annotation | On | Meaning |
|---|---|---|
| `TA_HANDLE` | opaque typedef | An owned object, referenced through a handle. Requires one function that creates it and one parameter marked `TA_DESTROYS`. |
| `TA_VIEW` | opaque typedef | An object borrowed from the core; hosts cannot create it. |
| `TA_STRING`, `TA_BYTES` | struct typedef | `{ptr, len}` text or bytes. |
| `TA_HOST_VALUE` | struct typedef | A host value with retain/release semantics (`ta_value`). |
| `TA_ADAPTER` | struct typedef | `{void *self, operations}` implemented by the embedder. Required on every struct that holds a vtable or callback. |
| `TA_VTABLE` | struct typedef | The operations of an adapter. |
| `TA_TAGGED(tag, union)` | struct typedef | Enum member `tag` selects the member of anonymous union `union`. Every union member needs `TA_WHEN`. A union without a tagged owner is an error. |
| `TA_ZERO_IS_ABSENT` | struct typedef | Members equal to zero are absent. A tagged union with this annotation is absent when its tag is zero. |
| `TA_SYMBOLS("PREFIX_")` | enum typedef | Required on every enum. A constant's symbol is its name without the prefix, lowercased: `TA_RUN_PAUSED` is `paused`. A constant without the prefix is an error, so a new constant can never rename its neighbours. |
| `TA_CONSTANTS_OF(type, "PREFIX_")` | anonymous enum | The constants are the values of integer typedef `type` (`ta_result`). |
| `TA_FLAG_SET(name, "PREFIX_")` | anonymous enum | The constants are bit flags named `name`. |
| `TA_SUCCESS` | enum constant | The result that means success (`TA_OK`, `TA_OP_OK`, `TA_EXTERNAL_EVENT_ACCEPTED`). At most one per enum. |

### Members, parameters, and callback parameters

Pointer shapes, exactly one per pointer the rules above do not cover:

| Annotation | Shape | Meaning |
|---|---|---|
| `TA_IN` | `single`, in | One element the receiver reads. The pointee must be const. |
| `TA_OUT` | `single`, out | One element the callee writes. |
| `TA_INOUT` | `single`, inout | One element the callee reads and updates in place. |
| `TA_TERMINATED_BY(member)` | `terminated`, in | A list ending at the first element whose `member` is zero (a null pointer; for a string, a null `ptr`). |
| `TA_COUNTED_BY(member)` | `counted`, in | A list of `member` elements. `member` is an integer sibling, which gets `count_of`. |
| `TA_BUFFER(capacity, needed)` | `buffer`, out | A caller array of `capacity` elements. The callee reports the total through the out sibling `needed`, which should be `TA_ALWAYS`. A `char` buffer holds text. |
| `TA_VIEW_OUT` | `view_out`, out | `const T **`: the callee stores a pointer to a borrowed graph. |
| `TA_ARENA` | `arena`, inout | A caller `ta_arena` that receives the outputs. |

Facts:

| Annotation | Meaning |
|---|---|
| `TA_OPTIONAL` | The pointer may be null, and null means absent. Without it, a pointer must not be null. |
| `TA_ALWAYS` | The output is valid after every result, for example `needed` after `TA_ERR_NOMEM`. |
| `TA_VALID_ON(CONSTANT)` | The output is valid only after that result, for example a `ta_fault` after `TA_OP_ERROR`. |
| `TA_RELEASE(function)` | The receiver owns the output and releases it with `function`. On a by-value host value member (`ta_fault.data`), the member is owned by whoever receives the struct. |
| `TA_RELEASE_IF(function, flag)` | Like `TA_RELEASE` for a by-value member, when bool sibling `flag` is true (`ta_actor_completion.data`). |
| `TA_LIFETIME(scope)` | Borrowed data stays valid for `scope`: `call` (until the call or callback returns), `system` (while the system is externally synchronized against mutation), `definition` (while the definition exists), `arena` (until the arena is reset), or `input` (while the input it came from is retained). An output that holds borrowed references (pointers, strings, host values, adapters) needs `TA_LIFETIME` or `TA_RELEASE`. |
| `TA_DESTROYS` | On a handle parameter: the function destroys the handle. |
| `TA_CONSUMED_ON_OK` | On a handle parameter: the callee takes ownership when it returns `TA_OK`. |
| `TA_WHEN(CONSTANT)` | On a member of a tagged union: `CONSTANT` selects it. |
| `TA_FLAGS(set)` | On an integer member: it holds flags of a `TA_FLAG_SET`. |

### Functions

| Annotation | Meaning |
|---|---|
| `TA_API` | Public. Every function that is not `static inline` must have it. |
| `TA_VALUE_MODEL(kind, path)` | The function's `ta_value` inputs belong to the data model of the actor (`kind` `actor`, `path` naming a `ta_actor_id`), definition (`definition`, a `ta_definition_id`), or session image (`session_image`, a handle) at `path`, for example `params.definition`. |
| `TA_ANY_VALUE_MODEL` | The function's `ta_value` inputs may belong to any data model; it does not interpret them. |
| `TA_OPTIONAL`, `TA_LIFETIME(scope)` | For a function that returns a pointer. Every non-`void` returned pointer needs `TA_LIFETIME`. |

Every function that takes `ta_value` inputs needs one of the value-model
annotations. Put function annotations after the parameter list:
`mise run amalgamate` recognizes public prototypes by `TA_API <type> ta_...(`
at the start of a line.

`#define` constants are described with the type of their expression, so write
them as typed expressions, for example
`#define TA_STATE_NONE ((ta_state_id)UINT32_MAX)`.

## Format

```json
{
  "format": "tinyactors-api/2",
  "header": "include/tinyactors.h",
  "targets": {"x86_64-unknown-linux-gnu": {"pointer": {"size": 8, "align": 8}, "builtins": {"size_t": {"size": 8, "align": 8}}}},
  "constants": [{"name": "TA_STATE_NONE", "type": {"type": "ta_state_id"}, "value": 4294967295}],
  "flag_sets": {"ta_event_flags": {"constants": [{"name": "TA_EVENT_FLAG_OWNS_DATA", "value": 1, "symbol": "owns_data"}]}},
  "types": {"<name>": {"kind": "..."}},
  "functions": [{"name": "ta_actor_spawn", "...": "..."}]
}
```

### Type references

| Form | Meaning |
|---|---|
| `{"builtin": "uint32_t"}` | `void`, `char`, `bool`, `int`, or a `<stdint.h>`/`<stddef.h>` type |
| `{"type": "ta_str"}` | a named type in `types` |
| `{"pointer": T}` | pointer to `T` |
| `{"function": {"returns": R, "params": [slot]}}` | a function; its parameters are slots with names and facts |
| `{"record": {"kind": "struct" \| "union", "fields": [slot]}}` | an anonymous struct or union member |

`"const": true` qualifies the node it appears on:
`{"pointer": {"type": "ta_str", "const": true}}` is `const ta_str *`, and
`{"pointer": {...}, "const": true}` is a const pointer.

### Types

Every entry has a `kind` and may have a `doc` (the header comment, as text).
Sized kinds have a `layout`:
`{target: {"size", "align", "offsets": {"member.path": offset}}}`.

- `alias`: `{"type": T}`. `ta_actor_id` is `uint64_t`. A callback typedef is an
  alias of a pointer to a function.
- `enum`: `constants` of `{name, value, symbol, success?, doc?}`.
  `underlying` is present when the constants belong to an integer typedef
  (`ta_result`).
- `handle`: an owned opaque object. `handle` is its protocol name
  (`system`). `created_by`, `destroyed_by`, and `consumed_by` list
  `{function, path, on?}`, where `path` leads from a parameter to the slot:
  `["params", "out"]`.
- `view`: an opaque object borrowed from the core.
- `struct` / `union`: `fields` (slots), plus optional facts:
  - `semantic`: `string`, `bytes`, `host_value`, `adapter`, or `vtable`;
  - `tagged`: `{"tag", "union", "enum", "members"}`, where `members` maps
    every constant of `enum` to its union member or `null`;
  - `zero_is_absent`: `true`.

### Slots: members, parameters, and callback parameters

| Key | Present on | Meaning |
|---|---|---|
| `name`, `type`, `doc` | every slot | |
| `shape`, `direction` | pointers | see [Annotations](#annotations) |
| `optional` | pointers | may be null |
| `terminator` | `terminated` | the element member whose zero ends the list |
| `count` | `counted` | the sibling holding the element count |
| `capacity`, `needed` | `buffer` | the siblings holding the capacity and the total |
| `count_of`, `capacity_of`, `needed_of` | the siblings above | the list or buffer they belong to |
| `valid_on` | outputs | `success` (the function's `success` result), `always`, or a result constant |
| `release` | outputs, owned host value members | the function that releases the value |
| `release_if` | owned host value members | `{"function", "flag"}` |
| `lifetime` | pointers | how long borrowed data stays valid |
| `destroys`, `consumed_on` | handle parameters | handle ownership |
| `when` | union members | the constant that selects the member |
| `flags` | integer members | the flag set they hold |

Output validity is relative to the call that receives the slot: a
parameter's function, or, for a member of a params struct, every function or
callback that takes the struct. The extractor checks that each of them
returns an enum with the named result.

### Functions

`{name, doc?, public, static_inline, implemented, returns, params, value_model?}`:

- `returns` is `{type, success?, shape?, optional?, lifetime?}`. `success` names
  the success constant of the returned enum. `shape` is `single` or `raw` for
  a returned pointer.
- `implemented` is true when the amalgamation exports the function or it is
  `static inline`. An unimplemented function is declared but does not link.
- `value_model` is `{"kind": "any"}` or `{"kind", "path"}`, where `kind` is
  `actor`, `definition`, or `session_image`.

Callbacks (the members of `TA_VTABLE` types and callback typedefs) use the
same `returns` and slot facts inside their `function` type.

## Using the description

- A host copies the contents of a `single` in pointer before the call, may
  pass null only for `optional` pointers, and reads outputs only when their
  `valid_on` holds.
- A host releases every `release` output and every `release_if` member whose
  flag is set, and treats every other reference reached through an output as
  borrowed for its `lifetime`.
- A binding that builds `ta_value` inputs from wire data picks the data model
  from `value_model`.
- Handles begin at `created_by`, end at `destroyed_by`, and pass to the core at
  `consumed_by`.

## Not described

- The results each function can return, beyond its success result.
- Threading: every call is externally synchronized (ADR 0017).
- Doc comments of `#define` constants.
