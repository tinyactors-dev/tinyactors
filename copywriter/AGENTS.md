# AGENTS.md: copywriter

The tinyactors copywriter: a reviewer that improves a website's copy in
place. Pick an element on a page (or select text), ask Gemini for better copy (typed or spoken),
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
- **Answer length** (`settings.length`: short, medium, long; default
  short) is an instruction for the model's own words in the page chat and
  an annotation's conversation (short: one or two sentences; medium: a
  short paragraph; long: a thorough answer with reasons), never for the
  rewrites, whose length follows the voice. It sets no token cap:
  thinking counts against `maxOutputTokens` (checked: a cap of 200 at
  high thinking spent 197 tokens thinking and was cut), so a cap would
  cut thinking and function calls first; it stays 65536.
- **Google's built-in tools** (`settings.tools`: `search`, Google Search
  grounding, off by default; `url`, URL context, the model reads pages a
  message names, on; `code`, code execution, off) go in every request
  beside our functions, with `toolConfig.includeServerSideToolInvocations:
  true`, without which Gemini 4 Hydrogen and 3.8 Flash refuse the
  combination (400; checked 2026-11-23, both models, each tool alone and
  all three). A transcription uses none. The web pages an answer was
  grounded on (`groundingMetadata.groundingChunks`) are its message's
  `sources`, also when they come with the follow-up after a reply of
  function calls only (which sends each call's `id` back); the export
  lists an annotation's sources.
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
  annotations per page URL, the page chat's conversations per page URL,
  settings (not the key: the model, thinking, the voice when edited, the
  active audiences), audiences (one list for every page). Kept changes
  are re-applied to a page when it is opened again.
- **Export.** A ZIP with `packet.xml` (valid against `spec/work-packet.xsd`,
  copied into the ZIP), `CHANGES.md` (a prompt a coding agent can follow:
  per page, the original text, the replacement, the note, the anchor) and
  `README.md`. On a phone, "Copy for agent" puts `CHANGES.md` on the
  clipboard, since a download there is awkward.
- **Pick mode.** The main way to choose what to review (the maintainer,
  2026-11-23: selecting text on a phone raises the system's text menu over
  the widget). One tap on the pick button enters it; then a tap or click
  on the page picks the nearest text block (p, li, h1-h6, blockquote,
  figcaption, td, th, dt, dd, a button's, link's or label's text, or any
  element with text of its own) that fits one annotation, outlined in
  pine by an overlay over it, never by the page's own styles, and that is
  the selection (the element's whole text, its CSS path, the text around
  it). The same element again picks its parent block, while that fits.
  While picking, the page's text cannot be selected (no system menu) and
  a tap there opens no link and presses no button; a mouse shows a dashed
  outline of what it would pick; the phone's sheet lowers to its least
  height. It ends with an annotation of the pick (the sheet rises again
  and shows it), the pick button again, or Escape. Selecting text still
  works outside pick mode.
- **Voice and audiences** (the maintainer, 2026-11-23: a persona is who
  reads, not how we write). The **voice** is how to write: the website's
  voice (`website/AGENTS.md`, "The voice: short and literal") by default,
  editable in Settings, never deleted; saved empty it is the default
  again. The **audiences** are who reads: each a name, who they are, what
  they know, what they want from the page, the tasks they came to do and
  their objections. None by default; one or several are active. They are
  added and edited in Settings or by the model (`define_audience`,
  `use_audiences`, from the page chat or an annotation's conversation;
  asked "who is this page for?" it proposes them), and deleted in
  Settings. The system instruction gives the voice, then the active
  audiences, and asks each rewrite's rationale to name the audience need
  it serves; an annotation keeps the names of the audiences its rewrites
  were written for (`audiences`), which the export prints ("Written
  for"). The database's version 2 moved an edited default persona's
  instructions to the voice and dropped the other personas.
- **Reading as an audience** (the maintainer, 2026-11-23). "Read as"
  an audience (from its chip in the list, its row in Settings, "Read as
  all active", or the page chat's `read_as` tool when the reviewer asks
  how a reader would see the page) has the model read the whole page
  (`page.context()`) as that reader, with the voice and the audience's
  description, knowledge, wants, tasks and objections, and report with
  function calls only (`report_finding`, `report_coverage`; one call per
  finding and per item, in one answer). Checked against the API on
  2026-11-23 (`mise run read-eval`: the live home page in Chromium, the
  three starting audiences side by side, Gemini 4 Hydrogen at high
  thinking): 40 to 85 s a reading, 7 or 8 findings each, every anchored
  one on the page (5 to 7 placed), a coverage call for every item. The
  `replacement` parameter is required and empty when nothing on the page
  helps: optional, Gemini left it out of every finding. A **finding** is where
  the reader gets stuck: `objection` (raised or left unanswered),
  `task` (no next step), `doubt` (a claim without proof or source),
  `jargon` (a word they would not know), `missing` (something they want
  that the page lacks); at most eight, each with the page text it attaches
  to (resolved with `page.anchor`; not found, or `missing`: page-level),
  a one-sentence explanation in the reader's terms, a short `need` and
  maybe a rewrite (its first suggestion). Each becomes an ordinary
  annotation (source `audience`), so keep, dismiss, preview and export
  work unchanged; a page-level one is never previewed and is exported as
  an item to add. The **checklist** is the audience's wants, tasks and
  objections split into items (`checklist` in `src/core/audiences.mjs`:
  one per quoted question, per semicolon part and sentence; tasks also at
  commas), each `answered` (with the anchor of the text that answers it),
  `partial` or `missing`; a gap can be made an open item (page-level,
  kind `missing`), and the export lists the rest under "Gaps by audience".
  Reading an audience again replaces its untouched findings (draft or
  suggested, no note, no message, no text of the reviewer's) and keeps
  the rest; a finding the reviewer acted on is not reported again. One
  reading per audience and page is kept (the database's version 3 added
  the `readings` store).
- **Conversations.** The page chat has any number of conversations per
  page; a new one starts empty and the model reads only the open one.
  Each is an actor of `charts/conversation.scxml` while it is open (the
  core ends it and starts another on `chat.new` and `chat.open`), and a
  record in the store. A page opens on its newest. The database's version
  2 made a page's one chat its conversation `c0`.

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
                          "Thought for N s"; an audience call is a chip that opens the audience in
                          Settings. Settings: the model, a segmented Thinking control, the voice,
                          the audiences (each a switch, an editor and delete), Answers (short, medium,
                          long) with a switch per built-in tool. A grounded answer's sources are small
                          numbered links under it.
  src/platform/dom.mjs    the page adapter's DOM side, shared: anchors, locate, preview/revert of Text nodes, highlights
  src/platform/extension/ manifest.json, service-worker.mjs, content.mjs (page adapter), sidepanel.html/.mjs, mic.html/.mjs
  src/platform/embed/     embed.mjs (launcher, Shadow DOM, page adapter on the live DOM), loader snippet
  site/                   the /copywriter/ page: download the extension, turn the widget on for this device
  test/                   node:test for charts and core; Playwright for the extension and the widget
  mise/tasks/             build, test, charts, eval, read-eval (Ruby)
  tools/                  serve.mjs (the panel's preview), eval.mjs and read-eval.mjs (the real Gemini)
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
  selection: { text, fits } | null,        // the page's current selection or pick, if any; fits false: more than one
                                           // sentence, heading or paragraph (another block, or over 600 characters)
  picking,                                 // true in pick mode (pick.start until pick.stop, an annotation, or Escape)
  settings: {
    hasKey, rememberKey, model, models: [{ id, label }],
    thinking,                              // 'low' | 'medium' | 'high' | 'xhigh'
    length,                                // 'short' | 'medium' | 'long': the model's own answers
    tools: { search, url, code },          // Google's built-in tools, each on or off
    voice: { instructions, custom },       // how to write; custom false: the default voice
    audiences: [{ id, name, description, knows?, wants?, tasks?, objections?,
      source? }],                          // who reads; source 'model': defined by the model (define_audience)
    audienceIDs,                           // the active audiences, in order
  },
  annotations: [{                          // this page's, newest first
    id, state,                             // 'draft' | 'transcribing' | 'asking' | 'suggested' | 'previewing' | 'kept' | 'dismissed' | 'failed'
    original, anchorFound,                 // anchorFound false: the text is no longer on the page
    source,                                // 'typed' | 'model' (made by the page chat) | 'audience' (a reading's finding)
    finding?: { audienceID, kind, explanation, need?, section?, item? },  // source 'audience': kind 'objection' |
                                           //   'task' | 'doubt' | 'jargon' | 'missing'; section: the heading a page-level
                                           //   one belongs under; item: {field, status} of the checklist gap it was made of
    pageLevel?,                            // true: no anchor (what the page lacks); never previewed, exported as "add"
    note,                                  // the user's note (why)
    suggestions: [{ id, text, rationale, own?, after? }],   // own: the user's text (edit); after: the number of
                                           // messages before it (it follows the reply that brought it)
    chosenID,                              // suggestion previewed or kept
    audiences,                             // the names of the audiences its last rewrites were written for
    messages: [message],                   // below
    error?, createdAt, updatedAt,
  }],
  readings: [{                             // this page's last reading per saved audience, in Settings' order
    audienceID, state,                     // 'reading' | 'done' | 'failed'
    at,                                    // when it was answered (while reading: when it started)
    items: [{ field, text, status, explanation, anchor?, annotationID? }],
                                           // the checklist: field 'wants' | 'tasks' | 'objections'; status 'answered' |
                                           //   'partial' | 'missing'; anchor: the text that answers it; annotationID:
                                           //   the open item made of it (reading.open), while it exists
    findingIDs,                            // the annotations its findings made, while they exist
    error?, seconds?,
  }],
  activeID: string | null,
  chat: {                                  // the page chat's open conversation
    id, messages: [message],               // below
    conversations: [{ id, title, at }],    // this page's conversations with a message, newest first; title: the
                                           //   first message, cut at 60 characters
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
  audiences?: [{ name, action, id? }],     // a model message: its audience calls ('define' | 'use' | 'read'); id once saved
  sources?: [{ title, uri }],              // a model message: the web pages it was grounded on (search, URL context)
  thoughts?, seconds?,                     // a model message: the summary of its thinking, and the request's seconds
}
```

`recording.state` is `transcribing` from the end of a clip until its
transcript exists; then the message shows, and the conversation goes on
(`busy`, or the annotation's `asking`).

### Commands

| `type` | fields | what it does |
|---|---|---|
| `annotate` | `note?` | an annotation of the current selection or pick; with a note, it asks the model at once; it ends pick mode. A selection that does not fit (`fits` false) is refused: "Select one sentence, heading or paragraph." |
| `pick.start` / `pick.stop` | | pick mode on the page (`page.pick`); refused where the page has none |
| `select` / `deselect` | `id` | the active annotation; highlights its text in the page |
| `ask` | `id`, `text` | a message about one annotation; the model answers and may propose rewrites |
| `suggest` | `id` | more alternatives, no message |
| `edit` | `id`, `text` | the user's own replacement, added as a suggestion and previewed |
| `preview` | `id`, `suggestionID` | the page shows that suggestion in place of the original |
| `keep` | `id` | the previewed suggestion is kept (its approval) |
| `undo` | `id` | the page shows the original again; back to `suggested` |
| `dismiss` / `reopen` / `delete` | `id` | |
| `note` | `id`, `text` | sets the note |
| `chat` | `text` | a message in the open conversation; the model may create annotations (`propose_rewrite`) |
| `chat.new` | | a new, empty conversation (none when the open one is empty) |
| `chat.open` | `id` | opens an earlier conversation of this page |
| `record.start` / `record.stop` | `target` | push-to-talk: the clip is transcribed, and the transcript is the next message for `target`; an empty one is not sent ("Nothing was heard.") |
| `settings.save` | `apiKey?`, `rememberKey?`, `model?`, `thinking?`, `length?`, `tools?` (`{search?, url?, code?}`, merged), `voice?`, `audience?`, `audienceIDs?` | a new key is checked with models.list first: one Google refuses is not saved ("That key was not accepted by Google."); `voice` is the instructions ('' for the default); `audience` `{id?, name, description, knows?, wants?, tasks?, objections?}` creates or updates one; `audienceIDs` sets the active ones |
| `settings.deleteAudience` | `id` | deletes an audience (it stops being active) |
| `settings.forgetKey` | | |
| `models.refresh` | | |
| `read` | `audienceIDs?` | reads the page as each audience (default: the active ones), side by side; settles when every reading has answered. An audience already reading is skipped ("Already reading as …" when all are); a failed reading is retried |
| `reading.show` | `audienceID`, `index` | highlights the text that answers a checklist item |
| `reading.open` | `audienceID`, `index` | makes a partial or missing item an open item (a page-level annotation of kind `missing`); resolves to its id |
| `export` | `scope: 'page' \| 'all'`, `to: 'download' \| 'clipboard'` | the kept changes, the open items, and the checklist gaps not made into open items |

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
  pick?(on),                     // pick mode on or off; → boolean (false: the page cannot). A pick is reported
                                 //   through onSelection and returned by selection(), like a selection
  onPickEnd?(fn),                // fn() when the page ended pick mode itself (Escape)
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
  content script sends `{cw: 'selection'}` back, and `{cw: 'pick', on:
  false}` when Escape ends pick mode in the page; a tab the panel leaves
  while picking is told to stop. The panel follows its
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

Charts in `charts/`, one actor per annotation, one for the page chat's
open conversation, and one per reading of the page as an audience
(`charts/reading.scxml`: reading → done or failed, retry; its header
draws the flow).
Actors reach the world only through `<send type="...">` to I/O processors
the core registers: `page`, `model`, `store`, `ui`, `settings` (the
audiences the model defines or chooses: it saves them and answers
`audience.saved {ids: {name: id}}` or `audience.refused {message}`),
and `spawn` (the
package's `Spawner`: the conversation actor spawns an annotation actor for each
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
`define_audience {name, description, knows?, wants?, tasks?, objections?,
use?}` (create or update by name; `use: true` makes it active) and
`use_audiences {names}` (the active ones) and `read_as {audience}` (the
`model` processor plans the reading; the conversation spawns it). A
reply's audience calls go from the actor to `settings`; a reply of
audience calls only says what was saved. A reading asks with
`READING_FUNCTIONS` only: `report_finding {kind, original_text?, need,
explanation, replacement?, section?}` and `report_coverage {item, status,
answer_text?, explanation}` (`readingOf` reads them); the reading actor
sends `model` a `read {page, audienceID}`, answered `model.reading
{items, findings, thoughts?, seconds?}` with the findings' anchors and
the items' found, at most `MAX_FINDINGS`.

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
