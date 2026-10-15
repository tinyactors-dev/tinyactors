A fixed-capacity ring buffer — use to show how a mailbox or stream is written and read: who owns which index, what's queued, where the next write lands.

```jsx
<RingBuffer name="mailbox" mode="SPSC" capacity={8} head={2} tail={6}
  items={['OPEN', 'TICK', 'TICK', 'CLOSE']} slotSize="64 B" producer="ta_send" consumer="scheduler" />
```

- Occupied slots are ochre (queued/waiting); the next-write slot is dashed pine.
- `head === tail` is empty unless `full`.
- Pair with `MemoryLayout` for the slot struct and `ArchDiagram` for who produces/consumes.
