# UI elements backed by actors — frontend only

Custom elements whose behaviour lives in statecharts running in the page
(the web binding: tinyactors compiled to WebAssembly). There is no server
logic, only static files.

```sh
mise run example:ui                # builds the wasm, serves http://localhost:8000/examples/ui-elements/
mise run example:ui --port 9000    # another port (--port 0: any free one)
```

Run it in `tinyactors-core`. The server (`examples/serve.mjs`) stays in the
foreground and prints the page's URL; Ctrl-C stops it.

Each element (`customElements.define(...)` in `elements.mjs`) owns one actor and
does three things: it clones its `<template>`, forwards DOM events to the actor
as events, and renders the actor's snapshot. The editor's behaviour is in
`editor-chart.mjs`; the smaller examples use `charts.mjs`. Tests drive
these exact charts on the web binding's wasm runtime in Node.

## The centerpiece: an editor whose keymap is a chart

Focus the buffer and type **3dw**, then **u**. Try **i**, type something,
**Escape**, then **.**. Leave **Show states** checked: the five parallel
regions stay visible while you edit. Click a state to see the actual compiled
SCXML, not a separate illustrative chart.

| Region | What to try | What the chart owns |
|---|---|---|
| Mode | `i`, `v`, `d`, Escape; Emacs `C-Space` | Normal, insert, visual, operator-pending, Emacs marked/unmarked, and IME composition. A history state restores the editing mode after composition. |
| Key sequence | `3dw`, `2d3w`, `ci"`, `gg`, `C-x C-s`, `C-u 4 C-f` | A keymap object becomes a trie of **nested states**. Counts compose with operators. Invalid command prefixes abort without leaking counts into the next command. |
| Save | Fail next save → Save → keep typing → Retry save | Dirty → delayed autosave → saving → clean/dirty/failed, independently of editing mode. A delayed server reply captures text and revision at request time; it never replaces the live buffer. |
| Focus | Tab out and back | Focus/blur transitions. Leaving the buffer aborts command prefixes and flushes pending literal insert text. Tab is not trapped. |
| Macro | `qa`, `dw`, `q`, then `@a` | Recording and paced mailbox replay. Registers `a`–`z` store resolved command messages (including counts and inserted text), so replay cannot reinterpret a timed-out `j` as the first half of `jj`. |

**The timeout is a transition.** In insert mode, type one `j`: it waits for
650ms, then inserts the literal character. Type `jj` quickly: it leaves insert
mode without inserting either character. Type `jx`: the pending `j` is
inserted and `x` is processed normally. Each deeper trie node gets a fresh
deadline; leaving its waiting state cancels that timer. The blue timeout bar
only animates the chart's deadline—it does not decide what the key means.

**Keys are hierarchical event names.** A key press is the event
`key.char.letter.d`, `key.char.digit.0`, `key.char.symbol.quote`, `key.ctrl.x`,
`key.meta.less` or `key.special.escape`. An SCXML event descriptor matches every
name it is a prefix of, so one transition on `stroke.char` inserts any
printable key in insert mode, one on `stroke.char.digit` accumulates any count
digit, and one on `stroke` aborts a prefix whatever was typed. No guard looks at
the key. Precedence is document order, from specific to general: after `d`,
`stroke.char.digit.0` (the `0` motion, `d0`, or a count digit after another
one, `d10w`) comes before `stroke.char.digit` (a count), and both come before
`stroke` (abort).

**The keymap is live code, compiled from data.** Switch Vim ↔ Emacs, or change
the insert escape binding to `j k l m` and click **Compile & migrate**. This
uses `system.load(chart)` and `system.migrate(actor)`, not stop/spawn or a buffer
copy into a new editor. The address shown under the form stays identical;
text, cursor, undo/redo and an in-flight save remain intact. Pending prefixes
are canceled (literal insert prefixes are flushed), insert transactions are
finished, and macro recording/replay is stopped at the version boundary.
The new keymap starts in normal/Emacs mode. The form allows 2–32 lowercase
keys; the trie generator has no fixed two- or three-key dispatch table.

**Undo is data; control history is a state.** An insert/change session is one
undo transaction, including text, backspaces and composition. Vim `u` / `C-r`
and Emacs `C-x u` / `C-x r r` traverse the document history. `.` replays the
last Vim edit, including an insert session's text and arrow movements.
SCXML history is used for returning from IME, not confused with an undo stack.

Without the chart, a keydown handler needs to coordinate mode, prefix depth,
two counts, a pending literal, timer cancellation, macro replay and async save
responses. Every exit path—Escape, blur, timeout, mismatch, keymap change—has
to clean up the right subset of flags. Here those lifetimes are state entry,
exit and delayed transitions. Pure text operations remain ordinary JS
datamodel actions; the interpreter decides **when** they run. The element
only normalizes DOM events, displays snapshots, and requests chart migration.
No editor-specific changes to the generic web binding are needed.

### Deliberate scope

This is a teaching editor, not a Vim/Emacs compatibility layer. Vim supports
`h j k l w b 0 $ gg G`, `i a v`, `x`, `d`/`c` + those motions, `dd`/`cc`,
`di"`/`ci"`, `u`, `C-r`, `.`, named macros and `C-s`. Emacs supports character,
word and line movement, numeric arguments, mark/cut, undo/redo and save (see
`keyBindings()` for the exact data). Counts are capped at 999 and undo at 200
transactions. Macro replay is nonrecursive. The cursor is a text offset, not
a full grapheme-aware layout engine; there is no syntax highlighting.

The “server” is a deterministic asynchronous simulation (900ms); failures
are explicit and retryable. Nothing is sent to a network service. IME uses
the browser's composition events; the element leaves preedit rendering to
the browser and commits the result once. Paste inserts in insert/Emacs mode.
Split panes, shared buffer actors, persistence across page reload, server
conflict resolution and complete native-editor key compatibility are out of
scope. Reloading the page resets this playground. The smaller counter demo
below still demonstrates actual hibernation.

## Smaller building blocks

| Element | The chart shows |
|---|---|
| `<ta-toggle>` | guards (`disabled` ignores clicks), events from attributes |
| `<ta-counter>` | bounds; press-and-hold auto-repeat with delayed transitions (`after`) that stops at the bound |
| `<ta-search>` | debouncing by re-entering a state; a slow "server" as a delayed `raise`; stale answers dropped by request id; keyboard navigation |
| `<ta-form>` + `<ta-field>` | actor-to-actor messaging in the page: each field validates itself and reports to its form's actor; submit unlocks when all are valid |
| `<ta-dialog>` | opening and closing as timed states; the element only animates them |
| 200 × `<ta-counter>` | hibernation in the browser: counters scrolled out of view are unloaded from the wasm runtime into a store (`system.hibernate(actor)`). Use one and it wakes up with its value. |

**Show states** also reveals each smaller element's current state. Click a
state to see the SCXML the chart compiles to.

## Tests

`mise run examples:test` includes all files in `test/`. `editor.test.mjs`
uses a deterministic clock on the same wasm runtime: exact timeout boundaries,
deep prefixes, count/operator composition, aborts, insert repeat, macro replay,
undo, mark, composition history, keymap migration and stale/failed saves.
`elements.test.mjs` exercises DOM event wiring, rendering, selection, paste,
composition, binding edits, migration and disconnect/reconnect. Run just these
tests in `tinyactors-core`, after `mise run web:build`, with:

```sh
mise exec -- node --test examples/ui-elements/test/*.test.mjs
```
