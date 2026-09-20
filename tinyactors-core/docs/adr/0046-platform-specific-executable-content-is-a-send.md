# ADR 0046: Platform-specific executable content is a `<send>` to a host I/O processor

**Status:** Accepted; amends ADR 0030, applies ADR 0040
**Date:** 2026-09-20

## Context

SCXML 4.10 lets an implementation add its own executable content — an
element from a foreign namespace inside a block, such as
`<ccxml:accept connectionid="_event.data.connectionid"/>`. Core reserved a
spec surface for it:

```c
TA_WHEN(TA_SCXML_ACTION_EXTENSION) struct { ta_str type; ta_bytes payload; } extension;
```

plus a `ta_extension_id` typedef and a `TA_ACTION_EXTENSION` in the compiled
IR. Nothing behind them was ever built: `src/builder.c` answers
`TA_ERR_UNSUPPORTED`, there is no way to register an extension
implementation, and `docs/internal/definition-inspection.md` records that
"the compiler rejects `<invoke>` and extensions as executable content, so
export never meets them". Meanwhile `docs/internal/executable-content.md`
lists extension actions among the content the IR must represent, and
`docs/internal/builder.md` says the compiled IR holds "parameters, invokes,
and extensions". The docs promise a feature the code refuses.

### The reserved shape cannot express what an extension needs

Every other attribute in the public spec is one of four things:

| Shape | Used for |
|---|---|
| `ta_str` | names never evaluated: `state.id`, `param.name`, `log.label`, `foreach.item` |
| `ta_expression` | attributes that are always expressions: `transition.cond`, `assign.location`, `script`, `log.expr`, `foreach.array` |
| `ta_scxml_string_spec` / `ta_scxml_event_spec` / `ta_scxml_id_spec` | the `x` / `xexpr` attribute pairs: `type`, `target`, `src`, `delay`, `event`, `id`/`idlocation` |
| `ta_scxml_value_spec`, aggregated by `ta_scxml_param_spec` | values and payloads: `<data>`, `<assign>`, `<send>`/`<invoke>` data, `<donedata>`; `namelist` lowers to `params` |

`type` plus an opaque `ta_bytes` is none of them. It cannot carry an
evaluated attribute at all, so the very example SCXML 4.10 gives —
`connectionid="_event.data.connectionid"` — is inexpressible: no expression
enters the definition's expression table, so no data model ever prepares or
evaluates one. It cannot carry a host expression (`TA_EXPRESSION_HOST`), so
a Node or CPython importer cannot pass a `napi_ref` or `PyObject *` through
it, contradicting ADR 0005 and ADR 0006. It has no namespace, so
`<ccxml:accept>` and `<v3:form>` collapse into one string. An importer would
have to invent an encoding inside the payload for all of this, which is the
duplication ADR 0044 was written to end for send targets.

So the choice was to fix the shape or to remove it. What the standard
requires, and what the field does, decided it.

### What SCXML actually requires

SCXML 4.10 is two normative rules and nothing else: an extension **must not**
be in the `scxml` namespace, and its functionality is unrestricted **except**
that it "MUST NOT cause transitions or any form of change of state (except
indirectly, by raising events that trigger transitions)". It adds that
executable content is a single blocking operation, so extensions must
"execute swiftly".

SCXML A.2 supplies the negative: when a processor "encounters a
non-conformant document, or one containing non-SCXML elements or attributes
which are proprietary, or defined in a non-SCXML namespace, **its behavior is
undefined**". There is no normative rule that unknown foreign content is
ignored. Refusing it is conformant.

Appendix G.7 describes the alternative: lower the custom element to a
`<send>` to a custom event processor, with the result arriving as a
completion event that a transition handles. G.7 is informative prose, uses no
RFC2119 keywords, and ends by deferring the mappings to "the individual
Custom Action Element specifications" — which were never written.

### What implementations actually ship

| Engine | Mechanism | Attributes | Unknown foreign element |
|---|---|---|---|
| Apache Commons SCXML | `CustomAction(nsURI, localName, Class)` on the reader, per parse; `Action.execute(ActionExecutionContext)` | raw strings via `setFoo(String)` reflection; the action calls the evaluator itself | warn, or `ModelException` in strict mode |
| SCION | `registerCustomActionTags({ns: {tag: fn}})`; the handler returns JavaScript source spliced into generated code | raw attributes; `*expr` inserted into data-model scope | hard compile error |
| uSCXML | `Factory::registerExecutableContent()`, process-global, plugin-loadable; DOM `enterElement`/`exitElement` | raw DOM, evaluated selectively | `assert(false)` |
| PySCXML | `@custom_executable(namespace)`, keyed by namespace only | raw element node | `exmode="strict"` per document, else ignored |
| Qt SCXML | none | — | silently skipped, no diagnostic |
| Genesys ORS | ~10 built-in module namespaces; no embedder API | every attribute is an expression (`method="'get'"`) | undocumented |

