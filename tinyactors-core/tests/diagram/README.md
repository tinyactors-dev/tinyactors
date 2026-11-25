# The layout's corpus

Charts, and what `tinyactors graph` answers for each: every box of the
layout (`src/diagram/layout.c`) with its kind and rectangle, every edge with
its route, and the transitions each box lists inside it. `test_tool`
(`test_layout_corpus`) runs `graph` on every `*.scxml` here and compares its
answer with the `.md` beside it, byte for byte. A change that moves one box
by one pixel fails it.

This corpus is the layout's reference. The layout was ported from scxmljs's
`layoutChart` (`@tinyactors/scxmljs` 0.3.0, `src/view/layout.ts`), and on
2026-11-25 the port gave that implementation's layout, box for box and route
for route, for every chart of the repository scxmljs could read (223), given
the same sizes; the answers here were written then. Nothing here depends on
scxmljs any more: the layout is ours, and so is its suite.

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
corpus, and three written for it:

| Chart | What it exercises |
|---|---|
| `document-approval` | parallel reviews, a history state, 24 states, long edges |
| `email-campaign` | a parallel state, self loops, back edges |
| `order-fulfillment` | 15 states in one container, layering, crossings |
| `shopping-cart` | many transitions listed inside boxes, self loops |
| `scheduled-job`, `site-visitor`, `subscription`, `timed`, `chat-user` | back edges, history, labels sliding along their routes |
| `support-ticket` | a deep history, parallel regions, a self loop, a portal, a targetless transition |
| `warehouse-order` | edges between parallel regions (lanes), out to the own container, into a descendant from its container, a long edge, same-layer edges |
| `insurance-claim` | three levels of nesting, a container's transition into its descendant, edges out of nested states |

A new chart needs only its `.scxml` here and `mise run diagram:expect`; give
it a name of its own, an entity of a real system.
