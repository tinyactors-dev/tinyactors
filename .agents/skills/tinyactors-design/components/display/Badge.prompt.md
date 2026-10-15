Mono status chip for actor lifecycle — the only place state colors appear as fills.

```jsx
<Badge tone="running" />          {/* dot only; tone is the tooltip */}
<Badge tone="running">running</Badge>
<Badge tone="error">crashed</Badge>
<Badge tone="neutral" dot={false}>v0.4.2</Badge>
```
