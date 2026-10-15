# Inspector UI kit

Devtool for a running tinyactors system: supervision tree, the selected actor's current state, and its event log. Original design (no existing product was provided).

- `Toolbar.jsx` — system id, live/paused badge, pause/step/reset, export
- `ActorTree.jsx` — filterable supervision tree with lifecycle badges
- `ChartView.jsx` — chart pane (current state only; statechart drawing removed pending a layout component) and event composer
- `EventLog.jsx` — Events / State (JSON) / Mailbox tabs
- `data.js` — the checkout chart and tree fixtures

Click-thru: send events (tags or typed), watch the active state move, see toasts for failed/final/ignored; Reset opens a confirm dialog.
