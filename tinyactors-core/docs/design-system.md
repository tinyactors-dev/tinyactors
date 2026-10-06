# Design system — "quiet instrument"

The torture chamber dashboard (`web/torture/`) is an instrument panel for
watching an actor system under stress. It is read for minutes at a time while
numbers move, so the design optimizes for **calm, legible, comparable
numbers**: few colors, generous whitespace, one type family, no decoration
that moves unless data moves.

Built with web standards only: HTML, CSS custom properties, custom elements
that clone `<template>` content into shadow roots, `<canvas>` for charts, and
a WebSocket for live data. No frameworks, no build step.

Tokens live in [`web/torture/tokens.css`](../web/torture/tokens.css); every
component reads them through `var(--…)` (custom properties inherit into
shadow roots), so a theme change is a token change.

## Principles

1. **Data first.** Numbers are the largest, highest-contrast elements; chrome
   (borders, labels, icons) recedes.
2. **One accent.** The accent color means "interactive or selected". Status
   colors are used only for status. Chart series use the data palette.
3. **Rhythm over lines.** Group with spacing before borders; borders are
   hairlines; no drop shadows on resting surfaces.
4. **Stable layout.** Values use tabular numerals and fixed slots so the page
   does not jitter as digits change.
5. **Motion is information.** Only live data animates. Interface transitions
   are ≤ 120 ms and disabled under `prefers-reduced-motion`.

## Typography

| Token | Value | Use |
|---|---|---|
| `--font-sans` | `ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif` | All UI text |
| `--font-mono` | `ui-monospace, "SF Mono", "JetBrains Mono", Menlo, Consolas, monospace` | Log lines, identifiers |
| `--text-xs` | 12px / 16px | Axis ticks, captions, legend |
| `--text-sm` | 13px / 20px | Labels, table cells, buttons |
| `--text-md` | 14px / 22px | Body (base size) |
| `--text-lg` | 16px / 24px | Card titles |
| `--text-xl` | 20px / 28px | Page title |
| `--text-2xl` | 28px / 32px | KPI values |

- Weights: 400 body, 500 labels/buttons, 600 titles and KPI values.
- Numbers: `font-variant-numeric: tabular-nums` everywhere a value updates.
- Labels are sentence case; units follow values in the secondary text color
  (`12.4 k events/s`).
- Letter spacing: `-0.01em` for ≥ 20px, `0.02em` for uppercase eyebrow labels
  (`--text-xs`, used sparingly).

## Spacing and layout

A 4px base grid. Use the scale, never ad-hoc values.

| Token | px | Typical use |
|---|---|---|
| `--space-1` | 4 | icon gaps, tight inline spacing |
| `--space-2` | 8 | control padding (vertical), legend gaps |
| `--space-3` | 12 | control padding (horizontal), table cells |
| `--space-4` | 16 | gap between related items |
| `--space-5` | 24 | card padding, grid gutters |
| `--space-6` | 32 | section spacing |
| `--space-7` | 48 | page top/bottom padding |

- Page: max width `--page-max` (1440px), side padding `--space-5`.
- Grid: CSS grid with `--space-5` gutters; KPI row uses
  `repeat(auto-fit, minmax(180px, 1fr))`; charts use
  `repeat(auto-fit, minmax(420px, 1fr))`.
- Header height 56px; content starts `--space-6` below it.

## Color

Neutral slate scale plus one accent; light and dark themes switch on
`prefers-color-scheme` (and `[data-theme]` overrides).

| Role | Token | Light | Dark |
|---|---|---|---|
| Page background | `--bg` | `#f7f8fa` | `#0e1116` |
| Surface (cards) | `--surface` | `#ffffff` | `#161a21` |
| Raised surface (hover, inputs) | `--surface-2` | `#f1f3f6` | `#1d222b` |
| Hairline border | `--border` | `#e3e6eb` | `#272d38` |
| Primary text | `--text` | `#14171c` | `#e8ebf0` |
| Secondary text | `--text-2` | `#5b6472` | `#9aa4b2` |
| Tertiary text (ticks) | `--text-3` | `#8a93a1` | `#6b7483` |
| Accent | `--accent` | `#3b5bdb` | `#7c94ff` |
| Accent surface | `--accent-soft` | `#eef1fd` | `#1e2547` |
| OK | `--ok` | `#2f9e44` | `#51cf66` |
| Warning | `--warn` | `#e8590c` | `#ffa94d` |
| Danger | `--danger` | `#e03131` | `#ff6b6b` |

