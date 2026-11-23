# Step 7: ride out a lost connection

A phone in a tunnel drops its connection for a few seconds. Until now
that made the user offline at once: everyone saw it, and the messages
posted meanwhile never reached the user. The user chart now waits.

## A grace period and the messages it holds

- `connection.lost` (your program, when a socket drops without a
  sign-out) moves the user to `reconnecting`. Its status stays what it
  was, so the others see no change. `disconnect` still means signed out.
- In `reconnecting`, each `message` is kept in `held`.
- `connect` within 30 seconds goes back through the history state
  `resume` to `active` or `away`, as before, and `online`'s entry sends
  the held messages to the screen.
- After 30 seconds, `grace.over` makes the user offline, and the held
  messages go.

Coming back re-enters `active`, which reports its status again: the
workspace now ignores a status that did not change, so nobody is told.

## Run it

```sh
node examples/chat-tutorial/7-reconnect/main.mjs
```

```output
-- bob enters a tunnel, ada posts
ada sees ada in general: where are you?
-- 10 seconds later bob is back
bob sees ada in general: where are you?
-- bob loses the connection for 40 seconds
ada sees bob is offline
ada sees ada in general: gone?
```

Ada saw nothing of bob's first loss, and bob got her message when he was
back. The second loss outlasted the grace period: bob went offline, and
the post after it never reached him.
