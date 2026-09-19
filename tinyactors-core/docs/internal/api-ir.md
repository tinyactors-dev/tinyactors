# API description (`tinyactors.api.json`)

`include/tinyactors.api.json` is a machine-readable description of the public
API in `include/tinyactors.h` (ADR 0041). Binding generators read it instead of
parsing C. The test host's RPC bindings are its first consumer; bindings for the
hosts in ADR 0024 can use it too.

## Workflow

```text
mise run api           # regenerate include/tinyactors.api.json
mise run api --check   # fail if it is out of date (run by mise run build)
```

The file is checked in, so an API change shows up as a reviewable diff. The
task parses only `include/tinyactors.h`, with
`cc -xc -std=c11 -Xclang -ast-dump=json -fsyntax-only`, and keeps declarations
whose names start with `ta_` or `TA_`. It then applies naming conventions and
the exceptions in `api/annotations.rb`. Shared source conventions (the
prototype and definition patterns used by `mise run amalgamate`) live in
`mise/lib/c_api.rb`.

## Format

```json
{
  "format": "tinyactors-api/1",
  "header": "include/tinyactors.h",
  "constants": [{"name": "TA_INVOKE_ID_NONE", "value": 18446744073709551615, "macro": true}],
  "types": {"<name>": {...}},
  "functions": [{...}]
}
```

A type reference is one of:

| Form | Meaning |
|---|---|
| `{"builtin": "uint32_t"}` | C builtin or `<stdint.h>` type |
| `{"type": "ta_str"}` | a named type in `types` |
| `{"pointer": T, "const": true}` | pointer; `const` qualifies the pointee |
| `{"record": {"kind": "union", "fields": [...]}}` | anonymous struct or union |
| `{"function": {"returns": T, "params": [T]}}` | function pointer |

Types have a `kind`:

- `alias`: `{"type": T}`, for example `ta_atom` is `uint32_t`.
- `enum`: `constants` with `name`, `value`, and `json`, the lowercase name
  without the enum's common prefix (`TA_RUN_PAUSED` is `paused`). Anonymous
  enums that give an integer typedef its values (`ta_result`) are attached to
  it through `api/annotations.rb` and record the `underlying` type.
- `opaque`: an incomplete struct. `ownership` is `handle` (an owned object such
  as `ta_system`, with its `handle` name) or `borrowed` (a view such as
  `ta_dm_host`).
- `struct`: `fields`, plus optional facts:
  - `semantic`: `string` (`ta_str`), `bytes` (`ta_bytes`), `adapter` (a
    `{void *self, vtable or callback}` fat pointer), or `vtable` (only function
    pointers);
  - `tagged_union`: `{"tag": "kind", "union": "as", "enum", "members"}`, where
    `members` maps every enum constant to its union member or `null`;
  - `zero_is_absent`: true for `ta_scxml_*` specifications, whose optional
    members are absent when zero.

Fields and parameters carry:

- `role`: `in` or `out`. A non-const pointer to a non-opaque type is an output;
  a const pointer is an input.
- `shape` for pointers:

  | Shape | Rule |
  |---|---|
  | `single` | one optional element; the default for const pointers |
  | `terminated` | a `{0}`-terminated array; const pointers inside `ta_scxml_*` structs |
  | `counted` | followed by a `count` or `*_count` field (named by `count`; that field gets `count_of`) |
  | `handle` | pointer to an owned opaque type |
  | `borrowed` | pointer to a borrowed opaque type |
  | `handle_out` | `T **` where `T` is opaque; creates a handle |
  | `view_out` | `const T **` where `T` is a struct; the callee stores a pointer to a borrowed graph |
  | `arena` | a caller-supplied `ta_arena *` that receives the outputs (role `in`; annotated) |
  | `buffer` | caller-supplied output buffer, with `buffer.capacity` and `buffer.needed` fields |
  | `raw` | `void *` |
  | `function` | callback pointer |

- `release_when`: when an output value must be released: the flag field that guards a struct field, or `"ok"` for a function output parameter (release after `TA_OK`).
- `consumed_on`: the function takes ownership of this handle on `"ok"`.

Functions have `name`, `returns`, `params`, `public` (declared with `TA_API`),
`static_inline`, `retires` (the parameter that `ta_<type>_destroy` retires),
and `implemented`: whether a module source defines the function, which tells
generators whether calling it links.

## Annotations

`api/annotations.rb` holds only what conventions cannot express: `single`
exceptions inside specifications, output `buffers`, output `arenas`, union members whose names
differ from their enum constants, `borrowed` opaque types, `owned_outputs`,
ownership transfers (`consumes`), and `enum_constants`. When the extractor
cannot map something, for example a union member without an enum constant, it
fails instead of guessing.
