# Step 6: who is typing

A chat shows who is typing, and a notice per key would flood every
screen. The user chart tells its room at most once every 3 seconds, and
the room tells the other members. Each member's chart shows the notice
until the typist posts, or for 5 seconds after the last one.

## A throttle and an expiry, as timers

- `typing` (from your program, on each key) sends `typing` to the room
  only while `quiet[room]` is false, then sets it and sends itself
  `quiet.over` in 3 seconds. Every key still counts as activity.
- The room sends `others.typing` to each member but the typist.
- A member's chart shows it (`typing` to `chat:client`) and starts a
  5-second `indicator.expired` timer, cancelling the one before. A post
  from the typist cancels it at once (`typing.stopped`).
- Leaving `online` cancels every indicator's timer and takes each
  indicator off this user's screen (`typing.stopped`).

The timer's event is `indicator.expired`, not `typing.expired`: an
SCXML event name matches by its dotted prefix, so a transition on
`typing` would take `typing.expired` too.

## Run it

```sh
node examples/chat-tutorial/6-typing/main.mjs
```

```output
-- ada types three keys
bob sees ada typing in general
cat sees ada typing in general
-- 4 seconds pass, ada types and posts
ada sees ada in general: lunch?
bob sees ada stop typing in general
cat sees ada stop typing in general
bob sees ada in general: lunch?
cat sees ada in general: lunch?
-- bob types, then stops for 6 seconds
ada sees bob typing in general
cat sees bob typing in general
ada sees bob stop typing in general
cat sees bob stop typing in general
-- bob types again, then cat disconnects
ada sees bob typing in general
cat sees bob typing in general
cat sees bob stop typing in general
```

Three keys made one notice. The post ended ada's indicator on every
screen; bob's ended by itself, 5 seconds after his last key. When cat
disconnected, bob's indicator left cat's screen at once; ada still sees
it.
