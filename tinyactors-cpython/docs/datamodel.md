# The `"python"` datamodel

Chart logic in Python: a chart with `datamodel="python"` runs its
expressions, locations and scripts as Python, in the application's
interpreter (`src/datamodel/`). It implements core's datamodel vtable
(`tinyactors-core/include/tinyactors/datamodel.h`) and is held to the same
suite as quickjs (bindings.md, section 8). Where the guide and the headers
leave a choice open, the decision is below with its source; the rows were
checked with the reasoned-analysis skill (a fresh agent's refutation,
2026-10-12, with spikes against the built module). It confirmed most of
them, and refuted four, which were changed: `<content>` parsed numbers
JSON does not have and collapsed all of Unicode's whitespace; `assign`
let a chart replace `In` and `__builtins__`; a `KeyboardInterrupt` did not
stop the run and dropped a handler's exception; `_event`'s text was
decoded with replacement.

## What a session sees

| Name | Value | Source |
|---|---|---|
| each `<data>` id | `None` from the start, then its value when it is initialized (early or late binding); a spawn's or an invoke's params of the same name win | SCXML 5.3; `confPython.xsl`'s header: "a variable declared without a value is None"; `init_data` in `tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c:1333` (params first) |
| `_sessionid`, `_name` | `str`: the session id, and the chart's name | `ta_datamodel_system_variables` |
| `_ioprocessors` | `{type: {"location": ...}}` | `confPython.xsl`: `_ioprocessors[type]['location']` |
| `_event` | a `tinyactors.Event`, read by attribute: `name`, `type` (`"platform"`, `"internal"`, `"external"`), `sendid`, `origin`, `origintype`, `invokeid`, `data`, `raw`; `None` for a field with no value, and `data` is `None` without data. Bound from the first event on, as the ECMAScript datamodel binds it | SCXML 5.10.1; `confPython.xsl`: "`_event` is a record read by attribute", "a field with no value ... is None", "an event without data has None" |
| `In(state_id)` | `True` when the session is in that state; `False` outside an evaluation (in a `<foreach>`'s item binding, which core calls without an environment), as the ECMAScript datamodel's | SCXML 5.9.1; `ta_datamodel_environment_in`; `datamodel_ecmascript.c:613` |
| `__builtins__` | the builtins module's dict | Python's `exec` |

**One globals dict per session, and no locals:** every evaluation runs
with the session's dict as both, so a generator expression or a lambda
sees the session's variables (AGENTS.md, "Rules the suite cannot see").

## Expressions, by kind

- **Conditions, values, locations and the `*expr` attributes** are Python
  expressions, compiled once at `chart_attach` (`Py_CompileString`,
  eval mode); **`<script>`** is Python statements (exec mode), its common
  indentation removed (the chart's XML indents it). One that does not
  compile raises `error.execution` when it is evaluated, so a chart with
  an illegal expression still loads (SCXML 5.9; `confPython.xsl`:
  "Expressions that must fail ... fail when evaluated, not when
  compiled").
- **`<content>`** (and a `<data>`'s children): JSON if it parses, as
  JSON has it (`json.loads` with `parse_constant` refusing `NaN` and
  `Infinity`, which `JSON.parse` refuses too), else its text with each run
  of space, tab, CR and LF made one space and trimmed (other Unicode
  whitespace is kept). The ECMAScript datamodel does the same
  (`content_value`, `datamodel_ecmascript.c:338-375`), and makes an XML
  DOM of XML content, which Python's datamodel does not (the W3C tests
  that need it, 557 and 561, are ECMAScript's); `confPython.xsl` writes
  `<content>foo</content>` for "the string 'foo'".
- **A value as text** (`eval_string` for the `*expr` attributes, and
  `<log>`): `str(value)`, Python's own text of a value. Our choice:
  ECMAScript's `<log>` writes an object as JSON
  (`datamodel_ecmascript.c:1473`), and nothing in the suite depends on it
  (its charts log `'pass'` and `'fail'`).
- **A condition** is `bool(value)`.
- **Fuel:** one unit per evaluation (bindings.md, section 8).

## Assignment and the system variables

- **`<assign>`**: the location's first name must be a declared variable
  (a key of the session's globals) and not a system variable (`_event`,
  `_sessionid`, `_name`, `_ioprocessors`, `_x`) or one of the
  datamodel's own names (`In`, `__builtins__`), else `error.execution`
  (SCXML 5.4; bindings.md, section 8; the ECMAScript datamodel refuses
  `In` too, `datamodel_ecmascript.c:438`). A plain name is set directly; any
  other location runs `<location> = __ta_value__`, the value bound under
  that name for the statement only.
- **After every evaluation** the system variables, `In` and
  `__builtins__` are compared with the objects the session keeps; one an
  evaluation rebound, deleted or made (`_event` before the first event,
  `_x`) is put back, and the evaluation is `error.execution`
  (bindings.md, section 8: "check after the evaluation and restore").
  **The limit:** the check is by identity, so it sees a name rebound or
  deleted (W3C tests 322 and 324), not a change inside the value
  (`_ioprocessors[t]['location'] = 'x'`, `_event.data.append(1)`).
- **`<foreach>`**: the array is a `list` or a `tuple`, copied shallowly
  (SCXML 4.6: the body may change it); anything else is
  `error.execution`. The item and the index may be new names: a plain
  name is declared (W3C test 150), as the ECMAScript datamodel declares
  it (`declare_if_missing`).

## Values that cross

- **Captured values are copied** through the value table
  (`docs/values.md`): `<send>`'s params, namelist and content,
  `<donedata>`, an invoke's params, a variable for `actor.context`. The
  table's immutable values are shared; lists and dicts are made again.
  An object outside the table fails the capture with `error.execution`
  (bindings.md, section 8, "Actors share no mutable value"). A namelist's
  keys are its locations as written.
- **`to_wire`, `from_wire`**: the value table, `src/values.c`. Data from
  another system that the table refuses is `TA_ERROR_INVALID`, so core
  refuses the envelope as malformed.
- **An application's data for a Python chart** (spawn's params, send's
  data) is copied through the table straight into a payload, with no CBOR
  between (the hot path, the repository's `AGENTS.md`).

## The rules the suite cannot see

Each has a test in `tests/test_datamodel.py`.

- **The last drop of a Python object waits** until the outer call into
  core returns: a payload's last `release`, a session's end (`destroy`),
  a `<foreach>`'s end and a dropped exception go to a list emptied after
  each turn, each request and the host's `close` (bindings.md, section 8).
  An ended session's globals are cleared then too: they are in a cycle
  with the functions and classes defined in them, which would otherwise
  wait for the cycle collector, and that can run during a run.
- **A `BaseException` that is not an `Exception`** (`KeyboardInterrupt`,
  `SystemExit`) is `error.execution` for the chart; the first one is kept
  and raised again, the same object, after the turn (bindings.md,
  section 8). It stops the run: the evaluation that raised it charges
  more fuel than a turn's budget (`ta_datamodel_environment_charge`), so
  the run ends when its microstep does, and the turn with it; the chart
  goes on at the next turn, so a chart that would raise again forever is
  stopped by one Ctrl-C. A handler's exception from the same turn becomes
  its `__context__` when it has none.
- **Handles are slots with generations**, for sessions, payloads and
  `<foreach>` loops (`src/datamodel/handles.c`): core holds a slot and its
  generation, never a pointer, and a stale handle is refused.

## Not in M1

`serialize`, `deserialize`, `migrate`, `payload_serialize` and
`payload_deserialize` are not offered: core answers
`TA_ERROR_UNSUPPORTED` for hibernation, export and migration of a Python
session, which are M4's (ROADMAP.md). A chart's expressions are not
checked at compile time (the root ROADMAP.md, M2, decides whether they
should be).
