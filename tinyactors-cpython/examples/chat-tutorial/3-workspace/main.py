"""Step 3: the workspace decides which rooms exist. Run: python main.py"""

import asyncio
import pathlib

import tinyactors

CHARTS = pathlib.Path(__file__).parent / "charts"


class Client:
    def __init__(self):
        self.lines = []

    def receive(self, send):
        data = send.data
        if send.event == "joined":
            self.lines.append(f"{data['to']} joined {data['room']}")
        if send.event == "error":
            self.lines.append(f"{data['to']} gets an error: {data['reason']}")
        if send.event == "message":
            self.lines.append(f"{data['to']} sees {data['from']} in {data['room']}: {data['text']}")


async def run():
    client = Client()
    processors = {"chat:client": client, "spawn": tinyactors.Spawner()}
    with tinyactors.compile_dir(CHARTS) as compiled:
        host = tinyactors.Host(compiled, system="chat", io_processors=processors)
    async with host:
        workspace = host.spawn("workspace", name="workspace")
        for name in ["ada", "bob"]:
            host.spawn("user", name="user." + name, params={"name": name}).send("connect")
        ada = host.actor("user.ada")
        bob = host.actor("user.bob")

        for name in ["general", "general", "Bad Name"]:
            event, data = await workspace.ask("room.create", {"name": name})
            reason = f" ({data['reason']})" if "reason" in data else ""
            client.lines.append(f"create {name}: {event}{reason}")
        _, data = await workspace.ask("rooms.list")
        client.lines.append("rooms: " + ", ".join(data["rooms"]))

        ada.send("join", {"room": "general"})
        bob.send("join", {"room": "general"})
        bob.send("join", {"room": "random"})
        await host.idle()

        ada.send("post", {"room": "general", "text": "hi"})
        await host.idle()
    return client.lines


if __name__ == "__main__":
    for line in asyncio.run(run()):
        print(line)
