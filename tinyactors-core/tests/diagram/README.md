# The layout's corpus

Charts, and what `tinyactors graph` answers for each: every box of the
layout (`src/diagram/layout.c`) with its kind and rectangle, every edge with
its route, and the transitions each box lists inside it. `test_tool`
(`test_layout_corpus`) runs `graph` on every `*.scxml` here and compares its
answer with the `.md` beside it, byte for byte. A change that moves one box
by one pixel fails it. The answers are `graph`'s default layout, top to
bottom; a chart with a `<name>.args` beside it is answered with the options
that file holds (`document-overview.args`: folding, a path and a
highlighted state).

This corpus is the layout's reference. The layout was ported from scxmljs's
`layoutChart` (`@tinyactors/scxmljs` 0.3.0, `src/view/layout.ts`), and on
2026-11-25 the port gave that implementation's layout, box for box and route
for route, for every chart of the repository scxmljs could read (223), given
the same sizes. It has changed since, here only: a node is aligned on its
neighbours' ports rather than their boxes' middles (a container's ports are
below its header), a route ends on the outline drawn, a final's pill or
a history state's circle, a leaf that three or more edges arrive at grows
so their arrows keep 12 px apart, down a box grows to 480 px (from
320) to put its outgoing labels side by side, edges through one corridor
keep lanes of their own and an inner segment on another transition's line
moves aside, and a label keeps within its own height of its route where it
can. Folding (scxmljs's
`collapsed` set) is a chart made before the layout (`src/diagram/fold.c`).
Nothing here depends on scxmljs any more: the
layout is ours, and so is its suite.

## Problems

Each answer ends with its drawing's problems (`ta_diagram_check`,
docs/cli.md's "Graph"). `test_layout_corpus` requires none of the hard ones
in any chart: an arrow off its box (`startDetached`, `endDetached`,
`initialDetached`), a bent segment, a box outside its parent or over a
sibling. The others (`notCentred`, `throughBox`, `arrowAlongSide`,
`labelOverlap`, `labelOnBox`, `labelOnEdge`, `labelAstray`, `edgesOverlap`) are faults the layout still
makes; they are part of the answers, so a change that adds one shows in the
diff, and one that removes one is progress.

## Changing the layout

A deliberate change to the layout (or to the sizes `ta_diagram_measure`
estimates) changes these answers. Write them again, and review the diff of
the `.md` files as the change's effect on every chart:

```sh
mise run diagram:expect
git diff tests/diagram
```

## What is here

Charts from the examples, copied so that they do not change under the
corpus, and five written for it:

| Chart | What it exercises |
|---|---|
| `document-approval` | parallel reviews, a history state, 24 states, long edges |
| `email-campaign` | a parallel state, self loops, back edges |
| `order-fulfillment` | 15 states in one container, layering, crossings |
| `shopping-cart` | many transitions listed inside boxes, self loops |
| `scheduled-job`, `site-visitor`, `subscription`, `timed`, `chat-user` | back edges, history, labels sliding along their routes |
| `support-ticket` | a deep history, parallel regions, a self loop, a portal, a targetless transition |
| `warehouse-order` | edges between parallel regions (lanes beside them), out to the own container, into a descendant from its container, a long edge, same-layer edges |
| `insurance-claim` | three levels of nesting, a container's transition into its descendant, edges out of nested states |
| `order-processing` | three parallel regions and nine edges between them, both ways, sharing the lanes beside them |
| `checkout` | five parallel regions wrapped into a grid, edges within a column and across to the other |
| `document-overview` | `document-approval` with `--depth 2 --path --to archiving --highlight escalated`: the parallel review folded into one box, the edges of its hidden states attached to it, a path and a highlighted state |

A new chart needs only its `.scxml` here (and a `.args` for options) and
`mise run diagram:expect`; give it a name of its own, an entity of a real
system.
