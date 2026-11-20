"""Step 4: the status roster. Run: python main.py"""

import asyncio
import pathlib

import tinyactors

CHARTS = pathlib.Path(__file__).parent / "charts"


class Client:
    def __init__(self):
        self.lines = []

    def receive(self, send):
        data = send.data
        if send.event == "roster":
            entries = ", ".join(f"{user} {status}" for user, status in data["roster"].items())
            self.lines.append(f"{data['to']} sees the roster: {entries}")
        if send.event == "roster.update":
            self.lines.append(f"{data['to']} sees {data['user']} is {data['status']}")


async def run():
    client = Client()
    clock = tinyactors.SimulatedClock()
    processors = {"chat:client": client, "spawn": tinyactors.Spawner()}
    with tinyactors.compile_dir(CHARTS) as compiled:
        host = tinyactors.Host(compiled, system="chat", clock=clock, io_processors=processors)
    async with host:
        workspace = host.spawn("workspace", name="workspace")
        for name in ["ada", "bob", "cat"]:
            host.spawn("user", name="user." + name, params={"name": name})
        await host.idle()

        client.lines.append("-- ada connects")
        host.actor("user.ada").send("connect")
        await host.idle()

        client.lines.append("-- bob connects")
        host.actor("user.bob").send("connect")
        await host.idle()

        client.lines.append("-- 300 seconds pass")
        clock.increment(300)
        await host.idle()

        client.lines.append("-- bob closes the app")
        host.actor("user.bob").send("disconnect")
        await host.idle()

        _, data = await workspace.ask("roster.get")
        client.lines.append(f"the workspace answers: {data['roster']}")
    return client.lines


if __name__ == "__main__":
    for line in asyncio.run(run()):
        print(line)
