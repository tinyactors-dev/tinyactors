# Step 9: direct messages

Two people who want to talk privately have only channels. Anyone can join
a channel, and the two could make two channels for one pair. A direct
message (DM) is a room of the two, made once for the pair, that nobody
else can join.

## A room of two, made by the workspace

- The user chart sends `dm.open` with the other person's name to the
  workspace. The workspace alone decides which conversations exist, so it
  makes the DM, as it makes the rooms.
- A DM's name is `dm.` and the two names, sorted: `dm.forager.scout`.
  Both sides get one name, so a second `dm.open`, from either side, finds
  it in `dms` and answers with the same DM.
- A channel's name holds no dot, so no channel can take a DM's name. The
  workspace keeps DMs in `dms`, apart from `rooms`, so the channel list
  never shows one.
- The workspace spawns a room through the `Spawner`, with its two members
  and `direct` as params. A DM is the room chart of step 8, so it has a
  channel's history, typing, held messages and catch-up.
- The room refuses a join from anyone but its members when `direct` is
  set, and takes typing and posts from members only, in a channel too.
- A DM with oneself has one member: a place for one's own notes.

## Telling the other person

The other person learns of a DM as they learn the roster (step 4).

- When they come online, the workspace sends `dms`: their DMs, each its
  room and the other person.
- When a DM is made with them while they are online, they get `dms` again.
- The screen reads each new DM's posts from the store, as it does on
  `joined`. It lists a DM once you opened it or a post came, as Slack
  shows a DM to the recipient when the first message arrives.
- The asker gets `dm.opened`, and its screen opens the DM.

## Run it

```sh
node examples/chat-tutorial/9-direct-messages/main.mjs
```

```output
-- scout opens a DM with forager, who is signed out, and posts twice
scout opens dm.forager.scout, the DM with forager, and reads nothing yet
scout sees post 1 in the DM with forager from scout at 0:00: sugar by the oak
scout sees post 2 in the DM with forager from scout at 0:30: can you carry?
-- forager signs in, types and answers
forager has dm.forager.scout, the DM with scout, and reads scout at 0:00: sugar by the oak | scout at 0:30: can you carry?
scout sees forager typing in the DM with forager
forager sees post 3 in the DM with scout from forager at 1:00: on my way
scout sees post 3 in the DM with forager from forager at 1:00: on my way
-- forager opens the DM with scout: the same one
forager opens dm.forager.scout, the DM with scout, and reads scout at 0:00: sugar by the oak | scout at 0:30: can you carry? | forager at 1:00: on my way
-- queen tries to read it
queen gets an error: not a member
the channels: general
```

Forager was signed out for scout's two posts, and read both on signing in.
Forager's `dm.open` answered with the DM scout made, holding all three
posts. Queen's join was refused, and her post reached nobody; the channel
list holds only `general`.
