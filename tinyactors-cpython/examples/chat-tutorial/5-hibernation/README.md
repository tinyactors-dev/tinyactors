# Step 5: hibernation

Most users and rooms wait most of the time. The host can keep them out
of memory. The charts do not change in this step. Only the host's
options change.

## A store and a bound

```python
host = tinyactors.Host(compiled, system="chat", clock=clock, store=store, resident=50,
                       io_processors=processors)
```

- `resident` is the most actors the host keeps in memory. After each
  turn it hibernates the actors past the bound. A hibernated actor is a
  blob in the store.
- `store` is an object with `save(session, blob)`, `load(session)` and
  `delete(session)`. [main.py](main.py) keeps the blobs in a dict and
  counts the calls. A real store writes to a database or to files. The
  package has `MemoryStore` and `SQLiteStore`.

An event for a hibernated actor wakes it: the host loads its blob,
deletes it from the store and delivers the event. A user's `idle` timer
stays in the host while the user is hibernated, and wakes the user when
it falls due. The program never wakes an actor itself.

## Run it

```sh
python main.py
```

```
1000 users spawned: 1002 actors, 50 in memory, 952 in the store
100 users connect and join general: 1002 actors, 50 in memory, 952 in the store
u0 posts: 1002 actors, 50 in memory, 952 in the store
100 messages reached the screens
300 seconds pass: 1002 actors, 50 in memory, 952 in the store
14850 roster updates reached the screens
the store saved 1260 blobs and loaded 308
at most 50 actors were in memory at rest
```

The post went to 100 users, and most of them were in the store. The
roster updates woke the online users again and again. The bound held at
each check.

The Python README's "Hibernation" section has the details.
[The chat](../../chat/) uses the same pattern with SQLite.
