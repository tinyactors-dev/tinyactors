# Chat tutorial

This series builds a chat in five steps. Each step is a program you can
run, and each adds one idea to the step before.

1. [A room](1-room/): one actor, events from your program, an I/O processor.
2. [Presence](2-presence/): a user actor per person, states and a timer.
3. [The workspace](3-workspace/): a collection actor makes the rooms.
4. [The status roster](4-roster/): the workspace tracks every user's status.
5. [Hibernation](5-hibernation/): a thousand users, fifty in memory.

The next step will run the chat in a browser.

[The chat](../chat/) is the full application: sockets, a heartbeat, a
database and a store.

## Run it

Each step has a `main.py`. From `tinyactors-cpython`, after `mise run
build`:

```sh
cd examples/chat-tutorial/1-room && PYTHONPATH=../../../build/abi3 python main.py
```

`mise run test` runs `test_tutorial.py`, which runs every step.

The charts use the `python` datamodel.
