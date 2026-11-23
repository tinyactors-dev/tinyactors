# Step 8: a room's history

A member who joins a room late saw nothing of what was said before: the
room sent each post only to the members it had then. And a member who
joined twice was added twice, so every post reached them twice. The room
now keeps its posts.

## Numbered posts, kept for the next member

- The room numbers each post (`seq`) and keeps the last 50 in
  `history`. Each `message` carries its number.
- `joined` carries the history, and the user chart passes it to the
  screen with its own `joined`. A screen that already shows a post
  skips it by its number.
- A `join` from a member already in the room adds nobody: the room
  answers it as any join, with the history.
- The user chart keeps `seq` on the messages it holds while
  reconnecting (step 7), so those are numbered too.

## Run it

```sh
node examples/chat-tutorial/8-history/main.mjs
```

```output
-- ada and bob join and talk
ada joins general and sees nothing yet
bob joins general and sees nothing yet
ada sees post 1 from ada: hi bob
bob sees post 1 from ada: hi bob
ada sees post 2 from bob: hi ada
bob sees post 2 from bob: hi ada
-- cat joins late, bob joins again
cat joins general and sees ada: hi bob | bob: hi ada
bob joins general and sees ada: hi bob | bob: hi ada
ada sees post 3 from ada: welcome, cat
bob sees post 3 from ada: welcome, cat
cat sees post 3 from ada: welcome, cat
```

Cat joined after two posts and saw both; bob's second join added him
once, so the third post reached him once.
