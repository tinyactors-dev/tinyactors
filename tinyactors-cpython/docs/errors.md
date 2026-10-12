# Errors: `tinyactors.TinyactorsError`

What the module raises when core refuses something, decided with the
reasoned-analysis skill on 2026-10-12 (D1 of the value table's check,
`values.md`): the proposal was given to a fresh agent to refute against
the repository, and one clause of it was refuted and changed (below).

## One class, with what core answered

`tinyactors.TinyactorsError` is the one exception the module raises for a
refusal from core, a subclass of `Exception`. It carries what core
answered, unchanged, and nothing the binding makes up (bindings.md,
sections 2 and 10: "An `error` becomes the language's exception, with its
`code`, `reason`, `path` and `message` unchanged. Match on `code` and
`reason`, never on `message`").

| Attribute | A refused request (`ta_host_request`) | A refused config (`ta_host_create`) | A core call that failed with a status |
|---|---|---|---|
| `status` | `None` | the call's status, `ERROR_INVALID` | the call's status (`ERROR_INVALID`, ...) |
| `code`, `reason`, `path` | the reply's `error.code`, `error.data.reason`, `error.data.path` | the same, from the error core wrote (`environment->error`) | `None` |
| the message (`str(error)`) | the reply's `error.message` | the same | the core function and the status's name: `ta_cbor_decode: TA_ERROR_INVALID` |

- **A refused request has no status.** `ta_host_request` answers `TA_OK`
  when its reply holds an error ("TA_OK: `reply` holds the reply (a result
  or an error)", `tinyactors-core/include/tinyactors/host.h:641-642`), so
  `status` is `None` there, not a value of the binding's own. (The first
  proposal said "the call's status"; the refutation found it would be 0.)
- **A bare status's message** names core's function and the status as
  `types.h` spells it. Core has no function that names a status, so the
  binding keeps the table of names (`src/module.c`,
  `ta_python_status_names`), which are also the module's constants:
  `tinyactors.OK`, `ERROR_EXECUTION`, `ERROR_INVALID`, ...,
  `ERROR_ABORTED`, with `types.h`'s values. Match on `status` against
  them.
- **Python's own errors stay Python's.** A `MemoryError` from Python's
  allocator stays a `MemoryError`; a `TypeError` for an argument of the
  wrong Python type (`Address(1)`) stays a `TypeError`. `TA_ERROR_NO_MEMORY`
  from a call that writes into a `ta_buffer` is retried with the length
  it asked for, never raised (`include/tinyactors/cbor.h:54`).

## `decode` and `encode`

- `tinyactors.decode(data)` raises it with `ta_cbor_decode`'s status:
  `ERROR_INVALID` (malformed, not UTF-8, nested deeper than 64, bytes
  after the item) or `ERROR_UNSUPPORTED` (an indefinite length); and
  with `ERROR_INVALID` for a well-formed item outside the value table (a
  tag other than 32, a simple value other than false, true, null and
  undefined, a map key that is not text).
- `tinyactors.encode(value)` raises it with `ERROR_INVALID` for a value
  outside the table (an `int` past 64 bits, a lone surrogate, a cycle,
  nesting past 64, an object of another type: `values.md`).

## Not chosen

`json` and `cbor2` raise `ValueError` and `TypeError` subclasses
(`json.JSONDecodeError`, `cbor2.CBORDecodeError`), so that `except
ValueError` catches a decode error. That is shape the binding could
borrow, and the refutation found the choice not conclusive either way.
One class keeps one place to read what core said, for requests, configs,
values and calls alike; the API review may revisit it.
