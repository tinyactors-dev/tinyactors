# Values: the JavaScript side of the value table

How `src/values.c` converts between neutral values (CBOR,
`tinyactors-core/docs/protocol.md`, "Values") and JavaScript values, for
the API (config, requests and replies, event data) and for the native
`"ecmascript"` datamodel's `to_wire` and `from_wire`. Each row gives its
source; a row without one is not decided yet and stays under TODO until
a test decides it.

The same table is the package's public CBOR conversion, the exports
`encode(value): Uint8Array` and `decode(bytes): unknown`
(`docs/api-map.md`; `ta_cbor_encode`, `ta_cbor_decode`), which the
adapter frames the driver's calls with. So the vectors go through this
table both ways: a value mapped wrong the same way when read and when
written still passes them, and only this binding's own tests of the
table, from the rows below and the `ecmascript` lines of `values.txt`,
catch it (AGENTS.md, "The package's CBOR").

Both of Node's datamodels are ECMAScript, so the native one is held to
what quickjs's does: `tinyactors-test/suites/protocol/values.txt`, every
case but those named "not ECMAScript: ...", and protocol.md's paragraph
on the ECMAScript datamodel (the same rules as
`tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c:2592-2602`).

## Decided

| Neutral value | JavaScript | Source |
|---|---|---|
| integers within ±(2^53-1) | `number` | protocol.md, "Values"; values.txt "integers past 2^53 are exact" |
| other integers, -2^64 … 2^64-1 | `BigInt` | the same |
| a `BigInt` | an integer; past 64 bits refused (`TA_ERROR_INVALID`) | values.txt "encode a BigInt is an integer", "invalid a BigInt past 64 bits" |
| a `number` written | an integer when `Number.isSafeInteger`, otherwise a float (-0, NaN, ±Infinity, a number past 2^53); never `napi_get_value_int64`, which saturates silently | protocol.md, "Values"; values.txt "encode non-finite numbers are floats", "encode -0 is a float", "encode a number past 2^53 is a float"; bindings.md, section 9; AGENTS.md, "Rules the suite cannot see" |
| floats | `number`; a float holding an integer comes back as an integer (ECMAScript has no integer type) | values.txt "roundtrip floats", "ECMAScript: a float holding an integer is an integer" |
| ids the API gives (sessions, segments, tokens) | always `BigInt` | AGENTS.md, "Rules the suite cannot see" ("ids are always `BigInt`") |
| text | `string`; a lone surrogate (in a value or a key) is refused (`TA_ERROR_INVALID`), never replaced: Node-API replaces it with U+FFFD silently, so the addon checks `String.prototype.isWellFormed` | bindings.md, section 9; values.txt "invalid a lone surrogate in text", "… in a key"; AGENTS.md, "Rules the suite cannot see" |
| bytes | read as a `Uint8Array`; an `ArrayBuffer`, a typed array or a `DataView` written as its bytes (a view's own bytes) | protocol.md, "Values"; values.txt "encode Uint8Array as bytes", "encode ArrayBuffer as bytes", "encode a view's own bytes" |
| arrays | `Array`; `undefined` in an array is written `null` | values.txt "encode undefined in arrays is null" |
| maps with text keys | objects, keys in the order `Object.keys` gives; a key starting with `$` is an ordinary key; properties that are `undefined` or functions are dropped; a `Date` is written as its JSON form | protocol.md, "Values" ("what `JSON.stringify` keeps"); values.txt "encode keys in the order Object.keys gives", "encode keys starting with $ are plain", "encode undefined properties are dropped", "encode functions are dropped", "encode Date as its JSON form" |
| other objects | what `JSON.stringify` keeps: `toJSON` is called, symbols are dropped as functions are | protocol.md, "Values"; `datamodel_ecmascript.c:2592-2595` |
| `false`, `true`, `null` | themselves | protocol.md, "Values" |
| `undefined` | written `null` at the top; read `undefined` at the top comes back `null` | values.txt "encode undefined at the top is null", "ECMAScript: undefined at the top is null" |
| tag 32 on text, an actor reference | its address, a `string`; read as its text and never written (`values.c` reads tag 32 and writes none) | commit `20189bdc` ("an actor reference is a string", decided 2026-10-12, with its measurements); values.txt "ECMAScript: an actor reference is its address"; bindings.md, section 9 |
| a value shared twice (acyclic) | copied, each time it occurs | values.txt "encode shared (acyclic) references are copied" |
| a cycle | refused (`TA_ERROR_INVALID`) | values.txt "invalid cycles" |
| nesting deeper than 64 | refused (`TA_ERROR_INVALID`); it stops cycles, not a value that shares subtrees, so charts are trusted code (README) | values.txt "invalid nested deeper than 64"; AGENTS.md, "Rules the suite cannot see" |
| anything else (another tag or simple value, an indefinite length, a key that is not text, text that is not UTF-8) | refused when read: `ta_host_deliver` answers `TA_ROUTE_REFUSED` and a `malformed` error envelope; `decode` throws `TinyactorsError` | values.txt, the `malformed` cases |

**Refused** means: `encode` and `decode` throw `TinyactorsError` with
`status` -3 (`TA_ERROR_INVALID`), or -7 (`TA_ERROR_UNSUPPORTED`) for an
indefinite length (`ta_cbor_decode`), and the status's name as the
message; an exception the value throws itself (a getter, `toJSON`)
propagates as it is (`docs/decisions.md`, "Errors"). The native
datamodel's `to_wire` and `from_wire` answer the status instead.

Why a reference is a string and not a class (commit `20189bdc`):
core writes no tag 32 (`ta_cbor_write_tag` has no caller in
`tinyactors-core/src`), both datamodels read a reference as its address
text, and a string compares with `===`, is a `Map` key, prints and
writes JSON as the address, and is what `send` and the charts already
use; a class costs more per value in time and memory (the measurements
are in the root ROADMAP.md).

What crosses between actors is copied through this table; immutable
values may be shared (bindings.md, section 8, "Actors share no mutable
value").

## The API's values

Every value the API takes or gives goes through this table, and nothing
else (`docs/decisions.md`, "The Host's API"):

- **taken:** the `config` of `new Host` (written as CBOR for
  `ta_host_create`), a request's `params`, a spawn's `params` and an
  event's `data` (made a payload of the host's datamodel with its
  `from_wire`), and `encode`'s value;
- **given:** a request's `result`, and the fields of a refused request's or
  config's error (`TinyactorsError`), a `done` record's `data` (the
  payload through its datamodel's `to_wire`), and `decode`'s value;
- **inside the native datamodel:** `to_wire` and `from_wire` are this
  table, and what crosses to another actor (`capture`: `<send>` data,
  `<donedata>`, invoke params) is copied through it (`docs/decisions.md`,
  "The native datamodel").

The application never holds a datamodel's payload: what it gives is copied
in, and what it gets is a copy. `index.d.ts` types a value as `Value`
(`null`, `boolean`, `number`, `bigint`, `string`, `Uint8Array`, arrays and
objects of them), and what `encode` takes as `unknown`, since the table
drops or refuses what is not a `Value`. Ids a reply holds are values like
any other: a session in `actor.list`'s reply is a number when it is a safe
integer (`docs/decisions.md`, "The Host's API", "Ids").
