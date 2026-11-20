"""Step 5: many users, few in memory. Run: python main.py"""

import asyncio
import pathlib

import tinyactors

CHARTS = pathlib.Path(__file__).parent / "charts"


class Store:
    """A store keeps the blobs of hibernated actors. This one keeps them in
    a dict and counts its calls. A real one writes to a database or to files."""

    def __init__(self):
        self.blobs = {}
        self.saves = 0
        self.loads = 0

    def save(self, session, blob):
        self.saves += 1
        self.blobs[session] = blob

    def load(self, session):
        self.loads += 1
        return self.blobs[session]

    def delete(self, session):
        del self.blobs[session]


class Client:
    def __init__(self):
        self.messages = 0
        self.updates = 0

    def receive(self, send):
        if send.event == "message":
            self.messages += 1
        if send.event == "roster.update":
            self.updates += 1


async def run(users=1000, online=100, resident=50):
    client = Client()
    store = Store()
    clock = tinyactors.SimulatedClock()
    processors = {"chat:client": client, "spawn": tinyactors.Spawner()}
    with tinyactors.compile_dir(CHARTS) as compiled:
        host = tinyactors.Host(compiled, system="chat", clock=clock, store=store, resident=resident,
                               io_processors=processors)
    lines = []
    highest = 0

    async def report(what):
        nonlocal highest
        await host.idle()
        stats = (await host.host_stats())["host"]
        in_memory = stats["actors"] - stats["hibernated"]
        highest = max(highest, in_memory)
        lines.append(f"{what}: {stats['actors']} actors, {in_memory} in memory, {len(store.blobs)} in the store")

    async with host:
        workspace = host.spawn("workspace", name="workspace")
        await workspace.ask("room.create", {"name": "general"})
        for n in range(users):
            host.spawn("user", name=f"user.u{n}", params={"name": f"u{n}"})
        await report(f"{users} users spawned")

        for n in range(online):
            user = host.actor(f"user.u{n}")
            user.send("connect")
            user.send("join", {"room": "general"})
        await report(f"{online} users connect and join general")

        host.actor("user.u0").send("post", {"room": "general", "text": "hi"})
        await report("u0 posts")
        lines.append(f"{client.messages} messages reached the screens")

        clock.increment(300)
        await report("300 seconds pass")
        lines.append(f"{client.updates} roster updates reached the screens")

        lines.append(f"the store saved {store.saves} blobs and loaded {store.loads}")
        lines.append(f"at most {highest} actors were in memory at rest")
    return lines


if __name__ == "__main__":
    for line in asyncio.run(run()):
        print(line)
