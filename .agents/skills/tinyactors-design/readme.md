# Tinyactors Design System

## Context
> **Supported languages:** C and JavaScript (Node, Bun). Do not show or imply other bindings in designs or copy.

**Tinyactors** is a foundational building block for developers who want to make actor systems widely available. "Think libghostty, but for actors." It is a small, embeddable, high-performance actor runtime with a C ABI, currently supported from **C** and **JavaScript (Node and Bun)**, with well-defined semantics grounded in **SCXML** and inspired by **Elixir** (processes, mailboxes, supervision) and **XState** (statecharts). The goals: quick verification loops, and semantics precise enough that coding agents can work with it reliably.

**Hallmarks:** millions of actors in a single system; transparent paging of actors in and out of RAM; hundreds of thousands of events per second; OpenTelemetry traces out of the box. Use these as the product's headline claims, in this wording. Don't turn them into more precise figures unless the team supplies them.

Audience: systems and application developers, C and JavaScript (Node/Bun) developers, and AI agents driving actors programmatically.

### Sources
Only the short company description above was provided. There was **no codebase, Figma file, logo, screenshots, or existing product UI**. Everything here is an original direction derived from the brief. The two UI kits are proposals, not recreations.

### Surfaces
- **Website + docs** (`ui_kits/website/`): landing page and a three-column docs page.
- **Inspector** (`ui_kits/inspector/`): a devtool for a running system. Supervision tree, the live statechart, event log / state / mailbox.

## Art direction
### Why ants
An ant is a finite state machine. It follows a pheromone trail and lays one down as it goes, reacting to events from the world around it. It carries out actions: it senses, walks, carries loads and breaks down material. Everything you would expect from a statechart, an ant already does.

Every ant also has a role, and its role determines its behaviour, just as a chart's type does. One ant can't do much alone. Ants in different roles work together in a nest, and the nest as a whole does useful work. Nests talk to other nests, but only by passing messages. That is the system model:

- **ant** = actor
- **role** = chart
- **nest** = system
- **between nests** = message passing only

### Why tiny
Tinyactors are tiny on purpose. The implementation is built for high memory throughput and low memory use, and you pay only for what you use, so you can fit a great many of them anywhere.
- The reference implementation fits about **4 million actors in 1.5 GB of RAM**, all resident.
- Only an actor's own state and its list of pending timers add to that. An actor with little state but pending timers grows to about **1 KB**, which still fits **4 million actors in 4 GB**.

Tiny things band together to move a lot. That is the philosophy, and every design should be able to show it. This is why the system has components for struct layouts and memory diagrams.

### Why pine and drafting
- **Pine green** ties the brand to nature, and the pine tree ties it to the forest where ants live.
- **Mid-century technical drawing** provides the warm browns, graphite and vellum paper. They sit in harmony with the pine.
- **The forest brings logs.** Distribution and persistence in Tinyactors are built around logs and message passing. The browns of the palette are the colour of that wood.
- **Ants are engineers.** They reshape their environment and build elaborate structures, and a nest runs like clockwork. The drafting style is how you draw clockwork: precise lines, states drawn as circles, registration marks, construction grids.

### In practice
- Nature supplies the warmth and engineering supplies the precision. A design should show both: paper, ink and pine, drawn with a draftsman's discipline.
- Show the system, not the single actor. Many small things, neatly arranged and clearly connected, are more on-brand than one hero object.
- Make efficiency visible. Where memory or throughput matters, draw it: bytes, cache lines, queues, logs. Use exact numbers in place of adjectives.
- Metaphors stay light. Use nests, trails and forest in illustration and tone, but name the technical parts correctly: actor, state, event, system.

---

## Content fundamentals
- **Voice:** precise, calm, engineering-first. Say what the thing does in the order a developer would use it: *"Spawn an actor. Send it an event. Assert the state."*
- **Verbs first, runtime vocabulary:** spawn, send, transition, supervise, restart, stop. Use SCXML/actor terms exactly (state, event, transition, final, parallel, mailbox, supervisor). Never swap in a vaguer synonym.
- **Person:** "you" for the reader, rarely "we". No first-person singular.
- **Casing:** sentence case for headings and buttons ("Get started", "Send event"). The product name is always lowercase **tinyactors**. Identifiers, event names and state ids are mono and verbatim (`checkout.1`, `PAY`, `paying`).
- **Errors and status:** facts, then what happened next. *"`checkout.1` crashed in `paying`. Restarted (1/3)."* No "Oops!", no apology.
- **Claims:** concrete and checkable ("Same semantics in C and JavaScript. Defined by SCXML."). No "blazing-fast", "revolutionary", "next-gen", "unleash".
- **Emoji:** never. Unicode is limited to functional glyphs: `→` for transitions, `·` as a separator, `⌘`/`⇧` in key hints.
- **Numbers:** exact with units (`1.2µs`, `3 restarts`, `v0.4.1`).

