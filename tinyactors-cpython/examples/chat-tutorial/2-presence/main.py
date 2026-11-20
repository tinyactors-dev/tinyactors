"""Step 2: users with presence. Run: python main.py"""

import pathlib

import tinyactors

CHARTS = pathlib.Path(__file__).parent / "charts"


class Client:
    def __init__(self):
        self.lines = []

    def receive(self, send):
        if send.event == "status":
            self.lines.append(f"{send.data['user']} is {send.data['status']}")
        if send.event == "message":
            self.lines.append(f"{send.data['to']} sees {send.data['from']}: {send.data['text']}")


def run():
    client = Client()
    clock = tinyactors.SimulatedClock()
    with tinyactors.compile_dir(CHARTS) as compiled:
        host = tinyactors.Host(compiled, system="chat", clock=clock, io_processors={"chat:client": client})
    with host:
        room = host.spawn("room", name="room.general")
        for name in ["ada", "bob"]:
            host.spawn("user", name="user." + name, params={"name": name}).send("connect")
            room.send("join", {"user": name})
        host.run_until_idle()

        client.lines.append("-- 300 seconds pass")
        clock.increment(300)

        client.lines.append("-- bob types, ada closes the app")
        host.actor("user.bob").send("activity")
        host.actor("user.ada").send("disconnect")
        room.send("post", {"user": "bob", "text": "anyone?"})
        host.run_until_idle()
    return client.lines


if __name__ == "__main__":
    for line in run():
        print(line)
