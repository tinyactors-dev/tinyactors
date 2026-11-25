# refusals

What the server refuses, each with JSON-RPC's code, core's reason and the
path: a line that is not JSON or repeats a key (-32700); a message that is
not a request: an empty batch, not an object, another `jsonrpc`, no
`method`, `params` not an object, an `id` that is not an integer, a string
or null, a member a request does not have (-32600); a `run.*` method the
server does not have and a method core does not have (-32601); params the
schema refuses, `hold` (not served yet) and a chart's index without its
segment (-32602); a chart no image has, another system's address and a
broken image (-32005); a session or a name that does not exist (-32001);
`data` for a chart of the null datamodel (-32602 `range`); `cbor` the
datamodel cannot read (-32006); the clock moved back; files that cannot
be read and charts that do not compile, with their diagnostics; a line past
`--max-message` (-32600 `limit`), skipped to its end; a notification that
fails, answered with nothing; and a batch holding a request, a
notification, something that is not a request and a forwarded request,
answered as one array in its order.
