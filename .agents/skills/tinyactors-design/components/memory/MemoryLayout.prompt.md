Byte-accurate struct layout drawn on cache-line rows — use to show where fields live, what's hot, and why a layout is fast or wasteful (show the naive struct next to the fixed one).

```jsx
<MemoryLayout name="ta_actor" fields={[
  { name: 'state', type: 'uint32_t', size: 4, kind: 'hot' },
  { name: 'alive', type: 'bool', size: 1, kind: 'hot' },
  { name: 'chart', type: 'const ta_chart *', size: 8, kind: 'ptr' },
  { name: 'id', type: 'uint64_t', size: 8, kind: 'cold' },
]} />
```

- Offsets and padding follow C rules (natural alignment; override with `align`/`offset`; `packed` removes padding).
- Padding is hatched; any field crossing a `lineSize` boundary is outlined in madder and listed in the summary.
- `kind` colours fields by access pattern: `hot`, `warm`, `cold`, `ptr`, `atomic` (cross-core writes — keep off hot lines).
- `table` adds a pahole-style listing with `note`s.
- `bytesPerRow={16}` for small structs; rows still mark each cache line.
