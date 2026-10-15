OpenTelemetry trace waterfall — use to show a trace's span hierarchy with timing (inspector panes, docs, debugging views).

```jsx
<SpanTree
  spans={[
    { spanId: 'a', name: 'POST /checkout', service: 'api', start: 0, duration: 42 },
    { spanId: 'b', parentId: 'a', name: 'ta_send PAY', service: 'tinyactors', start: 3, duration: 30,
      events: [{ name: 'transition paying→paid', time: 31 }] },
    { spanId: 'c', parentId: 'b', name: 'stripe.charge', service: 'payments', start: 6, duration: 22, status: 'error' },
  ]}
  selectedId={sel} onSelect={setSel}
/>
```

- Pass spans flat; hierarchy comes from `parentId`. Children sort by start time.
- `status: 'error'` turns the bar and name red and adds an alert glyph. Red is never used as a service color.
- `events` render as thin ticks on the bar (state transitions, logs).
- Chevron or ←/→ collapses a subtree; collapsed rows show `+N` children.
- `serviceColors` pins specific services to colors; otherwise colors are hashed from the service name.
- `treeWidth`, `rowHeight`, `ticks`, `showRuler` tune density. `formatDuration(ms)` is exported for detail panes.
