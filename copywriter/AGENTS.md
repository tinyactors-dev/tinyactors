# AGENTS.md: copywriter

The tinyactors copywriter: a reviewer that improves a website's copy in
place. Select text on a page, ask Gemini for better copy (typed or spoken),
preview the replacement in the page, keep or undo it, add notes, and export
the kept changes as a work packet a coding agent applies to the site's
sources. Its first user is the maintainer, improving www.tinyactors.dev.

It ships two ways from one code base:

1. **Chrome extension** (Manifest V3, side panel), downloaded as a ZIP from
   https://www.tinyactors.dev/copywriter/ and loaded unpacked.
2. **Embedded widget** on www.tinyactors.dev itself, for a phone: turned on
   per device at /copywriter/, then a launcher on every page opens the same
   panel in a Shadow DOM bottom sheet.

The repository root's `AGENTS.md` applies (commits on `main`, acronyms,
`mise run` tasks in Ruby), except that this is an **application with its own
life cycle**: it depends on the *released* `@tinyactors/wasm` from npm
(pinned in `package.json`), never on the working tree's packages. Commits
are scoped `copywriter` (`feat(copywriter): ...`).

The design packet it was built from is in `spec/` (the "visual reviewer"
specification v1.7: `packet-README.md`, `packet-AGENTS.md`,
`implementation.xml`, `second-pass.xml`, `io-processors.xml`,
`model-tools.xml`, `work-packet.xsd`, `widget-mock.html` and its
screenshots). It is a reference, not a contract: where this file and the
packet disagree, this file wins. Read and query XML with `sxml` (pinned in
`mise.toml`; `sxml xquery`, `sxml xpath`, `sxml validate`), not grep or
one-off Python.

## Decisions (made without the maintainer; change them here)

- **Scope.** One user, a handful of pages, no server. Text annotations,
  Gemini suggestions, in-page preview, keep/undo, notes, page-level chat,
  push-to-talk voice, export. No accounts, no sync, no Live API session
  (push-to-talk records a clip and sends it inline to `generateContent`;
  Hydrogen accepts audio input but is not a Live model).
- **Model.** Default `gemini-4-hydrogen` (Gemini 4 Hydrogen, released
  2026-11-12, successor of 3.8 Flash; function calling and audio input
  supported). The list comes from `GET /v1beta/models` with the user's key,
  filtered to models whose `supportedGenerationMethods` include
  `generateContent` and whose name starts with `gemini-`; until it loads,
  or when it fails, the hard-coded list in `src/core/models.mjs` is used.
- **Key (BYOK).** The user's own Google AI Studio key. Extension:
  `chrome.storage.session` by default, `chrome.storage.local` when
  "Remember on this device" is ticked. Embedded: `sessionStorage`, or
  `localStorage` when remembered. The key never enters a chart's datamodel,
  a log, IndexedDB or an export: actors name it by nothing at all; the
  `model` processor reads it from the credentials adapter per request.
- **One actor host, in the panel's own context**, not a Worker (the packet
  said a Worker; one user does not need it). Extension: the side panel
  page. Embedded: the site's page. Charts run on `datamodel: 'quickjs'`, so
  the extension's CSP needs only `'wasm-unsafe-eval'`.
- **Storage.** IndexedDB (`copywriter` database) in the panel's origin:
  annotations per page URL, settings (not the key), personas. Kept changes
  are re-applied to a page when it is opened again.
- **Export.** A ZIP with `packet.xml` (valid against `spec/work-packet.xsd`,
  copied into the ZIP), `CHANGES.md` (a prompt a coding agent can follow:
  per page, the original text, the replacement, the note, the anchor) and
  `README.md`. On a phone, "Copy for agent" puts `CHANGES.md` on the
  clipboard, since a download there is awkward.
- **Persona.** The default style guide is the website's voice
  (`website/AGENTS.md`, "The voice: short and literal"), editable in
  Settings; more personas can be added.

## Layout

```
copywriter/
  charts/*.scxml          the actors (quickjs datamodel), one file each
  src/core/               platform-free: the host, the processors, Gemini, IndexedDB, export
    reviewer.mjs          createReviewer(adapters) → Reviewer (the facade below)
    models.mjs            the hard-coded model list and the models.list call
    gemini.mjs            generateContent with tools, audio parts
    store.mjs             IndexedDB (and an in-memory store for tests)
    packet.mjs            the work packet: XML, Markdown, ZIP (no dependency)
    charts.mjs            the charts' text, imported by the bundle
  src/ui/                 the panel: plain DOM + Lucide icons, renders ViewState, dispatches commands
    panel.mjs             mountPanel(root, reviewer, {layout: 'sidebar'|'sheet'})
    panel-css.mjs         its stylesheet as a string (a ShadowRoot adopts it; no bundler loader)
  src/platform/dom.mjs    the page adapter's DOM side, shared: anchors, locate, preview/revert of Text nodes, highlights
  src/platform/extension/ manifest.json, service-worker.mjs, content.mjs (page adapter), sidepanel.html/.mjs, mic.html/.mjs
  src/platform/embed/     embed.mjs (launcher, Shadow DOM, page adapter on the live DOM), loader snippet
  site/                   the /copywriter/ page: download the extension, turn the widget on for this device
  test/                   node:test for charts and core; Playwright for the extension and the widget
  mise/tasks/             build, test, serve (Ruby)
  dist/                   build output (not in git): extension/, copywriter-extension.zip, site/
```

