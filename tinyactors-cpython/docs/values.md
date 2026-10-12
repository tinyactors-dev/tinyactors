# Values: the Python side of the value table

How `src/values.c` converts between neutral values (CBOR,
`tinyactors-core/docs/protocol.md`, "Values") and Python objects, for the
API (config, requests and replies, event data) and for the `"python"`
datamodel's `to_wire` and `from_wire`. Each row gives its source; a row
without one is not decided yet and stays under TODO until a test decides
it.

The same table is the module's public CBOR conversion,
`tinyactors.encode(value) -> bytes` and `tinyactors.decode(data) ->
object` (`docs/api-map.md`; `ta_cbor_encode`, `ta_cbor_decode`), which
the adapter frames the driver's calls with. So the vectors go through
this table both ways: a value mapped wrong the same way when read and
when written still passes them, and only this binding's own tests of the
table, from the rows below and the `python` lines of `values.txt`, catch
it (AGENTS.md, "The module's CBOR").

The vectors that hold it: `tinyactors-test/suites/protocol/values.txt`.
A datamodel in another language than ECMAScript passes every roundtrip
and malformed case but those named "ECMAScript: ..."; its `python` lines
give the Python value of an `encode` or `invalid` case, for this
binding's own tests of its conversion (`value.encode` is ECMAScript's).

## Decided

| Neutral value | Python | Source |
|---|---|---|
| integers, -2^64 … 2^64-1 | `int`, exactly | protocol.md, "Values"; values.txt "encode a BigInt is an integer" (`python [12345678901234567890, -5, -(2 ** 64)]`) |
| an `int` past 64 bits | refused (`TA_ERROR_INVALID`) | values.txt "invalid a BigInt past 64 bits" (`python 2 ** 64`) |
| `false`, `true` | `False`, `True`; a `bool` is tested before `int`, since `bool` is a subclass of `int` | bindings.md, section 9; AGENTS.md, "Rules the suite cannot see" |
| floats, -0.0, NaN, ±Infinity | `float`; NaN written as `f9 7e00` | protocol.md, "Values"; values.txt "encode non-finite numbers are floats", "encode -0 is a float", "encode a number past 2^53 is a float" |
| text | `str`; a lone surrogate (`'a\udc00b'`, in a value or a key) is refused (`TA_ERROR_INVALID`), never replaced | bindings.md, section 9; values.txt "invalid a lone surrogate in text", "… in a key" |
| bytes | `bytes`, not base64 | bindings.md, section 9; values.txt "encode Uint8Array as bytes" (`python bytes([0, 1, 2, 255])`) |
| arrays | `list` | protocol.md, "Values"; values.txt's `python` lines write arrays as lists (`python [float('nan'), float('inf'), float('-inf')]`) |
| maps with text keys | `dict`, keys in the order written (a `dict` keeps insertion order); a key starting with `$` is an ordinary key | protocol.md, "Values"; values.txt "encode keys starting with $ are plain" |
| `null` | `None` | bindings.md, section 9 |
| `undefined` | `tinyactors.UNDEFINED`, a singleton: Python has one empty value, so `undefined` needs its own, and it stays distinct from `None` | bindings.md, section 9; values.txt "not ECMAScript: undefined stays undefined, at the top", "… in arrays and maps" |
| tag 32 on text, an actor reference | `tinyactors.Address`, which writes tag 32 again | bindings.md, section 9; values.txt "not ECMAScript: an actor reference stays a reference"; the root ROADMAP.md, step 8, M1 ("Python keeps `Address`") |
| a value shared twice (acyclic) | copied, each time it occurs | values.txt "encode shared (acyclic) references are copied" |
| a cycle | refused (`TA_ERROR_INVALID`) | values.txt "invalid cycles" |
| nesting deeper than 64 | refused (`TA_ERROR_INVALID`); it stops cycles, not a value that shares subtrees, so charts are trusted code (README) | values.txt "invalid nested deeper than 64"; AGENTS.md, "Rules the suite cannot see" |
| anything else (another tag or simple value, an indefinite length, a key that is not text, text that is not UTF-8) | refused when read: `ta_host_deliver` answers `TA_ROUTE_REFUSED` and a `malformed` error envelope | values.txt, the `malformed` cases |

What crosses between actors is copied through this table; immutable
values may be shared. A mutable object outside the table fails the
capture with `error.execution`, and a session holding one is not
hibernated (`serialize` answers `TA_ERROR_UNSUPPORTED`). Never pickle
(bindings.md, section 8, "Actors share no mutable value").

## TODO

### Tuples

Whether a `tuple` is written as an array.

### Bytes-like objects

Whether `bytearray` and `memoryview` are written as bytes.

### Other Python objects

What an object outside the table gives when written by the API (a `set`,
a `datetime`, a user class): refused, with which error.

### `Address`

Its constructor, comparison, hashing and `repr`; how the API takes an
actor (an `Address` or its text).
