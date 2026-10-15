Drafting-sheet frame (double rule + registration marks, optional construction grid) — use once per view for the hero, a diagram plate or the inspector chart canvas.

```jsx
<Sheet grid style={{ height: 360 }} bodyStyle={{ padding: 32 }}>
  <StateNode name="idle" />
</Sheet>
```

- `grid`: construction grid (`--bg-grid`, 8px minor / 40px major) behind content.
- `marks={false}`: frame only, no margin or registration marks; for embedding in tight panels.
- Use sparingly: one sheet per screen. Cards, not sheets, for repeated items.
