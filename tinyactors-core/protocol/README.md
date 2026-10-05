# The protocol's records

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../../docs/proposals/ring-proto/README.md);
the codec (building block C2) is implemented, and nothing in core uses it yet.

Every record that crosses a ring is defined once, in [`catalog.rb`](catalog.rb),
with a small Ruby DSL ([`dsl.rb`](dsl.rb)). Evaluating the catalog builds a
model: rings, enums, scalar types and messages, with each message's byte layout
computed. Two emitters turn the model into other formats:
[`render.rb`](render.rb) writes the reference, and [`codec.rb`](codec.rb) writes
the C codec.

| Command (in tinyactors-core) | Does |
|---|---|
| `mise run protocol` | writes every file below |
| `mise run protocol --check` | fails when a committed file is out of date, or a generator check fails |

The generated files are committed so the proposal can be read without running
anything, and so that the build needs no Ruby step. After editing the catalog,
run the task.

| File | For |
|---|---|
| [`generated/MESSAGES.md`](generated/MESSAGES.md) | the reference every document links to |
| [`generated/messages.json`](generated/messages.json) | tools such as a test host's decoder |
| [`../src/ring_proto.h`](../src/ring_proto.h) | per message: the wire struct (the fixed part byte for byte, with `_Static_assert`s on its size and every field's offset), a data struct (scalars by value, spans as `ta_bytes`), and `size`, `write` and `get`; one C enum per group of constants |
| [`../src/ring_proto.c`](../src/ring_proto.c) | those functions, and the one static const descriptor table behind `ta_msg_table()` |
| [`../tests/ring_proto_cases.h`](../tests/ring_proto_cases.h) | test support: fill, compare, size, write and get for every message through `void` pointers, so one loop covers all of them |

## The codec

The generated code does not judge hostile bytes. That is hand-written, once,
in [`../src/record.h`](../src/record.h) and [`../src/record.c`](../src/record.c):
`ta_record_check` takes a record's bytes and a table of descriptors (where the
spans are, which bytes are enums, which flag bits exist) and either refuses it
with a reason or returns a `ta_record` token whose fixed part is a private
copy. The generated getters decode from that copy, so a host that rewrites
shared memory after the check cannot move a span. The package comments of the
two headers state the contract, the order of the checks, the lifetimes and when
anything allocates (never).

A generated `ta_msg_<name>_size` measures a record with a `ta_layout`, so a
length that overflows is `TA_ERR_NOMEM` and never a wrapped number.
`ta_msg_<name>_write` fills a ring reservation of exactly that size in place,
zeroing every pad and rounding byte, after all its checks, so a refusal stores
nothing. Records are read and written with `ta_mem_copy`, never by casting ring
bytes to structs.

## Stable identifiers

A message is named `<ring>.<name>` (`h2c.event`, `c2h.macrostep_ended`, `tel.trace`) and
has a 16-bit code. `0x01xx` codes are host to core, `0x02xx` core to host, and
`0x03xx` telemetry. The scenarios cite messages by these names. Once a scenario
cites a name, its meaning doesn't change. While the protocol is a sketch, a
record or enum value that nothing cites is deleted outright, and its code may
be used again; nothing outside this repository reads the codes. Codes become
permanent, and retired ones reserved, when the protocol is implemented.
Clarifying a record's name preserves its code and meaning; all catalog and
scenario references move to the new name together.

## Layout rules

- **The common header is 32 bytes:** `kind`, `flags`, `size`, `seq`,
  `session`, `at`.
- **Fixed fields follow the header in the order they are declared**, each at
  its natural alignment.
- **Text and bytes are spans:** a `u32` offset from the record's start and a
  `u32` length, pointing into the tail that follows the fixed part.
- **Every record's size is a multiple of 8.**
- **Values never cross as bytes.** A `vref` names a slot in the binding's value
  table. A `blob` names a byte buffer the host owns, read or written through a
  byte source or sink (core ADR 0090).

How rings carry records is in [reference/ring.md](../../docs/proposals/ring-proto/reference/ring.md).
Complete-spec requests and bounded structured compile diagnostics are in
[reference/definitions.md](../../docs/proposals/ring-proto/reference/definitions.md). They use blobs so
their size does not determine the size of a ring record. Exact spec and
diagnostic encodings remain sketch questions; native data model and value
integrations remain part of the binding.

## The DSL

```ruby
message 'c2h.macrostep_ended', code: 0x0208, summary: 'A macrostep ended' do
  doc 'Everything the macrostep wrote to c2h comes before this record. ...'
  field :macrostep, :macrostep
  field :effects, :u32, 'records this macrostep wrote to c2h, this end report excluded'
  field :mail, :u32, 'envelopes it put in the pool'
  field :status, :macrostep_status
end
```

`ring`, `enum`, `message`, `field`, `flag`, `replies_with`, `see` and `note`
are the whole vocabulary. `Model#check!` rejects duplicate codes and IDs,
unknown rings, and links to messages that don't exist.