### Label last
Words are the last tool, not the first. Before adding a label, try type weight, position, spacing, colour, line style or motion. Add text only when none of those can carry the meaning.

- No labels that restate an encoding. If a border, hatch or colour already means "external" or "padding", don't print the word too.
- Values that can be read off a ruler or grid go in tooltips, not on every cell.
- Legends are opt-in. Show one the first time a reader meets a visual vocabulary, then drop it.
- Status badges default to a dot; the tone name goes in the tooltip. Add text only where the state is the point of the row.
- Card eyebrows, tab counts, field hints and code-block language tags are optional. Use one only when it adds something the title, content or context doesn't already say.
- In toolbars, filters and dense rows, a placeholder or icon replaces the field label.

## Visual foundations
- **Vibe:** an architect's drafting table and an automata-theory notebook. Tracing vellum, graphite linework, one pine-green ink. Warm, precise, hand-made. Not a blueprint: never cyan-on-blue, never white-on-navy.
- **Color:** `vellum` (warm paper ramp) for surfaces, `graphite` for text and linework, `pine` as the only accent. Use it **sparingly**: primary buttons, focus, and the active state/configuration. Links, headings and rules stay graphite. Lifecycle: running = pine, waiting = ochre, done = slate, error = madder, stopped = graphite-500, each with a pale `-bg` tint. `--data-1…6` (ochre, slate, clay, rose, graphite, pale pine) for chart series and services; madder stays reserved for errors.
- **Themes:** light vellum on `:root` (default). Dark via `[data-theme="dark"]`: deep umber stock with parchment ink and a lighter pine. Only semantic tokens change.
- **Type:** **Newsreader** (serif) for display, h1, h2 and docs prose (`--type-prose`, 18/1.6). Regular weight, -0.02em tracking; italic for one emphasised word (e.g. "as a *library*"). **IBM Plex Sans** for UI, h3 and panel body. **IBM Plex Mono** for code, identifiers, state names, event names and uppercase labels (11px, +0.12em).
- **Spacing:** 4px base (`--space-1`…`--space-24`). Controls 26/32/40px high. Content max 1120px, prose 680px, sidebar 248px.
- **Drafting motifs (the signature):** (1) the **Sheet**: 1.5px graphite rule, 4px inset hairline, four circle-crosshair **registration marks** in the margin corners. One per view (hero plate, inspector chart canvas). (2) The **construction grid** (`--bg-grid`): 8px minor and 40px major hairlines, used only inside sheets and diagram canvases. (3) **PCB-style architecture drawings** (see below). No title blocks, dimension lines or fig. captions as decoration.
- **Backgrounds:** flat paper. No gradients, no grain or texture, no photography, no illustration. Warmth comes from the paper and ink colors, not effects.
- **Borders:** hairlines do the structural work (`--border-1`, `--border-2` on hover). `--rule` (full graphite) only for sheet frames. Dashed lines mean "zone" (architecture) or a non-owned/external thing. Don't use them decoratively.
- **Corner radii:** square. 1px (chips/kbd), 2px (buttons, inputs, cards, dialogs). Circles only for states, radios, dots; pills only for switches.
- **Depth:** from paper, not light. Four levels (`--paper-0…3`, `.ta-paper-*`): desk (grid), sheet (cards, panels; hairline edge), overlay (vellum, see-through, for live state over structure), loupe (menus, tips). Only the loupe and dialogs cast a shadow (`--shadow-pop`, `--shadow-dialog`). Earlier versions of a thing (restarts, snapshots) stack behind it on registration offsets with crop marks (`Card generations`).
- **Focus:** `.ta-dof` on a container plus `.is-focus` on one child: the rest blur (`--defocus-blur`) and fade (`--defocus-opacity`). One focus per view, following what just happened.
- **Perspective:** real 3D only to explain structure: `ExplodedLayout` for struct and message layers, `MailboxStack` for queues by age, `ArchDiagram tilt` for website heroes. Never in dense inspector UI.
- **Cards:** `--surface-1` (vellum-50) fill, 1px `--border-1`, 2px radius, no shadow. Mono uppercase eyebrow.
- **Active state:** pale pine fill with a pine stroke (`--state-running`, `--state-running-bg`). Statechart drawing is not part of the system yet. A layout-driven component will replace the old StateNode.
- **Transparency / blur:** the sticky site header, the dialog scrim, vellum overlays (`--paper-2`) and defocused items.
- **Motion:** transitions read like state changes. `--ease-step` 80–140ms for UI; `--ease-out` 220–360ms for panels; `--ease-move` / `--dur-move` (700ms) for spatial moves: focus shifts, diving into an architecture block, layers exploding. No bounce or looping decoration. Parallax only on the website: `Sheet parallax` scrolls the grid at half speed behind hero content.
- **Hover:** surfaces go one step darker on paper (`surface-1` → `surface-2`); secondary buttons darken their outline to full graphite; primary goes pine-500 → 600.
- **Press:** primary → pine-700, 1px downward nudge; no scale.
- **Focus:** 2px offset ring in `--focus-ring` (pine).
- **Disabled:** 40–45% opacity, `not-allowed` cursor.
- **Architecture drawings:** drawn like a PCB. Blocks are chips (square, 1.25px graphite, pin pads on edges); memory regions and stores are hatched like copper pours; zones are dashed silkscreen outlines labelled with where they live and their latency. Traces are orthogonal with 45° chamfers. The line style carries the cost: thick animated = stream, dotted with filled pads = shared/zero-copy, ochre dashed = copy, madder dashed with ✕ = don't. One pine `hot` path per drawing. Always pair a "don't" with the right path.
- **Layout:** sticky header on web; the inspector is a full-viewport three-pane layout with the chart on a Sheet. Dense rows (26–32px) in tree, log and span views.