Six findings from that survey decide this ADR:

1. **Results come back as events, universally**, because SCXML 4.10 forces
   it: Commons SCXML `getInternalIOProcessor().addEvent`, SCION
   `this.send(...)`, uSCXML `enqueueInternal`, JSSCxml
   `callback.done`/`callback.failed`, Genesys `<module>.<action>.done` and
   `error.<module>.<action>` correlated by a caller-supplied `requestid`.
   That is G.7's model. What engines kept was the authoring surface, not the
   semantics.
2. **Nobody auto-evaluates attributes.** Handlers receive raw strings and
   evaluate what they choose. The single exception, Genesys, made every
   attribute an expression and pays for it with inner quoting on literals.
3. **Registration is always out of band.** No engine resolves an extension
   from the document: the namespace URI is a key the embedder supplies, never
   a locator.
4. **G.7's literal rewrite was implemented once** — PySCXML's
   `@preprocessor`, now obsolete and commented out.
5. **Extensions rot when they are a second path.** uSCXML's custom content
   works in the interpreter but its C transpiler emits "writeExecContent
   unsupported element"; the Commons SCXML documentation still tells authors
   to implement `ExternalContent`, a class deleted from master. The engines
   that stayed healthy route their own built-ins through the extension table
   (SCION registers `script`, `assign` and `log` that way).
6. **The spec's own examples are dead letters.** CCXML 1.0 is a real
   Recommendation but never mentions SCXML, and no engine executes
   `<ccxml:accept>` as SCXML executable content. VoiceXML 3 never left
   Working Draft, and the `v3` namespace document says it is "reserved for
   use as an example in the first Working Draft of the SCXML 1.0
   specification".

### What core already has

ADR 0040 already names custom I/O processors "the extension point for
non-SCXML destinations". A `<send>` carries everything a custom action
needs: a `type` naming a registered processor, `params`/`namelist` of
evaluated named values, inline `<content>`, a target, a delay, and an
`idlocation` that stores the generated send ID for correlation. The
processor receives a `ta_send` of evaluated `ta_value`s and may report a
fault; replies arrive as ordinary external events. An unregistered type is
`error.execution`, a refused dispatch is `error.communication`, and the
reserved SCXML processor type cannot be replaced. All of this is
implemented and covered by `scxml_processor.rb`, `send_targets.rb` and
`fault_contexts.rb`.

## Decision

**Platform-specific executable content is a `<send>` to an I/O processor the
host registers. Core has no extension action.**

- `TA_SCXML_ACTION_EXTENSION` is removed from `ta_scxml_action_kind` together
  with its union member, `ta_extension_id`, and the internal
  `TA_ACTION_EXTENSION`. It is the last enumerator, so no other action kind
  is renumbered. A spec that names an unknown action kind is `TA_ERR_INVALID`,
  as it already is for any other unknown kind.
- The compiler's refusal stops being a gap and becomes the documented
  behaviour, which SCXML A.2 permits and which Apache Commons SCXML (strict
  mode) and SCION also choose.
- An importer of an XML document containing foreign-namespace executable
  content lowers it to `<send>`, or refuses the document. That is importer
  policy, and importers are outside core (ADR 0025), exactly as `<data src>`
  fetching is.

**The sanctioned pattern, which the docs state and the suite proves:**

1. the host registers an I/O processor under an interned type atom naming its
   vocabulary, for example `urn:example:telephony`;
2. the chart writes `<send type="urn:example:telephony" event="accept"
   idlocation="requestid"><param name="connectionid" expr="..."/></send>`;
   attributes become `<param>`s, evaluated by the data model, and the element
   body becomes `<content>`;
3. the processor performs the host work and reports completion as an ordinary
   event named `<vocabulary>.<action>.done`, or
   `error.platform.<vocabulary>.<action>` on failure. `<vocabulary>` is the
   vocabulary's dotted name, not its type URI, and the failure name sits
   inside the namespace ADR 0027 reserves for the application and platform,
   so a chart catches it with the plain `error` descriptor;
4. a transition handles that event. Following ADR 0027, a chart may listen to
   the whole vocabulary with a prefix.

A reply is correlated by a token the chart passes as a `<param>` and the
processor echoes back, or by the send's own ID. Writing these scenarios
showed that the second was unavailable: the chart holds the ID as the text
`#_send.1`, the processor holds `ta_send.send_id` as a number, and core's
text rule was not public, so neither side could reach the other's form.
**ADR 0048 closed that**, making `ta_send_id_parse`/`ta_send_id_format`
public for the reason ADR 0044 made the target grammar public. Both ways of
correlating are now legitimate; a chart-supplied token remains the simpler
one, since it needs nothing from core.

The naming convention is Genesys ORS's, the most disciplined example the
survey found. It is a convention for hosts, not a rule core enforces: core owns the send, the type resolution, the fault mapping, and the
event names' structure (ADR 0007), while what an event means is the
vocabulary's business.

