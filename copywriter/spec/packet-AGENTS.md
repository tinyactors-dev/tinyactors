# Implement the Chrome extension from this specification

This archive is a **design and acceptance contract**, not a compiled extension. Build **V1: a local-only Chrome Manifest V3 extension running TinyActors WASM**. Do not build a backend, cloud database, object-storage service, enterprise authentication or publishing pipeline. Do not ask the reviewer to approve a packet at the end: keeping a revision is its approval; download is explicit. The ZIP exported by the extension is **framework-agnostic** and must never require TinyActors of the recipient coding agent.

## Read in this order

1. `README.md` — release scope, build and source-of-truth map.
2. `implementation/implementation.xml` and `implementation/second-pass.xml` — normative runtime topology, UI hierarchy/tokens, 35 resolved edge decisions, linked test assertions. Open `implementation/implementation-view.html` for a readable inspection view.
3. `io-processors.xml` + `io-messages.xsd` — all typed I/O operations. `implementation/reference/runtime-wiring.xml` maps operations to actor inputs and completion events; `implementation/reference/patch-wired.scxml` demonstrates `<send>` and success acknowledgement.
4. `system.xml` + `specification.xsd` — 50 user flows, 102 requirements, tests, actor/state and storage models.
5. `deployment.xml` + `deployment.xsd` and `implementation/platform-parity.xml` + `.xsd` — two-platform adapter compatibility, deployment prerequisites, data flows and retention. Open `implementation/platform-compatibility.html` for a searchable parity table.
6. `statecharts/*.scxml` — state models with XHTML `<metadata>`, `xmlns:h` on each SCXML root. `model-tools.xml` is canonical for native Gemini function declarations.
7. `widget-mock.html` + `explorer.html` — usability reference and navigable docs, **not runtime code**. The mock itself fakes Gemini/audio and DOM interaction.

## V1 implementation boundaries

- **Side panel:** owns UI, Gemini HTTPS/Live calls, ephemeral microphone streams, IndexedDB and session-scoped credential vault. UI can be closed and recreated. User enters a Google AI Studio API key in Settings; keep it in `chrome.storage.session` only, never in IDB, a content script, a chart datamodel or a ZIP. Browser restart requires reentry. This is a trusted-browser BYOK security tradeoff.
- **Dedicated Worker owned by the side panel:** exactly one `@tinyactors/wasm` host, running the bundled charts, with effect requests resolved through typed adapters. Do not run the actor host in the MV3 service worker or the page.
- **Top-level content script:** selection/anchor/inspect and reversible literal-text DOM patches. Never call Gemini or receive its key. No arbitrary script, HTML or CSS from model output.
- **MV3 service worker:** ephemeral privileged bridge for extension activation and screenshot capture. No durable actor state and no assumed lifetime. Activate only after user gesture using `activeTab` and `scripting`.
- **Storage:** local IndexedDB domain events/records; optional user-confirmed screenshots. No audio files, full-page HTML archives or automatic screenshots. No server required. If voice/AI unavailable, text annotations and packet export still work.
- **Export:** freeze a consistent session revision; export kept copy changes and open design annotations with DOM anchors, source context and opted-in evidence as XML/XSD ZIP. No global approval dialog, remote handoff, TinyActors branding or secrets.

## Mandatory build and test gates

- Generate contracts/types from XML. Do not manually edit files under `implementation/generated/` or `gemini-function-declarations.json`.
- Validate decoded messages with generated JSON Schema **and** enforce semantic restrictions (origin, tab/document nonce, hash guard, mutation allowlist, actor authorizations); TS type checking alone is insufficient.
- For *every supported* I/O operation, have positive and failure tests. Include a browser integration suite with real MV3, a deterministic fake provider, vanilla/SPA DOM fixtures, keyboard accessibility and a ZIP that validates against `work-packet.xsd`.
- Never claim the extension works merely because `make all` passes. `make all` validates the **specification artifacts**, not a produced extension.
- Run `make all` from the archive root. `validate.py`, `implementation/validate_impl.py`, `implementation/test_platform_parity.py`, `test_widget_ui.py` (requires Playwright/Chromium) are spec/mock verification tools.
- The sxml CLI is optional and the installer is bundled in `tools/install-sxml.sh`. It could not be run during authoring because this environment lacked public GitHub DNS; `lxml` was used for validation.

## Source ownership and regen order

`build.py` generates `system.xml`, followed by `extend_design_v12.py` and `extend_local_v13.py`. `generate_extra.py` generates packet XSD/example and charts; `generate_io.py` generates I/O XSD/catalog/examples; `generate_model_tools.py` generates Gemini declarations; `extend_contracts_v13.py` applies local-first contract revisions. `implementation/upgrade_v15.py` adds the revision-commit I/O operation; `implementation/second_pass.py` generates the final audit XML/view. `implementation/platform_parity.py` generates the cross-platform I/O contracts. `implementation/test_platform_parity.py` validates the shared schemas against both adapter modes and their capability restrictions. `decorate_scxml.py` adds XHTML docs. `generate_deployment.py` creates deployment contracts. `render_reports.py`, `render_explorer.py`, and `render_widget.py` create the HTML documentation/mock from their sources. `implementation/generate_contracts.py` creates TypeScript, JSON Schema and routing index from the canonical contracts. Do not hand-edit generated files.

**Normative conflict rule:** `implementation/second-pass.xml` and `implementation/implementation.xml` constrain V1; `deployment.xml` constrains multi-mode behavior; `system.xml` is the product-wide graph, including later releases. If old generalized flows mention mandatory server auth, object storage or final approval, treat them as superseded for V1, not as implementation requirements. Report any remaining contradiction as a failing gate before coding.

## V2 embedded-mode constraints (normative, not part of V1 release scope)

- Embed as a first-party top-level `<script>` with Shadow DOM controls. A cross-origin `<iframe>` cannot inspect or patch the publication DOM; it is outside scope.
- The publication must permit static reviewer script/WASM under its CSP (`wasm-unsafe-eval` where required), required static asset origins, and provider `connect-src`. If CSP blocks script initialization, report an installation/configuration failure; the browser will not permit the widget to synthesize a UI error after the script has been blocked.
- The embedded adapter implements the same `io-messages.xsd` request/result/fault types. `chrome-visible-tab` capture is unsupported; use approximate DOM render or manual upload, with an explicit approximation label. BYOK lives in first-party page memory and cannot be protected from malicious page JavaScript; only trusted-editor demos should use it without a broker.
- Download semantics are **initiated**, not verified saved to disk. An extension may observe later Chrome download status, but `PacketExportResult.deliveryInitiatedAt` is the shared acknowledgement field.
- Cross-origin sites do not share IndexedDB, actor state, credential handles, microphone stream handles or DOM preview IDs. Don't implement cross-platform session synchronization in V1/V2; work packets are portable exports.
- Run `implementation/test_platform_parity.py` and add real browser integration smoke tests for the embedded script (desktop/mobile, strict CSP, permission denial, SPA rerenders). The spec-only suite does not prove that browser implementation works.
