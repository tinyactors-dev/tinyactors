# Values: the Python side of the value table

How `src/values.c` converts between neutral values (CBOR,
`tinyactors-core/docs/protocol.md`, "Values") and Python objects, for the
API (config, requests and replies, event data) and for the `"python"`
datamodel's `to_wire` and `from_wire`. Each row gives its source; a row
without one is not decided yet and stays under TODO until a test decides
it.

The same table is the module's public CBOR conversion,
`tinyactors.encode(value) -> bytes` and `tinyactors.decode(data) ->
object` (`docs/api-map.md`; core's streaming `ta_cbor_writer` and
`ta_cbor_reader`), which
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
| tag 32 on text, an actor reference | `tinyactors.Address`, which writes tag 32 again | bindings.md, section 9; values.txt "not ECMAScript: an actor reference stays a reference"; commit `20189bdc` ("Python keeps `Address`") |
| a value shared twice (acyclic) | copied, each time it occurs | values.txt "encode shared (acyclic) references are copied" |
| a cycle | refused (`TA_ERROR_INVALID`) | values.txt "invalid cycles" |
| nesting deeper than 64 | refused (`TA_ERROR_INVALID`); it stops cycles, not a value that shares subtrees, so charts are trusted code (README) | values.txt "invalid nested deeper than 64"; AGENTS.md, "Rules the suite cannot see" |
| anything else (another tag or simple value, an indefinite length, a key that is not text, text that is not UTF-8) | refused when read: `ta_host_deliver` answers `TA_ROUTE_REFUSED` and a `malformed` error envelope | values.txt, the `malformed` cases |

What crosses between actors is copied through this table; immutable
values may be shared. A mutable object outside the table fails the
capture with `error.execution`, and a session holding one is not
hibernated (`serialize` answers `TA_ERROR_UNSUPPORTED`). Never pickle
(bindings.md, section 8, "Actors share no mutable value").

## Decided for Python (2026-10-12)

The rows the guide and the vectors leave open, decided with the
reasoned-analysis skill: each proposal was given to a fresh agent to
refute against the repository and Python's own modules, and what is here
survived it (D1 to D5 of that check; D1, the error type, is in
`errors.md`).

| Python | Written as | Read back as | Source |
|---|---|---|---|
| `tuple` | an array | `list` | `json` and `cbor2` do the same; values.txt has no tuple. The type is not kept: a tuple comes back as a list. |
| `bytearray` | bytes | `bytes` | `bytes`' mutable twin. Taking these two only keeps out non-contiguous and many-dimensional buffers (`memoryview`, `array.array`), which have no single meaning as bytes; a buffer-protocol object is refused like any other object (below). `tinyactors.decode` reads its data from any bytes-like object. |
| a subclass of `int`, `float`, `str`, `list` or `dict` (`IntEnum`, `OrderedDict`) | as its base type | the base type | the table's types are tested with `PyLong_Check` and its kind, not the exact type; `bool` is tested before `int`. `Address` is not a `str` subclass, so it is never written as plain text. |
| anything else: `set`, `frozenset`, `datetime`, `Decimal`, `memoryview`, a function, an instance of a user class, a `dict` with a key that is not a `str` | refused: `TinyactorsError` with `status` `ERROR_INVALID` | | `json` refuses `set` and `datetime` too; turning a key into text, as `json` does, would borrow its semantics, and values.txt refuses a key that is not text ("malformed a key that is not text"). "Immutable values may be shared" (above) means the table's own immutable types: a `frozenset` or a `datetime` does not cross between actors either. |
| `tinyactors.Address(text)` | tag 32 on `text` | `Address` | the text is taken as it is, unchecked: core reads tag 32 on any text, and the binding decides no policy (bindings.md, section 2). `str()` gives the text; `==` and `hash` go by the text, and only between `Address` objects (`Address('x') == 'x'` is `False`); `repr` is `Address('…')`; it is immutable. Node's actor reference is a string; Python keeps `Address` because its datamodel has a reference type (root ROADMAP.md, step 8, M1). |
| `tinyactors.UNDEFINED` | `f7` | the same object | one object, which cannot be made again (`type(UNDEFINED)()` is a `TypeError`), and falsy, as `None` and ECMAScript's `undefined` are. |

## Open

- How the API takes an actor: M1's `Host` takes session ids (`int`), as
  core's calls do; whether it also takes an `Address` is for the API
  review.
