# Addresses, values and other systems

What actors in different systems share: how an actor is addressed, how a
value is written when it leaves a process, and how a send reaches another
system. How the systems are connected — threads and a queue, a WebSocket
between a browser and a server, HTTP, a broker — is the application's
choice, which tinyactors does not make.

- **system** — a set of actors sharing one address authority: a `ta_host`
  (one or more `ta_system` segments) with its `systemID`.
- **remote I/O processor** — the I/O processor the host config marks
  `"remote": true`; the application's transport to other systems.

## Addresses

```
address  = "tinyactors://" system "/" local
system   = label *( "." label )                 ; at most 253 characters
label    = alnum *( alnum / "-" ) ; 1-63 characters, lowercase a-z 0-9 "-",
                                   ; not starting or ending with "-"
local    = session / name
session  = "#_scxml_" 1*20DIGIT                 ; 1 .. 2^64-1, no leading zeros
name     = "@" 1*( unreserved / pct-encoded )   ; RFC 3986; the decoded name is UTF-8
```

The general form is **`tinyactors://<system>/<local>`**, where `<local>` is
either a session (`#_scxml_<id>`) or a name (`@<name>`).

- **Session ids** are unsigned 64-bit integers, written in decimal, from 1 to
  2^64-1 (18446744073709551615). Every implementation keeps them lossless: in
  JavaScript as `BigInt` or as strings, never as `Number` (which is exact only
  up to 2^53). Session ids are unique within a system for its lifetime and
  never reused.
- **Names** (`@<name>`) are resolved **by the system that owns them**, never
  by the sender. What a name means is the owner's business: a JS system's
  a boot actor of a system image, an actor spawned with a name. A name that resolves to nothing is an unknown target (*Errors*).
- **System ids** are configured explicitly (the host config's `systemID`);
  two systems that talk must not share one, which is the deployment's to
  ensure. Dots give hierarchy (`eu.orders`), nothing more: core does not
  interpret them.
- Within one system, SCXML's own targets keep working unchanged: `#_parent`,
  `#_<invokeid>`, `#_internal`, `#_scxml_<id>` (a session of this system).
  An event's `_event.origin` is always a full address.

Comparison is exact after parsing: the system id is lowercase by grammar, a
name compares by its decoded bytes.

## Other systems

Core decides one thing about an address: whether its system id is this
host's.

- **A local address** (this host's system id) is delivered by the host to
  the session, or to the live actor of the name, in whichever segment holds
  it, with `_event.origin` the sender's address and `_event.origintype` the
  SCXML event processor (`http://www.w3.org/TR/scxml/#SCXMLEventProcessor`).
  A session or name that does not exist is `error.communication` for the
  sender.
- **Another system's address** is a `TA_EFFECT_SEND` for the remote I/O
  processor, with the full target, the event, the sendid, the payload, the
  trace, and `TA_EFFECT_FIELD_ORIGIN`, the sender's address. Without a
  remote processor the sender gets `error.communication`. A chart that names
  the remote processor's type gets the same decision.
- **What arrives from another system** is an input: the transport resolves
  the local target with `ta_host_resolve` (another system's address is
  `TA_ERROR_INVALID`, one that names nothing `TA_ERROR_STALE`) and calls
  `ta_host_send` with the event's name, sendid, data (a payload of the
  target's datamodel) and `origin`; an origin without an origintype gets
  the SCXML event processor's. A chart's reply to `_event.origin` is then a
  send to another system again.
- **Failure.** A send the transport took needs no answer. One it cannot
  deliver is answered with `TA_EVENT_IN_ERROR_COMMUNICATION` and its sendid,
  and the sender gets `error.communication` (SCXML 6.2). A receiving side
  that cannot make a value of what arrived sends
  `TA_EVENT_IN_ERROR_COMMUNICATION` to the target (SCXML C.1).

What the application owns, then: the transport and its framing, the
encoding of values on the wire, delivery guarantees (ordering, retries,
duplicates), identity across restarts (session ids restart with a fresh
host), trust between systems, and any routing through intermediate
systems.

## Values

Event data that leaves a process is the application's to encode; the
**neutral value** is the encoding tinyactors offers for it, and what the
bindings' codecs and the datamodels' writers share: one CBOR item (RFC
8949), of these kinds:

| CBOR | meaning |
|---|---|
| unsigned and negative integers | integers, -2^64 … 2^64-1, exactly |
| floats (half, single, double) | numbers that are not integers, -0.0, NaN, ±Infinity |
| text strings | text (UTF-8) |
| byte strings | bytes |
| arrays | arrays |
| maps with text keys | objects; a key that is not text makes the value invalid |
| `false`, `true`, `null`, `undefined` | themselves |
| tag 32 on a text string | an actor reference: its address; the receiver turns it into its own reference type, or a string when it has none |

Nothing else is a neutral value: another tag, another simple value, or an
indefinite length is refused. A key starting with `$` is an ordinary key.
Writers use the shortest form of every integer, length and float, and write
NaN as `f9 7e00` (`ta_cbor_writer`); map keys stay in the order written.

Datamodels write their values as neutral values (`to_wire` in the datamodel
vtable; quickjs reads them back with
`ta_datamodel_ecmascript_payload_from_cbor`, a binding with its own codec):
only the datamodel knows a `PyObject *` or a quickjs value. The ECMAScript datamodel writes what
`JSON.stringify` keeps, with numbers, BigInts and bytes exact: an integer
within ±(2^53-1) as an integer and any other number as a float, a BigInt as
an integer (past 64 bits it is not a neutral value), and an `ArrayBuffer`, a
typed array or a `DataView` as bytes; it reads integers past 2^53 as BigInts,
bytes as a `Uint8Array` and a reference as its address. `ta_cbor_diagnostic`
writes a value as text, for logs and traces.

## Conformance

- Address parsing and formatting vectors (valid, invalid, formatted), in
  `tests/unit/test_address.c` (`test_vectors`), against the C parser.
- `tinyactors-test/suites/protocol/values.txt` — neutral value vectors,
  CBOR in hex with its diagnostic notation; run in C
  (`tests/unit/test_datamodel_ecmascript.c`) and by the suite, through the
  echo setup and the remote I/O processor.
- `tests/unit/test_remote.c` — the remote I/O processor: a send to another
  system, an input by address, the reply to its origin, sync, refusals.
