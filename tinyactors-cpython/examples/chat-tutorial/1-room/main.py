"""Step 1: a room. Run: python main.py"""

import pathlib

import tinyactors

CHARTS = pathlib.Path(__file__).parent / "charts"


class Client:
    """The I/O processor chat:client. It stands in for the users' screens."""

    def __init__(self):
        self.lines = []

    def receive(self, send):
        for user in send.data["to"]:
            self.lines.append(f"{user} sees {send.data['from']}: {send.data['text']}")


def run():
    client = Client()
    with tinyactors.compile_dir(CHARTS) as compiled:
        host = tinyactors.Host(compiled, system="chat", io_processors={"chat:client": client})
    with host:
        room = host.spawn("room", name="general")
        room.send("join", {"user": "ada"})
        room.send("join", {"user": "bob"})
        room.send("post", {"user": "ada", "text": "hi"})
        host.run_until_idle()
    return client.lines


if __name__ == "__main__":
    for line in run():
        print(line)
