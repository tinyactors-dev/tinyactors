"""The MCP suite's fixture program for tinyactors_mcp
(tinyactors-test/docs/mcp.md): MCP's stdio transport on standard input and
output, with the fixture's methods (fixture/open, settle, advance, spawn)
answered here and every other message given to the server through the
SDK's memory streams."""

import asyncio
import json
import sys

import anyio
import mcp_types as types
import tinyactors
from mcp.shared.message import SessionMessage

import tinyactors_mcp


def write(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def service(answer):
    """A service of the fixture: it answers as the fixture says (docs/mcp.md, "Fixtures")."""
    def start(invocation):
        if answer != "silent":
            def respond():
                if answer == "done":
                    invocation.done()
                else:
                    invocation.send_back("error.communication" if answer == "error" else answer)
            asyncio.get_running_loop().call_soon(respond)
        return object()
    return start


class Fixture:
    def __init__(self):
        self.host = None
        self.clock = None
        self.to_server = None

    async def open(self, params, task_group):
        self.clock = tinyactors.SimulatedClock()
        t = tinyactors_mcp.tap()
        with tinyactors.compile_charts([(c["name"], c["text"]) for c in params["charts"]]) as compiled:
            self.host = tinyactors.Host(compiled, **t.options(
                system=params["system"], config=params.get("config"), clock=self.clock,
                services={kind: service(answer) for kind, answer in params.get("services", {}).items()}))
        await self.host.__aenter__()
        options = params.get("options", {})
        mcp = tinyactors_mcp.create_mcp({params["system"]: {"host": self.host, "tap": t, "sources": dict(params.get("sources", {}))}},
                                        allow_deploy=options.get("allowDeploy", False),
                                        allow_terminate=options.get("allowTerminate", False))
        server = mcp.server()
        self.to_server, from_runner = anyio.create_memory_object_stream(64)
        to_runner, from_server = anyio.create_memory_object_stream(64)

        async def out():
            async for message in from_server:
                write(message.message.model_dump_json(by_alias=True, exclude_none=True))

        task_group.start_soon(server.run, from_runner, to_runner, server.create_initialization_options())
        task_group.start_soon(out)
        await self.host.idle()
        return {}

    async def settle(self, params, task_group):
        await self.host.idle()
        return {}

    async def advance(self, params, task_group):
        self.clock.increment(params["ms"] / 1000)
        await self.host.idle()
        return {}

    async def spawn(self, params, task_group):
        actor = self.host.spawn(params["chart"], name=params.get("name"), params=params.get("params"))
        return {"session": actor.id}


async def main():
    fixture = Fixture()
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader()
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
    async with anyio.create_task_group() as task_group:
        while line := await reader.readline():
            if not line.strip():
                continue
            message = json.loads(line)
            method = message.get("method", "")
            if method.startswith("fixture/"):
                try:
                    result = await getattr(fixture, method.removeprefix("fixture/"))(message.get("params", {}), task_group)
                    write(json.dumps({"jsonrpc": "2.0", "id": message["id"], "result": result}))
                except Exception as error:  # the runner reports it with the case
                    write(json.dumps({"jsonrpc": "2.0", "id": message.get("id"), "error": {"code": -32000, "message": repr(error)}}))
            else:
                await fixture.to_server.send(SessionMessage(types.jsonrpc_message_adapter.validate_json(line, by_name=False)))
        if fixture.to_server is not None:
            await fixture.to_server.aclose()
        if fixture.host is not None:
            await fixture.host.__aexit__(None, None, None)
        task_group.cancel_scope.cancel()


if __name__ == "__main__":
    asyncio.run(main())
