"""tinyactors_mcp: what the MCP suite (tinyactors-test/docs/mcp.md) does not
show: the catalog it serves is the module's, its argument checks, and
Streamable HTTP with a bearer token."""

import asyncio
import json
import unittest
import urllib.error
import urllib.request

import tinyactors

import tinyactors_mcp

ORDER = """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="order" initial="s">
  <state id="s"/>
</scxml>"""


def post(url, body, token=None):
    headers = {"content-type": "application/json", "accept": "application/json, text/event-stream"}
    if token:
        headers["authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, data=json.dumps(body).encode(), headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


class Server(unittest.IsolatedAsyncioTestCase):
    def test_the_catalog_is_the_modules(self):
        self.assertEqual(tinyactors_mcp.CATALOG, json.loads(tinyactors._mcp_tools()))
        names = {tool["name"] for tool in tinyactors_mcp.CATALOG["tools"]}
        self.assertTrue({"snapshot_summary", "rehearse", "deploy"} <= names)

    def test_arguments_are_checked_against_the_schema(self):
        tools = {tool["name"]: tool for tool in tinyactors_mcp.CATALOG["tools"]}
        self.assertEqual(tinyactors_mcp.arguments_of(tools["snapshot_actors"], {"snapshot": "s1"})["limit"], 25)
        with self.assertRaisesRegex(TypeError, "migrate is a boolean"):
            tinyactors_mcp.arguments_of(tools["deploy"], {"system": "s", "draft": "d", "chart": "c", "migrate": "yes"})
        with self.assertRaisesRegex(TypeError, "takes no argument extra"):
            tinyactors_mcp.arguments_of(tools["systems"], {"extra": 1})

    async def test_listen_serves_streamable_http_with_a_bearer_token(self):
        t = tinyactors_mcp.tap()
        with tinyactors.compile_charts([("order.scxml", ORDER)]) as compiled:
            host = tinyactors.Host(compiled, **t.options(system="shop"))
        async with host:
            served = await tinyactors_mcp.create_mcp({"shop": {"host": host, "tap": t}}).listen(token="secret")
            try:
                initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                              "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                         "clientInfo": {"name": "test", "version": "0"}}}
                self.assertEqual(await asyncio.to_thread(post, served.url, initialize), 401)
                self.assertEqual(await asyncio.to_thread(post, served.url, initialize, "wrong"), 401)
                self.assertEqual(await asyncio.to_thread(post, served.url, initialize, "secret"), 200)
            finally:
                await served.close()


if __name__ == "__main__":
    unittest.main()
