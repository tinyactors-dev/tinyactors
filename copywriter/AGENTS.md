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
- **Thinking.** Every generateContent request sets `generationConfig:
  {maxOutputTokens: 65536, thinkingConfig}` with the user's level
  (`settings.thinking`, default `high`): `{thinkingLevel, includeThoughts:
  true}` for Gemini 3 and 4, where `xhigh` goes only to the models that
  take it (Gemini 4 Hydrogen and Neon; Carbon, Argon and every 3.x answer
  400, checked against the API on 2026-11-23) and is `high` elsewhere, and
  a refused level is tried once more at `high`; `{thinkingBudget}` (1024,
  8192, 24576) for gemini-2.5, which this key can no longer call (404),
  so that mapping is Google's documented field, unverified here. Parts
  marked `thought` are never the answer: the last summary is the model
  message's `thoughts`, and `seconds` the time the request took. An answer
  cut at `MAX_TOKENS` keeps what it said; one cut before saying anything
  is an error that suggests a lower level.
- **Voice.** A clip is transcribed before it is a message: one
  generateContent request with the audio inline, an instruction to
  transcribe verbatim with no commentary, no tools and thinking `low`, on
  the user's model. The transcript is the user's message (`spoken: true`),
  sent to the conversation as text and used as the note in exports.
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
  Settings but never deleted; more personas can be added in Settings or by
  the model (`define_persona`, `use_persona`, from the page chat or an
  annotation's conversation), and every persona but the default can be
  deleted. The system instruction names the active persona, then its
  instructions, and tells the model it may define one when the reviewer
  describes an audience or a voice.

## Layout

```
copywriter/
  charts/*.scxml          the actors (quickjs datamodel), one file each
  src/core/               platform-free: the host, the processors, Gemini, IndexedDB, export
    reviewer.mjs          createReviewer(adapters) → Reviewer (the facade below)
    models.mjs            the hard-coded model list and the models.list call
    gemini.mjs            generateContent with tools and thinking, transcription
    store.mjs             IndexedDB (and an in-memory store for tests)
    packet.mjs            the work packet: XML, Markdown, ZIP (no dependency)
    charts.mjs            the charts' text, imported by the bundle
  src/ui/                 the panel: plain DOM + Lucide icons, renders ViewState, dispatches commands
    panel.mjs             mountPanel(root, reviewer, {layout: 'sidebar'|'sheet'}); its scrolling area is a div, so the
                          host page keeps its one <main>
    panel-css.mjs         its stylesheet as a string (a ShadowRoot adopts it; no bundler loader)
                          The thread: a transcript is the user's bubble with a small mic (a bubble
                          of dots while it is transcribed); a reply that thought has a collapsed
                          "Thought for N s"; a persona call is a chip that opens the persona in
                          Settings. Settings: the model, a segmented Thinking control, the persona
                          (delete for any but the default; Use when a chip opened one not in use).
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
    thinking,                              // 'low' | 'medium' | 'high' | 'xhigh'
    personaID, personas: [{ id, name, instructions,
      builtIn?,                            // true: the default, which cannot be deleted
      source? }],                          // 'model': defined by the model (define_persona)
  },
  annotations: [{                          // this page's, newest first
    id, state,                             // 'draft' | 'transcribing' | 'asking' | 'suggested' | 'previewing' | 'kept' | 'dismissed' | 'failed'
    original, anchorFound,                 // anchorFound false: the text is no longer on the page
    source,                                // 'typed' | 'model' (made by the page chat)
    note,                                  // the user's note (why)
    suggestions: [{ id, text, rationale, own?, after? }],   // own: the user's text (edit); after: the number of
                                           // messages before it (it follows the reply that brought it)
    chosenID,                              // suggestion previewed or kept
    messages: [message],                   // below
    error?, createdAt, updatedAt,
  }],
  activeID: string | null,
  chat: {                                  // page-level conversation
    messages: [message],                   // below
    busy, error?,                          // busy while transcribing or asking; error: the last request failed
  },                                       //   (until the next message), or 'Nothing was heard.'
  recording: { state: 'idle' | 'recording' | 'transcribing', target: annotationID | 'page' | null },
  totals: { kept, open, pages },           // across every page reviewed
  exporting: 'idle' | 'building' | 'done' | 'failed',
}

message = {
  role: 'user' | 'model', text, at,
  spoken?,                                 // a user message: true when it is a clip's transcript
  annotations?,                            // a page chat's model message: the ids of the annotations its rewrites made
  proposed?,                               // true: the model said nothing, the text lists its proposals
  personas?: [{ name, action, id? }],      // a model message: its persona calls ('define' | 'use'); id once saved
  thoughts?, seconds?,                     // a model message: the summary of its thinking, and the request's seconds
}
```

`recording.state` is `transcribing` from the end of a clip until its
transcript exists; then the message shows, and the conversation goes on
(`busy`, or the annotation's `asking`).

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
| `record.start` / `record.stop` | `target` | push-to-talk: the clip is transcribed, and the transcript is the next message for `target`; an empty one is not sent ("Nothing was heard.") |
| `settings.save` | `apiKey?`, `rememberKey?`, `model?`, `personaID?`, `persona?`, `thinking?` | a new key is checked with models.list first: one Google refuses is not saved ("That key was not accepted by Google."); `persona` `{id?, name, instructions}` creates or updates one |
| `settings.deletePersona` | `id` | any persona but the default; the active one gives way to the default |
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
  scrolls the text to the viewport's middle.  While the sheet is open on a phone, the widget's host
  adds half a viewport of room after the page, so its last lines can
  scroll up above the sheet. The launcher (44 px) steps aside while the
  page scrolls down and comes back when it scrolls up or stops; with a
  selection that fits it stays, and a tap opens the sheet on a new
  annotation of it.

## Actors and processors

Charts in `charts/`, one actor per annotation plus one per page session.
Actors reach the world only through `<send type="...">` to I/O processors
the core registers: `page`, `model`, `store`, `ui`, `settings` (the
personas the model defines or uses: it saves them and answers
`persona.saved {name, id, action}` or `persona.refused {name, message}`),
and `spawn` (the
package's `Spawner`: the session actor spawns an annotation actor for each
`propose_rewrite` of the page chat that names `original_text`). When the
model answers the page chat with function calls only, the `model`
processor sends their results back once (the model's turn as it came,
then a `functionResponse` per call) and takes the words of that second
answer; failing that, the message lists the proposals (`proposed`). A processor
answers with an event (`model.reply`, `model.error`, `page.located`,
`page.previewed`, ...) or fails the send (`error.communication`). Each
chart's header comment lists its sends and answers. The UI is never called
by a chart directly: a chart sends its datamodel to `ui` and the core folds
it into ViewState; it sends the same record to `store`, and an actor is
restored from it (`state` names where it was; a kept one previews its text
again). The audio of a clip never enters a chart: the core keeps it under
an id the `ask` or `chat` event carries; the actor goes to `transcribing`
and sends `model` a `transcribe` of that id, which answers
`model.transcript {text}`.

The model's tools (`src/core/gemini.mjs`, `FUNCTIONS`): `propose_rewrite
{original_text?, replacement, rationale}`, `set_note {note}`,
`define_persona {name, instructions, use?}` (create or update by name;
`use: true` makes it active) and `use_persona {name}`. A reply's persona
calls go from the actor to `settings`; a reply of persona calls only says
what was saved.

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
