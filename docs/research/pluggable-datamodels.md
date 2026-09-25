# Pluggable data models: how SCXML implementations let users bring one, and what it costs in napi

**Date:** 2026-09-25
**For:** [napi ADR 0019](../../tinyactors-napi/docs/adr/0019-a-data-model-in-javascript-compiles-source-for-the-built-in-one.md) (Proposed)

## Question

napi cannot import SCXML documents whose expressions are ECMAScript source
text: its only data model, "javascript", takes functions (plus `In(state)`
and bare data names as text). The user asked to explore exposing core's
data-model vtable (`ta_datamodel_vtable`) through Node-API, so a user can
write a data model in JavaScript. This note looks at how other SCXML
implementations split the job, and measures what a data model in
JavaScript costs per expression.

## How other implementations split it

Two shapes recur.

**A whole data model behind one interface.**

- **Qt SCXML** has `QScxmlDataModel`, an abstract class a user subclasses.
  Its virtuals include `setup`, `evaluateToString`, `evaluateToBool`,
  `evaluateToVariant`, `evaluateToVoid`, `evaluateAssignment`,
  `evaluateInitialization`, `evaluateForeach`, `setScxmlEvent`,
  `scxmlProperty`, `hasScxmlProperty` and `setScxmlProperty`. Qt ships
  its own ECMAScript data model (on `QJSEngine`) and a null data model as
  implementations of that class. [1]
- **uSCXML** loads data models as plugins through a `DataModelImpl`
  interface: evaluation as boolean or data, assignment, initialisation,
  the current event, and system variables. It ships ECMAScript (V8 and
  JavaScriptCore), Lua, PHP, Prolog, XPath and null. [2]

This is the shape of core's `ta_datamodel_vtable`: storage, evaluation,
foreach, parameters and content conversion all live in one adapter.

**Expressions separate from storage.**

- **Apache Commons SCXML** splits an `Evaluator` (it evaluates an
  expression, a boolean or a location against a `Context`) from `Context`
  (variables, scoped per state). JEXL, Groovy and JavaScript evaluators
  share the same context machinery. [3]

Here a new expression language brings only the evaluator, and storage,
scoping and the SCXML bookkeeping stay common.

**What users actually write.** In Qt and uSCXML, new data models are
rare: almost everyone uses the shipped ECMAScript one. The interface
exists so the implementation itself can plug in its engines. In Commons
SCXML, evaluators are what people swap.

## Measurements

Apple M4 Max, load about 2.4, Node 22.23.2 and Bun 1.4.1 through `mise
exec`, best of 5 rounds, 20,000 actors. Throwaway scripts, not committed.

**One guard through napi's built-in JavaScript data model.** A `ping` is
posted to every actor; the chart has 1 or 21 transitions on `ping`, the
last guard true and the others false, so a delivery evaluates 1 or 21
guards:

| | Node | Bun |
|---|---|---|
| Per guard evaluated (one call from C into JavaScript) | 94–96 ns | 89–90 ns |
| Extra when the call goes through a vtable-style method with a params object | 2–5 ns | 2 ns |

So the crossing from C into JavaScript is the cost. The shape of the
JavaScript function called barely matters.

**SCXML scoping in compiled source.** ECMAScript expressions in SCXML see
data as variables (`count > 5`, not `this.count > 5`). A data model that
compiles source text with `new Function` needs a scope. Per call, in a
tight loop:

| Expression | Node | Bun |
|---|---|---|
| `function () { return this.count > 5 }` | 5.5 ns | 0.4 ns |
| `new Function('ctx', 'with (this) { return (count > 5); }')` | 58 ns | 27 ns |
| the same with a `Proxy` as scope (to add `_event` and friends) | 226 ns | 115 ns |

A compiled SCXML expression costs roughly the crossing plus the scope:
about 150 ns on Node with `with`, about 320 ns with a `Proxy`. That is
for imported documents; builder charts keep passing functions and pay
only the crossing.

## Sources

1. Qt 6 documentation, `QScxmlDataModel` class reference:
   https://doc.qt.io/qt-6/qscxmldatamodel.html
2. uSCXML, source repository and data-model plugins (`DataModelImpl`):
   https://github.com/tklab-tud/uscxml
3. Apache Commons SCXML 2, `Evaluator` and `Context` API documentation:
   https://commons.apache.org/proper/commons-scxml/apidocs/org/apache/commons/scxml2/Evaluator.html
