# tinyactors run: transcripts

Each directory is a session with `tinyactors run` (docs/cli.md, "Run"):

- `transcript.jsonl`: the client's lines, `> ` and a JSON-RPC message, and
  the server's, `< ` and what it wrote, in the order they come: each client
  line is followed by the lines it caused;
- `arguments`: the command line after `tinyactors run`, when there is one;
- `exit`: the exit status, when it is not 0;
- the charts and the config the arguments name, and a README saying what
  the case shows.

`test_tool` (`test_run_transcripts`) runs the server in the case's
directory with `TZ=UTC`, feeds it the client's lines and requires the
server's byte for byte. `mise run run:schema` validates every line against
the protocol's JSON Schema (`clientMessage` or `serverMessage`). After a
deliberate change to the server, `mise run run:transcripts [case...]`
writes the server's lines again; review the diff.

The transcripts repeat byte for byte: the clock is simulated, the host's
trace ids come from `traceSeed` 0 and nonce 0, and `Math.random` and
`Date` read the datamodel's seed (`--seed`, 1) and the host's clocks.
