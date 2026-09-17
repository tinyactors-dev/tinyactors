# tinyactors-test

`tinyactors-test` is the instrumentable host used to verify Tinyactors. It
links the production `tinyactors-core` amalgamation and exposes a persistent
JSON-RPC 2.0 subset over newline-delimited JSON on standard input and output.
Human-readable diagnostics are written to standard error.

The Ruby driver owns scenario descriptions, scheduling choices, assertions,
generation, shrinking, and reports. The C host owns Tinyactors objects, test
adapters, measurements, and calls into the production API.

`rpc.h` and `rpc.c` implement reusable JSON-RPC parsing, dispatch, responses,
errors, and notifications. `server.h` and `server.c` contain the
`tinyactors-test` lifecycle and host-specific method set on top of those
capabilities. The host method set is a libjq `jv` object whose keys are RPC
method names and whose values select native handlers; discovery is derived
from the same table.

The host includes a jq-backed JSON data model. Source expressions and scripts
are jq filters over the actor's JSON root. A condition uses jq truthiness, an
assignment evaluates its expression as a jq location, and a script must produce
exactly one replacement root. Values use Tinyactors' normal retain, release,
and clone contract.

Named SCXML parameters are represented as an ordered JSON array of
`{"name_atom": number, "value": value}` records. This preserves order and
duplicate names while the core's atom handles remain the public boundary.

## Commands

From the repository root:

```text
mise run build
mise run test
```

The initial protocol methods are:

- `rpc.discover`
- `system.ping`
- `json.transform`
- `metrics.snapshot`
- `host.shutdown`

Actor and definition methods will be added as their production implementations
become executable.

See `docs/protocol.md` for framing, lifecycle, and extension rules.

## Vendored dependency

The source release of jq 1.8.2 is stored in `vendor/jq` and statically linked
into the host. Run `mise run vendor-jq` at the repository root to reproduce the
vendor directory from its pinned release archive and SHA-256 checksum.
