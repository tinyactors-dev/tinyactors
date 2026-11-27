# The MCP suite

Every language's MCP server is the same server: the same tools (core's
catalog, `tinyactors-core/mcp/tools.json`), the same arguments, and the same
results for the same system (docs/adr/0021 in the repository root;
`tinyactors-core/mcp/README.md`). The MCP suite holds each implementation
to that: a case opens a fixture on a new server, calls its tools as an
agent would, and compares each result with what the case expects.

    mise run //tinyactors-test:mcp -- <command…> [--case <name>]… [--timeout <s>]

The command is a **fixture program** of one implementation, started once
per case in the directory mise was called from. Each case prints `pass` or
`fail` with the first step that differed and the command's standard
error; the exit status is 1 when a case fails or none ran. A binding's
`test` task runs it against its fixture program on each of its runtimes
and datamodels:

| Implementation | Fixture program |
|---|---|
| `@tinyactors/mcp` (Node, Bun) | `tinyactors-napi/mcp/test/fixture.mjs` (`TA_NAPI_DATAMODEL`: `native` or `quickjs`) |
| `tinyactors_mcp` (Python) | `tinyactors-cpython/mcp/tests/fixture.py` |

## The fixture program

It speaks MCP's stdio transport on its standard input and output: one
JSON-RPC 2.0 message per line. A message whose method starts with
`fixture/` is the program's own, answered in order with the result below;
every other message goes to the implementation's MCP server. The runner
sends `fixture/open` first, then MCP's handshake (`initialize` with
protocol version `2025-06-18`, `notifications/initialized`), then the
case's steps, waiting for each reply.

| Method | Params | What the program does | Result |
|---|---|---|---|
| `fixture/open` | the fixture (below) | compiles `charts`, makes a host of `system` with `config`, a simulated clock, the fixture's services and the tap, on the program's datamodel; makes the MCP server over it with `sources` and `options`; turns the host until idle | `{}` |
| `fixture/settle` | none | turns the host until idle | `{}` |
| `fixture/advance` | `ms` | the simulated clock moved by `ms` milliseconds, then idle | `{}` |
| `fixture/spawn` | `chart`, `name`, `params` | spawns an actor of the chart's current version | `{"session"}` |

It exits when its standard input closes.

## Fixtures

`suites/mcp/fixtures/<name>.json`: what a case's server runs.

- `system`, `config`: the host's system id and config (core's, as
  `docs/host-config.md` has it);
- `charts`: `[{"name", "text"}]`, compiled into the host's image;
- `sources`: each chart's SCXML by its name, the server's `sources`
  (`chart_source`, `deploy`);
- `services`: by type, how the fixture's service answers an invocation:
  `silent` (never), `done`, `error` (`error.communication` sent back), or
  the name of an event it sends back;
- `options`: the server's options, by the catalog's names (`allowDeploy`,
  `allowTerminate`); a case's own `options` are merged over them.

## Cases

`suites/mcp/cases/<name>.json`: `{"name", "description", "fixture",
"options"?, "steps"}`. A step is one of:

- `{"fixture": "<method>", "params"?, "save"?}`: a fixture method;
- `{"tools": "catalog"}`: `tools/list` gives exactly the catalog's tools,
  each with its `description`, `inputSchema` and `annotations`;
- `{"call": "<tool>", "arguments"?, "expect"?, "text"?, "isError"?,
  "error"?, "save"?}`: `tools/call`. Without `isError` the result must
  not be an error; with it, it must be, and `error` is a regular
  expression its text must match. `expect` is matched against the
  result's `structuredContent`, `text` against its text.

`save` keeps a value of the result under a name, by its path (`"id"`,
`"actors[0].session"`); an argument that is the string `"$name"` is that
value, and `"${name}"` inside a string its text.

## Expectations

An expectation says what must be there, not everything that is:

- a map: each of its keys is in the result's map and matches; the result
  may have more;
- a list: the result's has as many items, each matching in order;
- a scalar: equal (JSON's numbers, text, `true`, `false`, `null`);
- a matcher, a map of `$` keys: `{"$gt": n}`, `{"$ge": n}`,
  `{"$length": n}`, `{"$type": "string" | "number" | "integer" | "list" |
  "map"}`, `{"$match": "<regex>"}`, `{"$any": true}`.

What differs between languages by design is never expected: the length of
a snapshot's bytes, a draft's or snapshot's id (saved instead), the
number of spans (`$gt` instead), an error's wording beyond the catalog's
and core's (a pattern instead).

## Adding a case

A case for a behaviour every implementation must share: a tool's result
shape, a refusal, a gate, a forwarded request. It passes against every
fixture program in the table before it is committed (each binding's
`test` task runs them). Behaviour one language's server has alone (its
transport's authentication, a promise that never settles) stays in that
server's own tests.
