# The protocol's records

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

Every record that crosses a ring is defined once, in [`catalog.rb`](catalog.rb),
with a small Ruby DSL ([`dsl.rb`](dsl.rb)). Evaluating the catalog builds a
model: rings, enums, scalar types and messages, with each message's byte layout
computed. [`render.rb`](render.rb) turns the model into other formats:

| Command (in this directory) | Writes |
|---|---|
| `mise run ring-proto:render` | all three files below, into `generated/` |

The generated files are [`MESSAGES.md`](generated/MESSAGES.md), the reference
every document links to; [`messages.json`](generated/messages.json), for tools
such as a test host's decoder; and [`ring_proto.h`](generated/ring_proto.h),
C structs with `_Static_assert`s on their sizes, and one C enum per group of
constants: message codes, common flags, each message's own flags and each
catalog enum. Struct fields stay fixed-width integers, since the size of a C
enum is up to the compiler; a comment names the enum a field holds.

The generated files are committed so the proposal can be read without running
anything. After editing the catalog, render again.

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

How rings carry records is in [reference/ring.md](../reference/ring.md).
Complete-spec requests and bounded structured compile diagnostics are in
[reference/definitions.md](../reference/definitions.md). They use blobs so
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
