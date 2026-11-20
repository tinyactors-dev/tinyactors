# Chat tutorial

This series builds a chat in five steps. Each step is a program you can
run, and each adds one idea to the step before.

1. [A room](1-room/): one actor, events from your program, an I/O processor.
2. [Presence](2-presence/): a user actor per person, states and a timer.
3. [The workspace](3-workspace/): a collection actor makes the rooms.
4. [The status roster](4-roster/): the workspace tracks every user's status.
5. [Hibernation](5-hibernation/): a thousand users, fifty in memory.

The next step will run the chat in a browser.

[Chat rooms](../chat-rooms/) is the full application: sockets, a
heartbeat, a database and a file store.

## Run it

Each step has a `main.mjs` and a `test.mjs`. From `tinyactors-napi`,
after `mise run build`:

```sh
node examples/chat-tutorial/1-room/main.mjs
mise run examples chat-tutorial     # every step's test, under node and bun
```

The charts use the `ecmascript` datamodel. The tests run each step on
the native datamodel and on quickjs.
