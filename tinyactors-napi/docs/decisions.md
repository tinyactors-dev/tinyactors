# Decisions

The design decisions the guide (`tinyactors-core/docs/bindings.md`) and
the headers leave open, each with its evidence: a scenario in this
design, and `path:line`, a primary source or a spike
(`.agents/skills/reasoned-analysis`). The commit that records one is the
commit that acts on it.

## Per-environment state is the environment's instance data

**Decision.** The addon's state for one environment (the main thread, or
a worker) is a `ta_napi_module` set with `napi_set_instance_data` at
module init and freed by its finalizer (`src/module.c`); nothing is a C
global (the lint rule `no-mutable-globals`).

**Scenario.** An application runs a host in the main thread and another
in a `Worker`, each loading the addon: a `napi_ref` (the
`TinyactorsError` class, a host's handlers) made in one environment
means nothing in the other, so a global holding one breaks the second
environment.

**Evidence.** A spike (2026-10-12, node v22.23.2 and bun 1.4.1, an addon
built with the pinned clang 22 and `-bundle -undefined dynamic_lookup`):
`napi_set_instance_data` returned `napi_ok` in both runtimes; a counter
in the instance data counted 1, 2 in the main thread, 1 in a worker that
loaded the addon, then 3 in the main thread: each environment has its
own. `napi_type_tag_object` and `napi_check_object_type_tag` told a
tagged object from a plain one in both. `test/load.test.mjs` keeps the
worker half of it.

## Errors: one class, TinyactorsError, with core's fields

**Decision.** What core refuses is thrown as `TinyactorsError`, an
`Error` the addon defines (`src/module.c`; in JavaScript, since a
Node-API class cannot extend `Error`), exported by the package:

- a refused request or config (`ta_host_request`'s reply, the
  `environment->error` of `ta_host_create`): its `message`, `code`,
  `reason` and `path`, unchanged (bindings.md, section 10: "An `error`
  becomes the language's exception, with its `code`, `reason`, `path` and
  `message` unchanged");
- a call that returned a status (`ta_host_spawn`, `ta_cbor_decode`, the
  value table refusing a value): `status`, the `ta_status` of `types.h`
  (-3 for `TA_ERROR_INVALID`), and as its message the status's name in
  `types.h` (`TA_ERROR_INVALID`), since core gives such a call no message
  and the binding writes none of its own (bindings.md, section 13: "No
  ... messages of the binding's own").

`decode` and `encode` refuse with it too: `status` -3
(`TA_ERROR_INVALID`) for bytes that are not one neutral value
(`ta_cbor_decode`'s refusal, or the value table's: another tag, another
simple value, a key that is not text) and for a value that is not one
(a cycle, nesting past 64, a BigInt past 64 bits, a lone surrogate);
-7 (`TA_ERROR_UNSUPPORTED`) for an indefinite length, as
`ta_cbor_decode` answers (`include/tinyactors/cbor.h`, "TA_ERROR_UNSUPPORTED:
indefinite lengths"). An exception the value itself throws (a getter, a
`toJSON`) propagates unchanged.

**Scenario.** An application decodes bytes that arrived on a socket and
matches the failure: with one class and core's status it writes
`error instanceof TinyactorsError && error.status === -3`, the same test
as for a refused spawn; a request's refusal is matched on `code` and
`reason` (requests.md), as the guide says.

**Not chosen.** JavaScript's own classes (`TypeError` as `JSON.stringify`
throws for a cycle, `SyntaxError` as `JSON.parse` for bad input): they
would carry no status, so a caller could match only on the message,
which core does not write. A class per kind of failure: core has one
status space and one error reply shape, so two fields of one class say
it.

**Evidence.** `test/values.test.mjs` ("anything else is refused when
read", "a cycle and nesting deeper than 64 are refused", "an exception
a getter throws while writing propagates").

## Strings are checked as UTF-16 in C

**Decision.** `values.c` reads a string as UTF-16
(`napi_get_value_string_utf16`), refuses a lone surrogate
(`TA_ERROR_INVALID`), and encodes UTF-8 itself, in one pass. It does not
call `String.prototype.isWellFormed` (which AGENTS.md names as the
check): the same test in C costs no call into JavaScript per string, and
the UTF-8 that Node-API would write (`napi_get_value_string_utf8`)
replaces a lone surrogate with U+FFFD silently, so the addon could not
use it unchecked anyway (bindings.md, section 9).

**Evidence.** values.txt "invalid a lone surrogate in text", "... in a
key", and `test/values.test.mjs` ("text is a string; a lone surrogate is
refused when written"), under node and bun.
