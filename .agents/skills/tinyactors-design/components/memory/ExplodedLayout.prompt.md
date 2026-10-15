Exploded axonometric of a struct or message. Use in docs to show what's inside, layer by layer.

```jsx
<ExplodedLayout layers={[
  { name: 'header · 16 B', fields: [{ name: 'state', size: 1, kind: 'hot' }, { size: 7, kind: 'pad' }, { name: 'id', size: 8 }] },
  { name: 'payload · 16 B', fields: [{ name: 'flags', size: 2 }, { size: 6, kind: 'pad' }, { name: 'mailbox', size: 8, kind: 'ptr' }] },
  { name: 'padding · 8 B', fields: [{ size: 8, kind: 'pad' }] },
]} />
```
