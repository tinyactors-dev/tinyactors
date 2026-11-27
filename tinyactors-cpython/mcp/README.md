# tinyactors-mcp

A [Model Context Protocol](https://modelcontextprotocol.io) server over
running [`tinyactors`](../README.md) hosts. An agent connected to it can:

- inspect a running system;
- send events and ask questions;
- read the spans and `<log>` lines the hosts export;
- take snapshots and read every actor in them;
- compile a new version of a chart and rehearse its migration on a copy;
- deploy the version and migrate the running actors.

It is the same server as Node's `@tinyactors/mcp`: the same tools, the same
arguments and the same results, which the MCP suite checks against both
(`tinyactors-test/docs/mcp.md`). The tools are core's catalog
(`tinyactors-core/mcp/tools.json`), which the `tinyactors` module embeds
and returns as JSON text from `tinyactors.mcp_tools()`;
this package is Python over that module's public API and the MCP SDK
(`mcp`), with no native code.

## Serving a host

A handler is fixed when its host is made, so the server reads what the
handlers see through a **tap**. `tap().options(...)` wraps the host's
keywords and keeps a copy of the spans, the log lines, the migrations'
results and the running services. Every handler you give is still called.

```python
import asyncio
import tinyactors
import tinyactors_mcp

async def main():
    t = tinyactors_mcp.tap()
    host = tinyactors.Host(tinyactors.compile_dir("charts"), **t.options(
        system="shop",
        config={"telemetry": {"flushInterval": 1000}},
        services={"payment": start_payment},
    ))
    async with host:
        mcp = tinyactors_mcp.create_mcp(
            {"shop": {"host": host, "tap": t, "sources": {"order": order_scxml}}},
            allow_deploy=True,
        )
        served = await mcp.listen(port=7333, token="...")   # or: await mcp.serve_stdio()
        print(served.url)                                   # http://127.0.0.1:7333/mcp
        await asyncio.Event().wait()

asyncio.run(main())
```

`listen()` serves Streamable HTTP on `127.0.0.1`; `token` is a bearer
token every request must carry. `serve_stdio()` serves MCP's stdio
transport instead, for an agent that starts the program. `server()` gives
the SDK's low-level `Server`, to connect to another transport; every
server of one `create_mcp` shares its snapshots and drafts.

The host runs under asyncio (`async with`), on the same loop as the
server. A copy of a snapshot (`snapshot_variables`, `rehearse`) is a host
restored with stand-ins and driven synchronously on a simulated clock,
closed before the tool returns.

`sources` maps a chart's name to its SCXML. An image holds no source, so
`chart_source` shows only what you give here; `deploy` writes the new text
here.

## The tools

The tools, what they do and what an agent can and cannot do with them are
the Node package's: [`tinyactors-napi/mcp/README.md`](../../tinyactors-napi/mcp/README.md).
The destructive ones are off unless the server allows them
(`allow_deploy`, `allow_terminate`).

## Development

The tests run against the extension built in the directory above
(`mise run build` there):

```sh
uv sync --frozen
PYTHONPATH=../build/abi3:. .venv/bin/python -m unittest discover -s tests
mise run //tinyactors-test:mcp -- .venv/bin/python tests/fixture.py
```