## Iconography
- No icon set was provided. **Substitute: [Lucide](https://lucide.dev)** via CDN (`lucide-static@0.460.0`), rendered as CSS masks so icons take `currentColor`. Flagged as a substitution.
- Use at 16px (UI) or 20px (feature headers), 1.5px-ish stroke, monochrome in `--fg-2`, `--fg-1` on hover. Never filled or multicolor.
- Use through the `Icon` component: `<Icon name="play" />`. Common set: play, pause, skip-forward, square, rotate-ccw, git-branch, network, inbox, activity, radio, terminal, search, copy, settings.
- No emoji. Unicode `→ · ⌘ ⇧` as noted above. No PNG icons, no icon font.

## Brand mark
**Symbol:** `assets/logo.svg` — an ant in dorsal view (ink `#302A26`, pine `#265E47` eyes/mandibles/antenna clubs, on `#FEFDF8`). `assets/logo-mark.svg` is the same ant cropped tight on a transparent background; `assets/logo-mark-mono.svg` is single-colour (`currentColor`). Lockups (see the Wordmark & logo card): horizontal (symbol left, wordmark right), stacked, symbol-only, wordmark-only; on vellum (original colours), pine (paper ink, pine-100 details) or umber (parchment ink, pine-400 details). **Clear space:** keep ¼ of the symbol's height free on every side of any lockup with the symbol. For the wordmark alone, keep ¾ of its font size. Nothing else goes inside that area, including the tile edge. Every part has an `id`, a `data-name` and a `data-pivot` joint for animation. Groups: `#ant` > `#legs` (`#legs-front|middle|hind` pairs), `#body` (`#gaster`, `#petiole`, `#thorax`), `#head` (`#head-capsule`, `#antennae` > `#antenna-left|right` > `-shaft` + `-club`, `#eyes`, `#mandibles`). Classes `side-left`/`side-right`, `leg`, `antenna`, `eye`, `mandible` select across groups. Left/right are the ant's own sides, which match the viewer's in this view.
**Wordmark:** the lowercase word `tinyactors` set in IBM Plex Mono 500, tracking -0.04em, in graphite on vellum, paper on pine, or parchment on umber.

## Fonts
Newsreader (variable, roman + italic), IBM Plex Sans and IBM Plex Mono (400/500/600) are self-hosted in `assets/fonts/` from Google Fonts. These were chosen, not supplied. Replace if the brand has its own typefaces.

---

## Repository
Private repository for internal use. The layout is described in the Index below.
- **Consumers link one file:** `styles.css`. It imports the tokens, fonts and base styles. Component styles are in `components/components.css`.
- **Components:** React function components (`components/**/<Name>.jsx`). Each has a `.d.ts` props contract and a `.prompt.md` usage note. They use React only and style through CSS custom properties.
- **Generated files:** `_ds_bundle.js`, `_ds_manifest.json` and `_adherence.oxlintrc.json` are built from source. Don't edit them by hand.
  - Inside Claude Design, all three regenerate automatically after every change.
  - From a clone, build the bundle so the component cards and UI kits render:
    ```
    npm install
    npm run build        # writes _ds_bundle.js
    ```
    Then serve the folder over HTTP, e.g. `npx serve .`, and open any card or `ui_kits/*/index.html`. The cards fetch the logo and icons, which won't load from `file://`.
  - `scripts/build-bundle.cjs` exports every `components/**/<Name>.jsx` onto `window.TinyactorsDesignSystem_2f489e` and uses the page's global `React`. Add a new component file and it's picked up on the next build.
  - `_ds_manifest.json` and `_adherence.oxlintrc.json` are Claude Design's index and lint rules. Nothing outside Claude Design needs them, so there's no local build for them. The repo ignores the bundle and manifest.
- **Brand assets:** `assets/brand/` is the ready-to-use logo pack. `assets/logo.svg` and `assets/ant-resting.svg` are the rigged masters.
- **Agent skill:** `SKILL.md` makes this folder usable as a Claude Code skill.
- **Placeholder content (intentional):** these are illustrative and invented, not real Tinyactors specs:
  - the JavaScript API in the website samples
  - the architecture, struct and ring buffer examples
  - the hallmark descriptions
- **Fonts:** Newsreader and IBM Plex are under the SIL Open Font License. Lucide icons (ISC) load from a CDN.

## Index
- `styles.css` — entry point; `@import`s only
- `tokens/` — `fonts.css`, `colors.css`, `typography.css`, `spacing.css`, `effects.css`, `base.css`
- `components/components.css` — component class styles
- `guidelines/` — foundation specimen cards (Brand, Colors, Type, Spacing)
- `assets/ant-resting.svg`: resting-pose ant illustration. **Not the logo.** It uses the same ids, classes and pivots as `logo.svg`, so animations work on either file. One difference: `#mandible-left` and `#mandible-right` are groups here, each holding a `-blade` and a `-highlight`.
- `assets/brand/`: logo pack (SVG, PNG and WebP lockups in 4 colourways, favicons, PWA and maskable icons, webmanifest). See `assets/brand/README.md`
- `assets/fonts/` — Newsreader + Plex woff2 files
- `ui_kits/website/` — landing + docs (`index.html`, `Header`, `Hero`, `Features`, `DocsPage`)
- `ui_kits/inspector/` — devtool (`index.html`, `Toolbar`, `ActorTree`, `ChartView`, `EventLog`, `data.js`)
- `thumbnail.html` — project tile
- `SKILL.md` — agent skill entry

### Components
- **core/**: Button, IconButton, Icon, Kbd
- **forms/**: Input, Select, Checkbox, Radio, Switch
- **display/**: Badge, Tag, Card, CodeBlock, Sheet
- **feedback/**: Dialog, Toast, Tooltip
- **navigation/**: Tabs
- **observability/**: SpanTree
- **architecture/**: ArchDiagram (with `tilt` and nested `inner` diagrams)
- **memory/**: MemoryLayout, RingBuffer, ExplodedLayout, MailboxStack

No source defined a component inventory, so this is a standard set sized to a devtool + docs brand.

### Intentional additions
- **ArchDiagram**: PCB-style C4 drawing — blocks with pin pads, memory zones with latency, chamfered traces typed by data movement (call/stream/shared/copy/don't). Requested to explain where memory lives and why it's fast.
- **MemoryLayout**: byte-accurate struct layout on cache lines (C alignment rules, padding, line-straddle warnings). Same reason.
- **RingBuffer**: mailbox/stream slots with head/tail. Same reason.
- **ExplodedLayout**, **MailboxStack**: perspective views for docs, from the depth exploration (`explorations/Depth Sketches.html`).
- **Sheet**: drafting-sheet frame with registration marks and optional construction grid. The brand's signature container.
- **CodeBlock**: mono code with a filename/language header and copy action. Docs and the inspector both need it.
- **SpanTree**: OpenTelemetry trace waterfall (collapsible span tree + duration bars on a shared ruler), after otel-tui/Jaeger. Requested for tracing actor message flow.
- **Kbd**: key hints for the keyboard-driven inspector.
- **Icon**: wrapper for the Lucide CDN substitution.
