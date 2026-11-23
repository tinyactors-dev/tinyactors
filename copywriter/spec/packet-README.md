# TinyActors Visual Reviewer — structured system specification

**Specification version:** 1.7.0 (implementation contract and cross-platform compatibility) · 2026-11-23 · **Status:** design specification and interactive mockup, not an implemented production system.

## Start here

- **`widget-mock.html`** — Redesigned, quieter Chrome side-panel mockup and the **best starting point**. Select text on the sample publication, browse a compact annotation list, inspect one change at a time, add notes, preview/keep/undo replacements, switch persona, simulate BYOK setup, and download an XML work-packet ZIP. The **Explore** control opens the full XML-derived specification explorer in the same interface; every selected annotation also offers a contextual I/O contract link. It is deliberately **simulated**: it does not record audio, call Gemini, or store API keys.
- **`explorer.html`** — Interactive requirements, flows, I/O contracts, Gemini tools, actors, and storage explorer generated from the canonical XML. Restyled to share the reviewer palette and supports contextual navigation from the mockup.
- **`io-contracts.html`** — Browsable typed I/O processor documentation. Its generated prose may lag newer browser-local overrides; the canonical contract is `io-processors.xml` and `io-messages.xsd`.
- **`system.xml`** — Canonical graph of requirements, flows, events, actor models, storage entities, architectural decisions, and verification links.
- **`deployment.xml`** — Explicit platform variants, data flow boundaries, dependencies, and retention choices.

## Product decisions (v1.3 model, v1.4 presentation)

1. **V1: client-only Chrome extension.** TinyActors WASM operates locally; a publication does not need to install a script or run a service.
2. **V2: embeddable website widget.** Same SCXML charts and typed I/O operations, with host-page DOM/capture/credential/download adapters instead of Chrome API adapters. Suitable for mobile browsers that cannot install Chrome extensions.
3. **V3: optional self-hosted shared mode.** Single-process server, SQLite and optional local file attachments; no S3, Redis, PostgreSQL, or managed auth dependency required.
4. **Gemini BYOK for V1.** The user enters a Google AI Studio key in extension settings. It is kept in transient trusted extension context by default, addressed by opaque handle in actor messages, and not exported. V1 deliberately has **no encrypted-at-rest key persistence**; the key must be reentered after browser restart. This does not eliminate the risk of placing a credential in a browser environment. An optional backend can broker ephemeral Live tokens for trusted team usage.
5. **Native Gemini dialogue.** The model handles questions and verbal pushback as native text/audio dialogue, and function calls are dispatched through typed TinyActors I/O operations. No parallel bespoke clarification protocol.
6. **Annotations are the review surface.** User action to keep a revision approves that individual change. Packet download snapshots kept changes and unresolved annotations; there is no separate final approval gate. Export works with no configured coding-agent handoff.
7. **Audio is never persisted by default.** Microphone frames are buffered in RAM and streamed to Gemini; no application audio recordings or log of raw frames. Optional text observations persist in IndexedDB. Gemini's service still processes data under Google's own terms.
8. **No object storage dependency.** Screenshots stay transient unless explicitly attached; IndexedDB is sufficient locally, and server-mode attachments may use the local filesystem. ZIP handoff is generated and downloaded client-side.

## XML and schema files

| Path | Role |
| --- | --- |
| `specification.xsd` | XML Schema for system graph: flows, requirements, tests, decisions, actor contracts, data entities |
| `io-processors.xml` / `io-processors.xsd` | I/O processor catalogue and executable contracts: guards, side effects, retries, faults, typed request/result bindings, traceability |
| `io-messages.xsd` | Shared XML Schema for the typed I/O messages and fault types; model-provider wire messages stay native |
| `statecharts/*.scxml` | TinyActors statecharts with XHTML documentation using root-level `xmlns:h` |
| `model-tools.xml` / `model-tools.xsd` | Model function declarations and links to the underlying processor contracts |
| `gemini-function-declarations.json` | Derived Gemini function declarations |
| `deployment.xml` / `deployment.xsd` | Deployment modes, platform mappings and data flows |
| `implementation/platform-parity.xml` / `.xsd` | Normative per-operation Chrome-extension and embedded-site adapter contracts |
| `work-packet.xsd` | Portable work packet schema, with referential integrity constraints |
| `examples/` | Typed I/O examples and example work packet |
| `queries/` | Integrity, coverage and traceability XQuery |
| `AGENTS.md` | Guidelines for modifying the specification |

## Regeneration and validation

Run `make all` from this directory. It reconstructs XML from the authoring scripts, applies the v1.2 and v1.3 deltas, generates SCXML and XSD examples, refreshes reports and both interactive HTML views, and executes `validate.py`. `widget-template.html`, `widget-ui.js`, `widget-export.js`, and `render_widget.py` are the editable source for the mockup; **do not edit generated `widget-mock.html` directly**. `render_explorer.py` owns the generated technical Explorer.

Since v1.4, the implementation contract, shared I/O message cardinalities, export initiation timestamp and platform compatibility map have been strengthened; changes are generated reproducibly from the authoring scripts. The mockup and Explorer are local, static HTML; no server is needed to open the extracted files. `test_widget_ui.py` is an optional Playwright smoke test for the UI and for the work-packet XSD/digest.

