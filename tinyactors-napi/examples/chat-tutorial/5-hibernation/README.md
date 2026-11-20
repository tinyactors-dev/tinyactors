# Step 5: hibernation

Most users and rooms wait most of the time. The host can keep them out
of memory. The charts do not change in this step. Only the host's
options change.

## A store and a bound

```js
const host = new Host(compiled, { system: 'chat', clock, store, resident: 50, ioProcessors });
```

- `resident` is the most actors the host keeps in memory. After each
  turn it hibernates the actors past the bound. A hibernated actor is a
  blob in the store.
- `store` is an object with `save(session, blob)`, `load(session)` and
  `delete(session)`. [main.mjs](main.mjs) keeps the blobs in a `Map` and
  counts the calls. A real store writes to a database or to files. The
  package has `MemoryStore` and `SQLiteStore`.

An event for a hibernated actor wakes it: the host loads its blob,
deletes it from the store and delivers the event. A user's `idle` timer
stays in the host while the user is hibernated, and wakes the user when
it falls due. The program never wakes an actor itself.

## Run it

```sh
node examples/chat-tutorial/5-hibernation/main.mjs
```

```output
1000 users spawned: 1002 actors, 50 in memory, 952 in the store
100 users connect and join general: 1002 actors, 50 in memory, 952 in the store
u0 posts: 1002 actors, 50 in memory, 952 in the store
100 messages reached the screens
300 seconds pass: 1002 actors, 50 in memory, 952 in the store
14850 roster updates reached the screens
the store saved 1156 blobs and loaded 204
at most 50 actors were in memory at rest
```

The post went to 100 users, and most of them were in the store. The
roster updates woke the online users again and again. The bound held at
each check.

[Hibernation](../../../docs/guide/hibernation.md) has the details.
[Chat rooms](../../chat-rooms/) uses the same pattern with a file store.