**Both of SCXML 4.10's normative rules survive, structurally rather than by
documentation.** The type atom plays the namespace's role, and the reserved
SCXML processor type cannot be taken (already enforced). No processor can
change a configuration, because `ta_io_vtable` has one operation, `send`,
whose only outcomes are success and a fault: the "no `goto_state()` escape
hatch" of ADR 0030 needs no separate enforcement. The survey's finding 6 —
that engines violate their own conformance rule unless the registration API
refuses — is why this matters: Apache Commons SCXML's constructor rejects the
SCXML namespace and stayed clean; uSCXML documented the rule and shipped four
extensions that break it.

**When to reopen this.** A chart that needs a *synchronous* result inside one
block, which the send-plus-completion-event pattern cannot give without an
extra macrostep, is the one motivation this ADR does not answer. If such a
chart appears, the design the survey implies is: an action carrying `type`, a
`ta_scxml_param_spec` list for its attributes, and a `ta_scxml_value_spec`
body; dispatch on (type atom, action name) as the field's (namespace, local
name); per-attribute literal-or-expression through `ta_scxml_value_spec`,
which gives the `x`/`xexpr` convention structurally instead of by convention
and avoids Genesys's quoting tax; results as events; a registration API that
refuses the reserved type; and the built-in actions on the same path, so the
two cannot rot apart. Recording it here means it does not have to be
rediscovered.

## Consequences

- One path for host effects, already implemented, exported, inspected and
  tested. Nothing new can rot the way finding 5 describes, because there is
  no second path.
- Session images are unaffected. An extension action would have needed the
  same runtime-handle rules as invoker services (ADR 0039); an I/O processor
  send completes within the call and leaves nothing to reconstruct.
- A result costs a send and a transition, which is what every surveyed engine
  does anyway, and an author writes `<send type="...">` rather than
  `<my:thing>`. That is the price of the decision, and the survey says the
  field pays it too.
- Documentation to correct in the same change: the extension bullet in
  `docs/internal/executable-content.md`, "and extensions" in
  `docs/internal/builder.md`, and the extension sentence in
  `docs/internal/definition-inspection.md`, which becomes permanent rather
  than provisional. `docs/manual/extensions.md` gains the recipe above,
  including the completion-event convention.
- ADR 0030 stands, with a narrower subject: there is no extension action left
  to constrain, and its rule now describes I/O processors and invokers, which
  cannot change a configuration by construction.
- The test host drops the `extension` chart action, and the three expected
  failures in `tinyactors-test/ruby/scenarios/extensions.rb` are replaced by
  five passing scenarios for the sanctioned pattern: SCXML 4.10's own
  `<ccxml:accept connectionid="_event.data.connectionid"/>` example as a
  `<send>` to a recording processor, its evaluated `<param>`s, the ID
  `idlocation` gives the chart, the completion event, correlation by the
  chart's ticket, and a vocabulary failure caught by the plain `error`
  descriptor.
- The gap above — a send ID the chart and the host hold in two forms that
  neither could convert — was found by writing those scenarios, and was the
  kind of duplication ADR 0044 removed for target URIs. ADR 0048 removes this
  one, and `extensions.rb` proves the round trip end to end.

## Sources

Primary, verified when this ADR was written:

- SCXML 1.0, 4.10 Extensibility of Executable Content —
  <https://www.w3.org/TR/scxml/#extensibility>
- SCXML 1.0, A.2 Conforming Processors —
  <https://www.w3.org/TR/scxml/#ConformingProcessors>
- SCXML 1.0, G.7 Custom Action Elements —
  <https://www.w3.org/TR/scxml/#custom_action>
- Apache Commons SCXML, custom actions —
  <https://commons.apache.org/proper/commons-scxml/guide/custom-actions.html>
- SCION, plugin API —
  <https://github.com/jbeard4/SCION/wiki/Plugin-API-for-SCION-2.0-branch>
- uSCXML, `src/uscxml/plugins/ExecutableContentImpl.h` —
  <https://github.com/tklab-tud/uscxml>
- PySCXML, "Extending PySCXML" — <https://github.com/jroxendal/PySCXML>
- Qt SCXML compliance —
  <https://doc.qt.io/qt-6/qtscxml-scxml-compliance.html>
- Genesys Orchestration Server, extensions —
  <https://docs.genesys.com/Documentation/OS/latest/Developer/OrchExt>
- JSSCxml `<jssc:fetch>` — <https://www.jsscxml.org/fetch.html>
- CCXML 1.0 — <https://www.w3.org/TR/ccxml/>; VoiceXML 3.0 Working Draft —
  <https://www.w3.org/TR/voicexml30/>

Reported but not verified against source, and therefore not load-bearing
above: JSSCxml's internal dispatch table, the "scxml-core-engine" project's
compile-time checks, and the LXSC/RXSC (Lua/Ruby) dispatch details.