## The facade: `Reviewer`

`createReviewer({page, credentials, download, clipboard, store?, wasmURL?, fetch?, recorder?})`
(`src/core/reviewer.mjs`; `store` defaults to IndexedDB, `wasmURL` to the
package's own `tinyactors.wasm`, `fetch` and `recorder` are for tests)
returns:

```js
reviewer.getState()            // ViewState, immutable snapshot
reviewer.subscribe(fn)         // fn(state) on every change; returns unsubscribe
reviewer.dispatch(command)     // a command object below; settles when its work is done (the actors idle, the model answered),
                               // rejects with an Error whose message is for the user (no selection, export with nothing kept, ...)
reviewer.ready()               // a promise: settles once booted, rejects when booting failed
reviewer.close()
```

Commands do not wait for each other (a note goes through while the model
works), only for the boot and a page being opened; `annotate` resolves to the new
annotation's id.

### ViewState

```js
{
  status: 'booting' | 'ready' | 'error', error?: string,   // error also when the store failed to save
  page: { url, title },
  selection: { text, fits } | null,        // the page's current selection, if any; fits false: more than one
                                           // sentence, heading or paragraph (another block, or over 600 characters)
  settings: {
    hasKey, rememberKey, model, models: [{ id, label }],
    personaID, personas: [{ id, name, instructions }],
  },
  annotations: [{                          // this page's, newest first
    id, state,                             // 'draft' | 'asking' | 'suggested' | 'previewing' | 'kept' | 'dismissed' | 'failed'
    original, anchorFound,                 // anchorFound false: the text is no longer on the page
    source,                                // 'typed' | 'model' (made by the page chat)
    note,                                  // the user's note (why)
    suggestions: [{ id, text, rationale, own?, after? }],   // own: the user's text (edit); after: the number of
                                           // messages before it (it follows the reply that brought it)
    chosenID,                              // suggestion previewed or kept
    messages: [{ role: 'user' | 'model', text, at }],
    error?, createdAt, updatedAt,
  }],
  activeID: string | null,
  chat: {                                  // page-level conversation
    messages: [{ role, text, at,
      annotations?,                        // a model message: the ids of the annotations its rewrites made
      proposed? }],                        // true: the model said nothing, the text lists its proposals
    busy, error?,                          // error: the last request failed (until the next message)
  },
  recording: { state: 'idle' | 'recording' | 'transcribing', target: annotationID | 'page' | null },
  totals: { kept, open, pages },           // across every page reviewed
  exporting: 'idle' | 'building' | 'done' | 'failed',
}
```

### Commands

| `type` | fields | what it does |
|---|---|---|
| `annotate` | `note?` | an annotation of the current selection; with a note, it asks the model at once. A selection that does not fit (`fits` false) is refused: "Select one sentence, heading or paragraph." |
| `select` / `deselect` | `id` | the active annotation; highlights its text in the page |
| `ask` | `id`, `text` | a message about one annotation; the model answers and may propose rewrites |
| `suggest` | `id` | more alternatives, no message |
| `edit` | `id`, `text` | the user's own replacement, added as a suggestion and previewed |
| `preview` | `id`, `suggestionID` | the page shows that suggestion in place of the original |
| `keep` | `id` | the previewed suggestion is kept (its approval) |
| `undo` | `id` | the page shows the original again; back to `suggested` |
| `dismiss` / `reopen` / `delete` | `id` | |
| `note` | `id`, `text` | sets the note |
| `chat` | `text` | a page-level message; the model may create annotations (`propose_rewrite`) |
| `record.start` / `record.stop` | `target` | push-to-talk: the clip goes to the model as the next message for `target` |
| `settings.save` | `apiKey?`, `rememberKey?`, `model?`, `personaID?`, `persona?` | a new key is checked with models.list first: one Google refuses is not saved ("That key was not accepted by Google.") |
| `settings.forgetKey` | | |
| `models.refresh` | | |
| `export` | `scope: 'page' \| 'all'`, `to: 'download' \| 'clipboard'` | |

## Platform adapters (what each platform gives the core)

```js
page = {
  info(),                        // → { url, title, lang }
  context(),                     // → { headings: [text], text }  (visible text, at most 20,000 characters)
  selection(),                   // → { text, anchor, fits? } | null   fits false: it spans blocks (p, li, h1-h6, td,
                                 //   blockquote, figcaption, dd, dt, a button's or a link's label) or is over 600 characters
  onSelection(fn),               // fn({ text, anchor, fits? } | null) when the selection changes
  locate(anchor),                // → boolean: the anchor's text is on the page
  anchor?(text),                 // → anchor | null: the first place text is on the page, with its context
                                 //   (the page chat's rewrites name text, not a selection); without it, a bare quote
  preview({ id, anchor, text, reveal? }), // replace the anchor's text with text, reversibly; → boolean. Refused
                                 //   (false) across blocks; the first Text node takes the text, the others lose
                                 //   only their selected part. reveal: scroll it to where the reader sees it
  revert({ id }),                // put the original back; → boolean
  highlight(anchor | null),      // outline the text and scroll it to where the reader sees it (a previewed one:
                                 //   its replacement), or clear
  onNavigate?(fn),               // optional: fn() when the panel follows another page (tab switch, navigation);
                                 // the reviewer ends the old page's actors and starts the new one's
}
anchor = { quote: { exact, prefix, suffix }, selector }   // W3C text quote + CSS path of the nearest element
credentials = { get(), set(key, { remember }), forget(), remembered() }
download(filename, bytes)        // Uint8Array
clipboard(text)
```

Only literal text is ever written into a page (`Text` nodes), never HTML
from the model.

## The platforms

- **Extension.** The reviewer runs in the side panel page. Its `page` is a
  proxy: `chrome.tabs.sendMessage` to the reviewed tab's content script
  (`{cw: 'page', method, args}`), injected with `chrome.scripting` where the
  manifest's did not run (a tab opened before the extension loaded); the
  content script sends `{cw: 'selection'}` back. The panel follows its
  window's active tab and that tab's loads (`onNavigate`); `?tab=<id>` binds
  it to one tab (the tests open it so). Host permissions are every http(s)
  page: one user, no per-site prompts. On a page no content script can
  enter (chrome://), the adapter answers empty and the panel stays usable.
- **Microphone in the side panel.** Chrome shows no permission prompt in a
  side panel, so `getUserMedia` there fails until the extension's origin
  holds the grant. The panel's recorder checks
  `navigator.permissions.query({name: 'microphone'})`; unless it is
  `granted`, it opens `mic.html` in a tab, which asks (the prompt shows
  there) and closes itself; the next press records in the panel.
- **Widget.** `embed.js`, loaded by the site's pages on a device whose
  `localStorage['tinyactors-copywriter']` is `on` (set at /copywriter/).
  It boots the reviewer at once, so kept changes show again before the
  sheet opens. A tap on the sheet clears the page's selection on a phone,
  so the adapter keeps the last non-empty one while the last press was on
  the widget; `page.selection()` uses it up. "Where the reader sees it"
  is the DOM adapter's `band()`: above the sheet, which a preview or a
  highlight lowers to its half height first, and left of the drawer,
  below a sticky header; the extension's content script has no band and
  scrolls the text to the viewport's middle.

## Actors and processors

Charts in `charts/`, one actor per annotation plus one per page session.
Actors reach the world only through `<send type="...">` to I/O processors
the core registers: `page`, `model`, `store`, `ui`, and `spawn` (the
package's `Spawner`: the session actor spawns an annotation actor for each
`propose_rewrite` of the page chat that names `original_text`). A processor
answers with an event (`model.reply`, `model.error`, `page.located`,
`page.previewed`, ...) or fails the send (`error.communication`). Each
chart's header comment lists its sends and answers. The UI is never called
by a chart directly: a chart sends its datamodel to `ui` and the core folds
it into ViewState; it sends the same record to `store`, and an actor is
restored from it (`state` names where it was; a kept one previews its text
again). The audio of a clip never enters a chart: the core keeps it under
an id the `ask` or `chat` event carries, and the `model` processor takes it.

`src/core/charts.mjs` is generated by `mise run charts` from
`charts/*.scxml` and `spec/work-packet.xsd` (string constants, so the
bundle needs no loader and Node.js no bundler); `test/charts.test.mjs`
fails when it is stale. Import `@tinyactors/wasm/init` (the core calls
`init(wasmURL)`), so a bundler need not place the module.

## Tests

`mise run test`: node tests of the charts and core against a fake page and
a fake Gemini (`test/*.test.mjs`), then Playwright (Chromium is
pre-installed; `require('playwright')` works) driving the built extension
(load unpacked, side panel page opened as a tab against a fixture page)
and the widget on a fixture page, with Gemini answered by `page.route`.
Then `mise run check-acronyms` at the root.

## Deploying

`website`'s build copies `copywriter/dist/site/` to `/copywriter/`
(`mise run build` here first). `cd ../website && mise run deploy` puts it on
www.tinyactors.dev. Deploy every change you push.
