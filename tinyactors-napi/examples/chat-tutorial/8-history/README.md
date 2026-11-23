# Step 8: a room's history

A member who joins a room late saw nothing said before: the room sent
each post only to the members it had then. And a member who joined twice
was added twice, so every post reached them twice. A channel's history is
data, so the application stores it, and the store numbers the posts and
records when each was made.

## Posts as data, numbered and timed by the store

- The room sends each post once to `chat:history`: an I/O processor the
  application gives the host. It appends the post to the channel's
  records, numbers it, records its time by the clock the application
  gives it (here the host's simulated clock) and answers `appended` with
  both. [main.mjs](main.mjs) keeps them in a `Map`; a real one writes a
  table.
- On `appended` the room sends the numbered `message`, with its time
  `at`, to each member.
  The room keeps no counter, so no new chart version or migration can
  start the numbers again.
- If the store fails the send, the room gets `error.communication`. The
  post is then neither stored nor delivered.
- `joined` tells the screen it joined. The screen reads the channel's
  latest 50 posts from the store, and shows each numbered post once,
  whether it came from the store or from a `message`.
- A `join` from a member already in the room adds nobody: the room
  answers it as any join.
- The user chart passes `seq` and `at` on to the screen, and keeps them
  on the messages it holds while reconnecting (step 7).
- A screen shows each post at its `at`: a post held during a lost
  connection, delayed on the way, or read from the store shows when it
  was posted, not when it reached the screen.

No chart holds the history: an actor keeps what decides its behaviour,
and the posts decide nothing. The store can page, search and keep
everything; the actors stay small.

## Catching up when a connection opens

A member who signs out misses the posts made meanwhile. The room still
sends them, but the user chart drops them while offline. So does a member
whose lost connection outlived the 30-second grace (step 7).

- The program opens a member's connection and then sends `connect`. Right
  there, the screen catches up: `opened` in [main.mjs](main.mjs).
- For each room the screen joined, it reads the latest posts from the
  store and shows those it lacks, in the store's order.
- Messages held while reconnecting still arrive after `connect`. The
  screen has read them already, so it shows each number once.

Catching up is the application's job: no chart changes.

## Run it

```sh
node examples/chat-tutorial/8-history/main.mjs
```

```output
-- ada and bob join and talk
ada joins general and reads nothing yet
bob joins general and reads nothing yet
ada sees post 1 from ada at 0:00: hi bob
bob sees post 1 from ada at 0:00: hi bob
ada sees post 2 from bob at 0:30: hi ada
bob sees post 2 from bob at 0:30: hi ada
-- cat joins late, bob joins again
cat joins general and reads ada at 0:00: hi bob | bob at 0:30: hi ada
bob joins general and reads ada at 0:00: hi bob | bob at 0:30: hi ada
ada sees post 3 from ada at 2:00: welcome, cat
bob sees post 3 from ada at 2:00: welcome, cat
cat sees post 3 from ada at 2:00: welcome, cat
-- cat signs out, and back in five minutes later
ada sees post 4 from bob at 3:00: lunch at noon
bob sees post 4 from bob at 3:00: lunch at noon
cat connects and reads in general: bob at 3:00: lunch at noon
```

Cat joined after two posts and read both from the store, each at its
time; bob's second join added him once, so the third post reached him
once. Cat was signed out for the fourth post, and read it five minutes
later with the time it was posted.