Data palette (`--series-1` … `--series-6`), chosen to stay distinguishable
for common color-vision deficiencies and in both themes:

| Token | Light | Dark |
|---|---|---|
| `--series-1` blue | `#4c6ef5` | `#748ffc` |
| `--series-2` teal | `#0ca678` | `#38d9a9` |
| `--series-3` amber | `#f59f00` | `#ffd43b` |
| `--series-4` magenta | `#d6336c` | `#f783ac` |
| `--series-5` violet | `#7950f2` | `#b197fc` |
| `--series-6` gray | `#868e96` | `#adb5bd` |

Series beyond six repeat with a dashed stroke.

Contrast: text tokens meet WCAG AA on `--surface`; never put `--text-3` on
`--surface-2` for essential information.

## Shape and elevation

| Token | Value |
|---|---|
| `--radius-sm` | 6px (buttons, inputs, pills) |
| `--radius-md` | 10px (cards) |
| `--border-width` | 1px |
| `--shadow-overlay` | `0 8px 24px rgb(0 0 0 / 0.12)` — only for floating overlays (tooltips) |

Resting cards have a border and no shadow.

## Components

All components are custom elements (`tb-*`). Each defines its markup in a
`<template id="tb-…">` in `index.html`; the element clones it into its shadow
root and adopts the shared component stylesheet. Components receive data via
properties (not attributes) and render on `requestAnimationFrame`.

| Element | Purpose | Anatomy |
|---|---|---|
| `tb-app` | Shell: owns the WebSocket, state, and layout | header, KPI row, chart grid, side panel |
| `tb-status` | Connection + run state pill | dot (status color) · label |
| `tb-stat` | KPI tile | label (`--text-sm`, `--text-2`) · value (`--text-2xl`, 600, tabular) · unit · sparkline |
| `tb-chart` | Time-series line chart (canvas) | title · legend (toggle series) · plot · hover readout |
| `tb-systems` | Per-system table | name · actors · runnable · events/s · pool bars |
| `tb-controls` | Stream + scenario controls | pause/resume stream, time window, mutation buttons |
| `tb-log` | Event log | monospace lines, newest first, level color dot |
| `tb-meter` | Capacity bar | track (`--surface-2`) · fill (`--accent`, `--warn` ≥ 80%, `--danger` ≥ 95%) |

### Buttons

- Height 32px, padding `0 var(--space-3)`, radius `--radius-sm`, `--text-sm`
  500.
- Default: `--surface` background, `--border` hairline, `--text`.
- Primary: `--accent` background, white text. One primary per region.
- Hover: background `--surface-2` (primary: 8% darker). Active: inset 1px.
- Focus: `outline: 2px solid var(--accent); outline-offset: 2px` (always
  visible for keyboard focus via `:focus-visible`).
- Disabled: 50% opacity, no hover.

### Charts

- Plot background is the card surface; grid lines `--border` at 1px, at most
  4 horizontal lines; no vertical grid.
- Axis ticks `--text-xs` `--text-3`; y-axis values abbreviated (`12.4k`,
  `3.1M`, `512 MB`).
- Lines 1.5px (2px when hovered in the legend), no area fill except for the
  first series at 8% opacity when a chart has a single series.
- Hover: vertical crosshair in `--text-3`, readout lists values at that time
  in legend order.
- Time window: 30 s, 2 min, 10 min; newest data at the right edge.

### Status

- `connected` → `--ok`, `connecting` → `--warn`, `closed` → `--danger`.
- Run state: `running`, `paused`, `finished`.

## Writing

- Titles name the quantity, not the chart type: "Events delivered", not
  "Events chart".
- Units are explicit and per second where rates are shown.
- Mutation buttons use verbs: "Spawn 10k actors", "Kill system beta".
