PCB-style architecture drawing — use to show which subsystems exist, where their memory lives, and how data moves between them (and which path not to take).

```jsx
<ArchDiagram width={700} height={300}
  zones={[{ x: 160, y: 16, w: 380, h: 260, label: 'process · RAM', meta: '~100 ns' }]}
  blocks={[
    { id: 'app', kind: 'external', name: 'host app', tech: 'C / JS', x: 16, y: 60, w: 120, h: 72, ports: [{ id: 'o', side: 'right' }] },
    { id: 'mb', kind: 'memory', label: 'SPSC ring', name: 'mailbox', x: 190, y: 60, w: 150, h: 72, ports: [{ id: 'i', side: 'left' }] },
  ]}
  wires={[{ from: 'app.o', to: 'mb.i', kind: 'stream', label: 'ta_send', hot: true }]}
/>
```

- Block `kind`: `system` (double frame), `container`, `component` (C4 levels), `external` (dashed), `memory` (hatched RAM region), `store` (disk; double bottom rule).
- Zones say where things live (process, core, disk) with an optional latency `meta`.
- Wire `kind` encodes cost: `call`, `stream` (thick, animated flow), `shared` (dotted, filled pads: zero-copy), `copy` (ochre dashed: memcpy/serialise), `dont` (madder with ✕: the anti-pattern).
- `hot` marks the one path being explained in pine. Use it once per diagram.
- Traces auto-route orthogonally with 45° chamfers; tune with `mid` or `via`. There is no auto-layout: place blocks on the 8px grid by hand.
- Wrap in `<Sheet grid>` with `grid={false}` for a framed plate.
