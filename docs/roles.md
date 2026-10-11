# Roles: who a change is for

A feature or a change serves someone. Before designing one, say whose
question it answers, what they decide with the answer, and who pays for
it. Most bad designs in this repository's history served nobody in
particular, or made one role pay for another's convenience.

Each role below has the questions it asks, what it decides, and what it
must never be made to pay. The reference application is granary
(`~/projects/granary`): when a role needs an example, look there first.

## The roles

### Application developer

Builds charts and the application around them (granary).

- **Asks:** why did this actor end up in this state? Which transition
  fired, with what data? Which step is slow? Did my change make it cheaper?
  How do I test this chart?
- **Decides:** how to fix a chart, a guard or an action; whether to accept
  a change.
- **Needs:** an API that reads like the language it is written in;
  errors that point at the chart and the line; traces of *one* actor or
  request, recorded when it matters; deterministic tests (a clock option,
  `flush()`).
- **Never pays for:** wiring the runtime's own features (tracing,
  hibernation, links) before they work; learning SCXML's XML to use the
  JavaScript API.

### Operator / on-call

Runs an application built on tinyactors in production.

- **Asks:** is it healthy (errors, faults, lost records)? Is it overloaded
  (backlog, late timers, a run blocking the loop)? Which chart is busy or
  failing? Is a pool about to fill? What happened in this incident?
- **Decides:** page or not; scale out, raise capacities, roll back a
  version, turn tracing up for one chart for a while.
- **Needs:** numbers that are on without setup and that sampling does not
  hide (counts, rates, failures); standard formats (OpenTelemetry, OTLP)
  that their stack already reads; switches that turn themselves off (a
  watch that ends by itself).
- **Never pays for:** a forgotten setting flooding memory or the bill; an
  outage caused by telemetry (export fails quietly, never stops a run).

### Capacity planner

Sizes machines and systems.

- **Asks:** how many actors and events per second fit on a machine? How
  much memory per actor? Which chart costs the CPU? How fast does it grow?
- **Decides:** machine size, the capacities in `ta_system_config`, how many
  systems per host.
- **Needs:** throughput and memory measured together, per chart, on named
  machines; documented costs per actor and per event.

### C embedder

Links libtinyactors into a program that is not a JavaScript runtime: a
game, the PostgreSQL extension, a microcontroller, a host in another
language.

- **Asks:** are my pools big enough? Did anything fail? How much fuel per
  frame or per statement? What does each feature cost me?
- **Decides:** how big a block to give each system; the fuel budget per
  frame; which features to turn off.
- **Needs:** plain structs and pulled data (effects, records, stats), no
  callbacks beyond the data model and the allocator, one block of memory
  sized at creation, and a way to turn off what it does not use.
- **Never pays for:** anything per event or per actor it did not ask for; a
  clock, an allocation after init, the C library, OTLP or XML it does not
  use.

### Binding author

Writes or maintains a language binding (none exists until
[ADR 0009](adr/0009-one-host-two-queues.md) regenerates them: JavaScript
first, then CPython; Go, Rust, the JVM and .NET are not written yet).

- **Asks:** what must I write myself, and what does C give me? How do
  values cross? How do I get charts in and out, traces out, errors back?
- **Decides:** how values look in their language and how they become
  bytes; nothing else.
- **Needs:** every piece free of a language's API already in a C layer
  (parsing, writing SCXML, encoding OTLP, routing, sampling), so a new
  binding is thin; one way to do each thing; conformance vectors it can run.
- **Never pays for:** reimplementing in its language what another binding
  already does in its own.

### tinyactors maintainers

Change the runtime.

- **Asks:** did this change cost throughput or memory? Where does the fuel
  go? Is the change in the right layer? Does every caller still work?
- **Decides:** accept or reject a commit; where code lives.
- **Needs:** the torture chamber (throughput and memory side by side), fast
  tests that cover every behaviour, one implementation of each thing,
  small layers with one job each.

### The observability backend

Prometheus or Mimir through OTLP, Tempo, Loki, Grafana Cloud, the
OpenTelemetry Collector: a role because its limits shape what tinyactors
should emit.

- **Asks:** how many series does this add? Are names, units and
  temporality what the translator expects? How many spans and logs per
  second?
- **Decides:** cost per month; whether data is kept or dropped.
- **Needs:** bounded cardinality (per chart, never per actor, session or
  event name), cumulative metrics, standard attribute names, and sampling
  so volume can be controlled.

## Using the roles in a design

A design, proposal or ADR says, near its start:

1. **Who it serves first, and why.** Usually one or two roles. Name the
   questions it answers and the decisions those answers feed.
2. **What already answers each question.** Prefer what exists (a trace, a
   stats field, an existing API) over something new. Every new piece names
   the role and question it serves.
3. **Who pays, and how much.** Per event, per actor, in memory, in code
   size, in API surface, and in work for every binding. Check each role's
   "never pays for".
4. **What is out of scope**, and which role that leaves unserved.

`docs/proposals/metrics-and-trace-policy.md` (section 1.1) is a worked
example.