Run `make validate` to validate the existing generated artifacts, including the regenerated platform parity map. The bundled `tools/install-sxml.sh` provides a pinned installer for the sxml tool, but sxml could not be installed inside the authoring environment due to GitHub network restrictions; the XML has instead been validated using `lxml`.

**Warning:** `build.py` generates `system.xml`; `generate_io.py` generates I/O files. If you edit generated files directly and then call `make all`, your changes may be overwritten. Prefer edits in the generator scripts or intentionally transition to direct XML authoring.


### Cross-platform compatibility contracts (v1.7)

- `implementation/platform-parity.xml` / `.xsd` — **64 typed adapter definitions**: 32 canonical I/O operations × Chrome extension and embedded website. Each has request/result root types, host boundary, availability, precondition, effect, fault behavior and contract test. `implementation/platform-compatibility.html` is an offline searchable view.
- `implementation/test_platform_parity.py` — validates both platforms' request/result samples against `io-messages.xsd`, checks capability allowlists, unsupported-operation typed faults, valid empty results, and the generated runtime JSON Schema's cardinalities. **These are conformance/specification tests, not a functioning Chrome extension or embedded widget.**
- The embedded mode is a **first-party top-level script with Shadow DOM UI**, not a cross-origin iframe. Its host must allow loading the bundled WASM (and worker if chosen) under the site's CSP, plus provider connectivity; otherwise installation is unsupported. It cannot call `chrome.tabs.captureVisibleTab` and cannot hide a BYOK key from compromised page scripts. Chrome has stronger credential isolation and native capture.
- The two platforms share **one message schema, actor charts and domain protocol**. The envelope is host-aware (`hostKind`): embedded page operations require a document nonce but no Chrome `tabId`, while extension page operations require both. They do not share live resource handles, origin-separated IndexedDB stores, or browser privileges. The same exported packet format works across platforms. `IO-CAPTURE` has host-specific method allowlists; it returns a typed fault for unavailable methods rather than pretending the same implementation exists.
- Corrections found in this audit: zero microphones, zero eligible page nodes and empty first-run context are valid; a design-only packet can compile with zero accepted text changes; `PacketExportResult.deliveryInitiatedAt` means **download initiated**, never guaranteed saved to disk. Required reference lists remain nonempty in both XSD and generated JSON Schema.

## Privacy and security boundaries

- Gemini receives submitted audio, text, context, and optionally screenshots; client-only does not mean no external data transmission.
- User credentials and provider keys never appear in actor messages, on-page DOM, or ZIP packets; a trusted extension context resolves opaque handles.
- The embedded V2 BYOK mode cannot securely isolate long-lived keys from a compromised host-page origin; treat it as trusted-editor testing and favor a credential broker for shared deployments.
- Do not transmit password fields or other sensitive document regions to models. All DOM changes are declarative text patches verified against their original hashes; do not run model-generated JavaScript.
- Work packet export is an explicit download action. No automatic publish or deploy.

## V1.7 implementation handoff, second-pass and platform portability (normative)

**The coding agent should implement the extension with TinyActors, but the packets exported by that extension must be framework-neutral.** The implementation specification is not the output packet format.

- `implementation/implementation.xml` / `.xsd` — exact Chrome MV3 runtime boundaries; component hierarchy and design tokens; 32 platform I/O bindings; local IndexedDB schema; reference manifest; 28 implementation acceptance cases. XML sources, not mock HTML, define expected behavior.
- `implementation/second-pass.xml` / `.xsd` — **35 resolved implementer ambiguities** across actor lifetime, startup permissions, message correlation, microphone devices, BYOK credential security, screenshot privacy, DOM identity, persona provenance, packet consistency, accessibility and recovery. Every decision links to an existing user flow and I/O operation and includes an observable test.
- **[Implementation decisions explorer](implementation/implementation-view.html)** — readable, searchable view generated from `second-pass.xml` (works offline).
- `implementation/reference/runtime-wiring.xml` — all I/O triggers, completions and unsupported V1 capabilities.
- `implementation/reference/patch-wired.scxml` — normative example illustrating effect completion vs user intent, including `revision.saved` after IndexedDB commit; other charts express the required behavior and must be connected by the implementation's adapter layer.
- `implementation/generated/io-messages.d.ts`, `io-contracts.d.ts`, `io-message-schemas.json`, `io-contract-registry.json` — generated artifacts from canonical XSD and I/O catalogue, not handwritten schemas.
- `implementation/reference/event-envelope.schema.json` — conditional page identity: global credential/context events need no tab; DOM/capture events require both `tabId` and `documentNonce`.

**Credential decision:** V1 uses `chrome.storage.session` (browser-session, memory-backed storage) for a user's own Gemini key, never permanent extension storage; the key is reentered after browser restart and is not copied into actor state. User-facing warning: BYOK in any browser context is less secure than server-side credentials. Google's data processing is not under the extension's retention control.

**Build checks are not product integration tests.** `make all` verifies schema consistency, generated types, references and the simulated UI. Implementing the V1 extension requires the additional real Chrome MV3/DOM/Gemini testing described in `AGENTS.md` and `implementation/second-pass.xml`.
