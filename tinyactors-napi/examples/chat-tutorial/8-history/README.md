# Step 8: a room's history

A member who joins a room late saw nothing said before: the room sent
each post only to the members it had then. And a member who joined twice
was added twice, so every post reached them twice. A channel's history is
data, so the application stores it; the room only numbers the posts.

## Posts as data, numbered by the room

- The room numbers each post (`seq`), keeps only the last number, and
  sends the post once to `chat:history`: an I/O processor the
  application gives the host, which appends it to the channel's records.
  [main.mjs](main.mjs) keeps them in a `Map`; a real one writes a table.
- Each `message` to a member carries its number.
- `joined` carries the number of the room's last post. The screen reads
  the channel's posts up to it from the store, and shows each numbered
  post once, whether it came from the store or from a `message`.
- A `join` from a member already in the room adds nobody: the room
  answers it as any join.
- The user chart keeps `seq` on the messages it holds while
  reconnecting (step 7).

No chart holds the history: an actor keeps what decides its behaviour,
and the posts decide nothing. The store can page, search and keep
everything; the actors stay small.

## Run it

```sh
node examples/chat-tutorial/8-history/main.mjs
```

```output
-- ada and bob join and talk
ada joins general and reads nothing yet
bob joins general and reads nothing yet
ada sees post 1 from ada: hi bob
bob sees post 1 from ada: hi bob
ada sees post 2 from bob: hi ada
bob sees post 2 from bob: hi ada
-- cat joins late, bob joins again
cat joins general and reads ada: hi bob | bob: hi ada
bob joins general and reads ada: hi bob | bob: hi ada
ada sees post 3 from ada: welcome, cat
bob sees post 3 from ada: welcome, cat
cat sees post 3 from ada: welcome, cat
```

Cat joined after two posts and read both from the store; bob's second
join added him once, so the third post reached him once.
