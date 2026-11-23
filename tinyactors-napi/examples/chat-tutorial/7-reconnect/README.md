# Step 7: ride out a lost connection

A phone in a tunnel drops its connection for a few seconds. Until now
that made the user offline at once: everyone saw them leave and come
back, and the messages posted meanwhile never reached them. The user
chart now waits 30 seconds for the connection to return. Meanwhile the
others see the user as away, as Slack shows a person none of whose
clients is connected: they cannot read a message right now.

## A grace period and the messages it holds

- `connection.lost` (your program, when a socket drops without a
  sign-out) moves the user to `reconnecting`, whose entry reports `away`
  to the workspace: the others see it at once. `disconnect` still means
  signed out.
- In `reconnecting`, each `message` is kept in `held`.
- `connect` within 30 seconds goes back through the history state
  `resume` to `active` or `away`, as before, and `online`'s entry sends
  the held messages to the screen.
- After 30 seconds, `grace.over` makes the user offline, and the held
  messages go.

Coming back re-enters `active` or `away`, whose entry reports its status
again. Back to `active`, the others see the user active again. Back to
`away`, the status did not change, and the workspace now ignores a status
that did not change, so nobody is told.

## The roster on return

While bob reconnects, the roster can change: someone signs out. The
workspace sends bob that `roster.update`, and `reconnecting` drops it.
Back, bob's screen would show the old status.

- `connect` from `reconnecting` raises `reconnected`, and `online` sends
  `roster.get` to the workspace on it, which answers with the whole
  roster. The screen replaces the one it has.
- A raised event comes after the states are entered, so `roster.get`
  leaves after the status `active` reports: sent from the transition
  itself, it would get a roster that still shows bob away.
- The user chart does not hold roster changes as it holds messages. The
  workspace keeps the roster, and one question gets it as it is now: no
  second list in the user chart, and no changes replayed in order.

## Run it

```sh
node examples/chat-tutorial/7-reconnect/main.mjs
```

```output
-- bob enters a tunnel, cy signs out, ada posts
ada sees bob is away
cy sees bob is away
ada sees cy is offline
ada sees ada in general: where are you?
-- 10 seconds later bob is back
bob sees ada in general: where are you?
ada sees bob is active
bob sees the roster: ada active, bob active, cy offline
-- bob loses the connection for 40 seconds
ada sees bob is away
ada sees bob is offline
ada sees ada in general: gone?
```

Ada saw bob away while he was in the tunnel and active when he came
back, and bob got her message with the roster that shows cy offline. The
second loss outlasted the grace period: bob went offline, and the post
after it never reached him.
