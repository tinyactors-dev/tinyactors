# Shapes: a tree-pattern matcher on the event stream

Status: option A, implemented. Node identity in the events ([nodes]); the
rules, their generator, the matcher, the compiler's recording, the off
switch and the differential test ([matcher]); and the first consumers,
`toSorted` and `map` running tagged callbacks in C ([consumers], section
7). Section 9 lists the commits, each with its tests; where the
implementation proved the first draft wrong in a detail, the sections say
what changed and why. Section 10 is what may come next. Section 11 adds
numeric sorts, predicates against a literal and sums (implemented), and
designs set membership through a captured name. Section 12 adds rewrites:
rules whose match changes the code the compiler emits, matched by an
anchored lane in every region and with shapes off (implemented, with one
rewrite: a mutating method called on a path). Section 13 lets a bracket
rule take the node before it, a call its callee (11.9.2's [brackets]), and
adds pushes into reduce's accumulator (implemented).

The compiler keeps reinventing small pattern matchers over the events it
receives: OBJECT_LITERAL gathers static keys until a property breaks the
run, `call_begin` turns a GET_MEMBER into a METHOD_MEMBER by looking back
at the last instruction, and the dropped `fuse_scan` tried to recognise a
callback after the fact. Each one is hand-written, tested on its own, and
tied to the compiler's state. This design replaces that habit with one
mechanism: rules, written in one file and compiled to a C table, that a
bottom-up tree automaton (iburg-style: Pelegri-Llopart and Graham's BURS,
Fraser, Hanson and Proebsting's iburg) runs over the parser's events as they
arrive.

The first use is **callback shapes**. When an arrow function is `x => x.k`
or `(a, b) => a.k - b.k`, the matcher tags it, and `map` and `toSorted` run
the tagged callback in C instead of in bytecode. Later uses (constant
folding, superinstructions, chain fusion) are further rule sets; they are
not part of the commits of section 9.

Three properties hold throughout, for shapes; a rewrite (section 12) is
the one exception to the first and the third, by design:

- **A match only tags.** The compiler emits the same bytecode for every
  function whether or not it matched. A match records a shape on the
  function's `sjs_function_info`. A built-in may use it for a C fast path
  and must call the bytecode whenever the fast path's preconditions do not
  hold.
- **The rule.** With shapes on or off, a program gives identical values and
  identical faults (code, message, subject and position). Fuel can go down,
  never up.
- **Matching can be switched off** per compiler, without global state, so
  the tests run the whole spec both ways and compare (section 8). Since
  rewrites, the switch turns off the shapes and leaves the rewrites on
  (12.6): they are semantics, the shapes fast paths.

```
bytes --> parser --events--> matcher (tee) --same events--> compiler
                                |                              ^
                                +--- match(shape, node, args) -+
```

The tee is the compiler's own sink function, which hands each event to the
matcher before it handles it (6.1). The matcher does not depend on the
compiler. Its checks see only the events
of the subtree they match, such as an arrow's own parameter list, and never
the compiler's scopes.

---

## 1. Node identity in events

The maintainer asked for this as its own first step: give each event "its
node's start position, or a node id, so a match can say which subtree it
covered without the matcher counting events."

**Decision: both.** They answer different questions.

- `node` is the node's **identity**: unique within one parse, the same on
  every event of the node. A start position cannot be an identity, because
  nested nodes share their first token: `a`, `a.b` and `a.b(c)` all start at
  `a`. The id also tells a sink which open bracket an event belongs to. That
  is how the matcher stays width-bounded (section 5) and how it reads an
  else-if chain without counting events.
- `start` is the node's **place**: the position of its first token, which
  for an operator, a member or a call is its first operand's. It says where
  the source a match covered begins. The matcher passes it through in every
  report, and tests print it.

### 1.1 The fields

`include/staticjs.h`, appended to `sjs_event`:

```c
typedef struct {
    sjs_event_kind kind;
    uint32_t count;
    double number;
    sjs_str text;   /* valid during the call only */
    sjs_str alias;
    sjs_str name;
    sjs_position at;       /* this event's own token */
    /*
     * The syntax node the event belongs to, so that a sink can tell which
     * subtree an event closes without counting events. Every event of one
     * node carries the same `node`: a bracketed node's BEGIN, its separators
     * (ELEMENT, ARGUMENT, KEY, PARAM, PARAMS_END, ...) and its END; an
     * operator's markers (LOGICAL, CONDITIONAL, CONDITIONAL_ELSE) and its last
     * event; an arrow function's parameter list and body (BLOCK_BEGIN,
     * BLOCK_END); every IF, ELSE and the IF_END of an else-if chain; a
     * template's STRING pieces. Every other event is a node of its own.
     * The ids are unique within one parse, made as the parser reads, so they
     * need not increase in the events' order (a const's pattern is read
     * before its value and sent after it). 0 is the program, whose only event
     * is END. A sink that does not need them ignores both fields.
     *
     * `start` is where the node's source begins: its first token, which for
     * an operator, a member, an index, a call or an optional chain is its
     * first operand's. Parentheses around an operand are not part of it.
     */
    uint32_t node;
    sjs_position start;
} sjs_event;
```

An earlier draft said ids increase in the order the parser meets each
node's first token. They do not quite: CHAIN_END has no token and gets its
id when the chain ends, an import's or export's specifiers get theirs when
the statement ends, and a later declarator of `const a = 1, b = 2` gets its
at the comma. Nothing needs the order, only uniqueness, so the header
promises only that.

### 1.2 Which event carries what

"Own" means the event is the only event of a node of its own: a fresh id.
"Of X" means the event carries X's id and start.

| events | node | start |
|---|---|---|
| NUMBER, STRING, TRUE, FALSE, NULL, NAME | own | its token |
| TEMPLATE_BEGIN, its STRING pieces, TEMPLATE_END | of the template | the template's first token |
| ARRAY_BEGIN, ELEMENT, SPREAD_ELEMENT, ARRAY_END | of the array | `[` |
| OBJECT_BEGIN, KEY, COMPUTED_KEY, PROPERTY, SPREAD_PROPERTY, OBJECT_END | of the object | `{` |
| MEMBER | own | its object's start |
| INDEX | own (created at `[`) | its object's start |
| OPTIONAL | own | its object's start |
| CHAIN_END | own | the chain's first operand's start (its `at` is the bracket around the chain, which can come before it) |
| CALL_BEGIN, ARGUMENT, SPREAD_ARGUMENT, CALL_END | of the call (created at `(`) | the callee's start |
| NEW_BEGIN, ARGUMENT..., NEW_END | of the `new` expression | `new` |
| UNARY | own (created at the operator) | the operator |
| BINARY | own (created at the operator) | the left operand's start |
| LOGICAL, LOGICAL_END | of the logical expression | the left operand's start |
| CONDITIONAL, CONDITIONAL_ELSE, CONDITIONAL_END | of the `?:` | the test's start |
| ARROW_BEGIN, PARAM, REST_PARAM, PARAMS_END, BLOCK_BEGIN, BLOCK_END, ARROW_END | of the arrow function | its first token (`(` or the name) |
| RETURN | own | `return` |
| BLOCK_STATEMENT_BEGIN, BLOCK_STATEMENT_END | of the block statement | `{` |
| IF, ELSE, IF_END (a whole else-if chain) | of the chain | the first `if` |
| BIND | own | the name |
| ARRAY_PATTERN_BEGIN, PATTERN_INDEX, PATTERN_REST, ARRAY_PATTERN_END | of the pattern | `[` |
| OBJECT_PATTERN_BEGIN, PATTERN_KEY, PATTERN_COMPUTED_KEY, PATTERN_REST, OBJECT_PATTERN_END | of the pattern | `{` |
| DEFAULT, DEFAULT_END | of the default value | `=` |
| CONST_BEGIN, CONST_END | of the declarator: `const a = 1, b = 2` is two | its statement's `const` |
| EXPORT | own | `export` |
| IMPORT, EXPORT_NAME, EXPORT_FROM | own, one per specifier (made at the statement's end) | the statement's first token |
| EXPORT_DEFAULT | own | `export` |
| END | 0 | offset 0, line 1, column 1 |

A block body's BLOCK_BEGIN and BLOCK_END belong to the arrow, not to a node
of their own: a block body is the arrow's children, after PARAMS_END. The
STRING pieces of a template carry the template's id, so a sink can tell a
piece from a string literal used as a substitution (`${"x"}`), which is a
node of its own.

### 1.3 How the parser assigns them

The parser did not track operand starts: `pending_op.at` is the operator's
position, and `frame.at` is the bracket's. So this is cheap, not quite free.
It needs a counter, a few fields in each frame and pending operator, and
one small struct:

- **`event_place`** `{ sjs_position at; uint32_t node; sjs_position start; }`
  is where an event is: its own token and its node. `counted()`,
  `texted()` and `send_kind()` take one in place of the bare position, so
  they keep their three parameters, and `push_frame()` takes one too. Six
  helpers make them:
  - `own_place(p, at)`: a fresh id, starting at `at` (a leaf, a bracket, a
    unary operator, a statement);
  - `operand_place(p, at)`: a fresh id, starting at `last_start` (a binary
    or logical operator, a member, an optional, a call, an index, a
    conditional);
  - `frame_place(f, at)` and `op_place(op)`: the node a frame or a pending
    operator keeps, for its separators and its closing event;
  - `no_node(at)` for the frames that send nothing of their own (F_PAREN,
    F_LABEL, F_COMPUTED_KEY, F_EXPRESSION_STATEMENT, the program's), and
    `program_end(at)` for END: node 0, from offset 0, line 1, column 1.
- `sjs_parser.next_node` counts from 1.
- `frame` gains `node` and `start`, set by `push_frame` from its place: a
  fresh id at `[`, `{`, `(` of a call, `new`, `if`, `return`, `const`, a
  pattern's bracket, a default's `=`, and a template's head. A call's, an
  index's and a conditional's place is an `operand_place`. A COMPUTED_KEY
  and a PATTERN_COMPUTED_KEY are sent after their frame is popped, with the
  object's place. A later declarator of a `const` takes a fresh id at the
  comma, keeping the statement's start.
- `frame` also gains `chain_start`: `last_start` when `?.` opens an
  optional chain in it, which CHAIN_END (a fresh id when the chain ends)
  starts at.
- `pending_op` gains `node` and `start`. `unary(p, op, at)` makes a unary
  operator with a fresh id and its own token as start. A binary or logical
  operator gets an `operand_place` after `end_chain` and `pop_ops` have sent
  what they close. OP_ELSE takes the F_CONDITIONAL frame's id and start
  when it replaces that frame at `:`. OP_ARROW takes the arrow's.
- `sjs_parser.last_start` is the `start` of the last event passed to
  `send()`, set before `send()` decides between holding and sending, as
  `last_operand` is. When an operator token arrives, the last event sent
  completed its left operand (end_chain and pop_ops send theirs first), so
  `last_start` is that operand's start. Every closing event carries its
  node's start, so after `f(x)`, `[a]` or `(a + b)` it is the operand's
  first token, and parentheses add nothing.
- An arrow's id is made where ARROW_BEGIN is sent: in `operand_name` for
  `x =>`, and in `operand` once `decide_paren` chose an arrow, where it is
  the F_PARAMS frame's. `end_params` copies it to `sjs_parser.arrow_node`
  and `arrow_start` before it pops F_PARAMS, and `arrow_body` gives them to
  the F_BLOCK frame or to the OP_ARROW operator at once. The F_PARAMS frame
  keeps its own copy, so an arrow in a default value (`(a = x => 1) => a`)
  cannot overwrite the outer arrow's.
- `sjs_parser.export_token` is the last `export` read, the start of an
  EXPORT, an EXPORT_DEFAULT and an export statement's specifiers.
- `held_event` gains `node` and `start`. Held and rotated events keep the
  identity they were made with.
- No id is made before a lookahead that can wait for more tokens, so how
  the source is chunked cannot change the ids (spec_test compares them
  across its three push modes).
- While refusing, the parser sends nothing. Ids may skip numbers there.

### 1.4 Who uses them

- **The compiler** reads `node` once per function:
  `begin_function` stores ARROW_BEGIN's `node` in the function's scope
  (`scope.node`), so that a match report names the function it is for; and
  once per call, for a rewrite's report (section 12). It reads `start` to
  find paths (docs/mutation.md, "Assignment").
  Errors and line tables keep using `at`.
- **The matcher** uses `node` to tell which open bracket an event belongs
  to, and copies the closing event's `node` and `start` into each report. It
  never counts events.
- **Other sinks** (the parser-only bench sink, `api_test`'s `log_event`, a
  future formatter or linter) may ignore both fields. A linter gets where
  any node begins from `start`. Where it ends is not in the events: the
  `at` of a node's last event is not always its last token (CALL_END's is
  the `(`, ARROW_END's is where the body begins, IF_END's is the `if`), so
  a sink that needs the end keeps the largest `at` it sees within the node.
- **Hand-built streams.** A stream built in C and pushed into a compiler,
  as `api_test`'s `compile_events` does, carries node 0 on every event. The
  compiler does not care. The matcher treats node 0 on anything but END as a
  stream it cannot read and stops matching (section 5.5).

### 1.5 Cost

`sjs_event` grows by 16 bytes (80 to 96), and so do `held_event` (48 to
64) and `pending_op` (16 to 32); `frame` grows by 28 (with `chain_start`).
With `max_held_events` at 65,536, the held list's worst case grows by
1 MiB. Each sent event costs one more counter increment and a few more
stores. The [nodes] commit quotes `bench-compare` for `compile.*`: no
throughput change it can resolve, and on `bench/modules/power.js` the
parser's stacks grow once more (an allocation per compile, 113 to 114) and
peak 624 bytes higher.

---

## 2. The tree the events spell

The event stream is the syntax tree in evaluation order. Every node's last
event comes after its children, which is exactly what a bottom-up automaton
needs. Five actions describe how each event builds the tree. They are
declared once, in the first section of `src/shapes.rules`, and the generator
turns them into the matcher's table (`shape_events`).

| action | meaning | examples |
|---|---|---|
| `leaf` | a node of its own with no children | NUMBER, NAME, BIND, EXPORT |
| `postfix N` | a node of its own over the N nodes before it | MEMBER (1), BINARY (2), INDEX (2) |
| `postfix count` | as `postfix 1` when its count is 1, `leaf` when 0 | RETURN |
| `open N CLOSE` | begins a bracketed node whose children follow until CLOSE; the N nodes before it are its first children | ARRAY_BEGIN (0), CALL_BEGIN (1: the callee), LOGICAL (1), CONDITIONAL (1), IF (1: the test) |
| `inner` | always an event of the bracket it is in: a separator | ELEMENT, ARGUMENT, KEY, PARAM, PARAMS_END |
| `close` | ends the bracket it is in | ARRAY_END, CALL_END, LOGICAL_END, ARROW_END, END |

Two rules make this exact without per-kind exceptions:

1. **Ownership by id.** An event whose `node` equals the innermost open
   bracket's `node` is an event of that bracket, whatever its declared
   action. If its action is `close` it ends the bracket; otherwise it is a
   separator. This is how a template's STRING pieces (declared `leaf`) and
   the second IF of an else-if chain (declared `open 1`) are read as
   separators.
2. **Items.** A node flagged `item` is never an operand: it is always a
   whole child of the bracket it is in. Statements, patterns, defaults,
   BIND and the module forms are items.

With both rules, the matcher knows that every cell above the innermost
marker is a complete child as soon as a separator, a close or an item
arrives. It folds those cells into the bracket's state at once (section
5.3). An array of 100,000 elements, a module of 1,000 consts or a template
of many substitutions never holds more than a few cells.

---

## 3. The rule language: `src/shapes.rules`

### 3.1 Grammar

```
file         = { event | shape | rewrite | nonterminal | rule } ;
rewrite      = "rewrite" UPPER ";" ;       (* section 12 *)
event        = "event" KIND action [ "item" ] [ field { "," field } ] ";" ;
action       = "leaf" | "postfix" ( INT | "count" ) | "open" INT KIND
             | "inner" | "close" ;
field        = lower ":" type ;            (* a name for a member of the event *)
type         = "count" | "operator" | "number" | "text" | "name" | "read" ;
shape        = "shape" UPPER "(" [ param { "," param } ] ")" ";" ;
param        = lower ":" ( "text" | "flag" | "count" | "number" ) ;
nonterminal  = "nonterminal" lower "(" [ attribute { "," attribute } ] ")" ";" ;
attribute    = lower ":" type ;
rule         = head "<-" pattern [ "cost" INT ] ";" ;
head         = ( lower | UPPER ) "(" [ term { "," term } ] ")" ;
pattern      = KIND [ "{" test { "," test } "}" ] [ "(" [ child { "," child } ] ")" ]
             | lower [ "(" [ term { "," term } ] ")" ] ;    (* a nonterminal *)
child        = pattern | "_" | "..." ;     (* `_` and `...`: a rewrite's rules only, section 12 *)
test         = lower ":" term ;            (* a field of the event *)
term         = lower                       (* a variable; `_` is a new one each time *)
             | INT | NUMBER | STRING | "true" | "false" | UPPER ;  (* a constant; UPPER: an sjs_operator *)
```

`KIND` is an `sjs_event_kind` without `SJS_EV_`. Comments run from `#` to
the end of the line. `NUMBER` is digits with a fraction (`1.5`); a string
escapes only `\"` and `\\`. The keywords (`event`, `shape`, `nonterminal`,
the actions, `item`, `cost`, `true`, `false`) are not variables or
nonterminals, and names beginning with `_` are the generator's.

- **Field types** say which member of `sjs_event` a field reads and what it
  may be compared with. `count` and `operator` read `count`, `number` reads
  `number`, and `text`, `name` and `read` read `text`. `name` is a name a
  pattern binds (BIND), and `read` is a name an expression reads (NAME). The
  two compare as text, and the generator uses the difference for the
  closure check in 4.3.
- **Variables and non-linear patterns.** A variable is bound where it first
  appears in a pattern. Every other appearance is a check that the values
  are equal. In `key_of(x, k) <- MEMBER{key: k}(NAME{name: x})`, `x` and `k`
  are bound. In the PLUCK rule below, `BIND{name: x}` and `key_of(x, k)`
  both name `x`, so the rule holds only when the name read is the name
  bound: "this name is the arrow's parameter 0". Two `key_of(_, k)` in one
  rule require both keys to be the same. Text compares by bytes, numbers by
  `==` and counts as integers.
- **Constants in tests** check a field: `ARROW_BEGIN{params: 1}`,
  `BINARY{op: SUBTRACT}`.
- **Children.** For a `postfix N` event, the parentheses hold exactly N
  patterns. For `postfix count`, they hold one pattern (count 1) or none
  (count 0). For an `open` event, they hold the bracket's whole child
  sequence, left to right, and the CLOSE is implied. In that sequence, an
  event pattern whose kind is declared `inner` matches a separator; any
  other pattern matches a child node. The sequence is fixed-length: a
  bracket with more or fewer children does not match. (Repetition, `x*` as
  the last child, is left for when a rule needs it.)
- **Heads.** A rule derives a nonterminal or a shape. Its head's terms give
  the derived attributes, in the declared order: variables bound in the
  pattern, or constants (`COMPARE_BY_KEY(k, true)`).
- **Chain rules.** A rule whose pattern is a lone nonterminal
  (`value(x) <- key_of(o, x)`) is a chain rule, as in iburg. Chain rules
  must not form a cycle.
- **Costs.** A rule costs `cost N` (0 if omitted) plus the costs of its
  children's derivations. At each node, each nonterminal keeps its cheapest
  derivation. Ties go to the rule written first. A node that derives more
  than one shape reports the cheapest.

### 3.2 The first rule set

The file has two sections: the event language, then the shapes and their
rules.

```
# src/shapes.rules: the trees the matcher looks for in the parser's events
# (docs/matcher.md). `mise run shapes` writes src/shapes.h and
# src/shapes_table.h from it; `mise run test` fails when they are stale.

# ---------------------------------------------------------------------------
# The event language: how each event of include/staticjs.h builds the tree.
# Every sjs_event_kind is declared once; the generator checks the header.

event NUMBER leaf value: number;
event STRING leaf value: text;          # a template's pieces are the template's own events
event TRUE leaf;
event FALSE leaf;
event NULL leaf;
event NAME leaf name: read;
event TEMPLATE_BEGIN open 0 TEMPLATE_END;
event TEMPLATE_END close parts: count;
event ARRAY_BEGIN open 0 ARRAY_END;
event ELEMENT inner;
event SPREAD_ELEMENT inner;
event ARRAY_END close;
event OBJECT_BEGIN open 0 OBJECT_END;
event KEY inner key: text;
event COMPUTED_KEY inner;
event PROPERTY inner;
event SPREAD_PROPERTY inner;
event OBJECT_END close;
event MEMBER postfix 1 key: text;
event INDEX postfix 2;
event OPTIONAL postfix 1 depth: count;
event CHAIN_END postfix 1 depth: count;
event CALL_BEGIN open 1 CALL_END;       # the callee is its first child
event ARGUMENT inner;
event SPREAD_ARGUMENT inner;
event CALL_END close arguments: count;
event NEW_BEGIN open 0 NEW_END constructor: text;
event NEW_END close arguments: count;
event UNARY postfix 1 op: operator;
event BINARY postfix 2 op: operator;
event LOGICAL open 1 LOGICAL_END op: operator;
event LOGICAL_END close op: operator;
event CONDITIONAL open 1 CONDITIONAL_END;
event CONDITIONAL_ELSE inner;
event CONDITIONAL_END close;
event ARROW_BEGIN open 0 ARROW_END params: count;
event PARAM inner index: count;
event REST_PARAM inner index: count;
event PARAMS_END inner;
event BLOCK_BEGIN inner;                # a block body's statements are the arrow's children
event RETURN postfix count item;
event BLOCK_END inner;
event ARROW_END close;
event BLOCK_STATEMENT_BEGIN open 0 BLOCK_STATEMENT_END item;
event BLOCK_STATEMENT_END close;
event IF open 1 IF_END item;            # the test is its first child; an else-if's IF is its own event
event ELSE inner chain: count;
event IF_END close;
event BIND leaf item name: name;
event ARRAY_PATTERN_BEGIN open 0 ARRAY_PATTERN_END item;
event PATTERN_INDEX inner index: count;
event ARRAY_PATTERN_END close;
event OBJECT_PATTERN_BEGIN open 0 OBJECT_PATTERN_END item;
event PATTERN_KEY inner key: text;
event PATTERN_COMPUTED_KEY inner;
event PATTERN_REST inner index: count;
event OBJECT_PATTERN_END close;
event DEFAULT open 0 DEFAULT_END item;
event DEFAULT_END close;
event CONST_BEGIN open 0 CONST_END item;
event CONST_END close;
event EXPORT leaf item;
event IMPORT leaf item module: text;
event EXPORT_DEFAULT postfix 1 item;
event EXPORT_NAME leaf item;
event EXPORT_FROM leaf item module: text;
event END close;                        # closes the program (node 0)

# ---------------------------------------------------------------------------
# Callback shapes. The compiler records a shape on the function it matched;
# map and toSorted run a tagged callback in C (docs/matcher.md, section 7).

shape PLUCK(key: text);                              # x => x.key
shape COMPARE_BY_KEY(key: text, descending: flag);   # (a, b) => a.key - b.key, or b.key - a.key

nonterminal key_of(object: read, key: text);         # object.key, the object a name

key_of(x, k) <- MEMBER{key: k}(NAME{name: x});

# x => x.k, and x => { return x.k; }: x is parameter 0, the only one, a plain name.
PLUCK(k) <- ARROW_BEGIN{params: 1}(PARAM{index: 0}, BIND{name: x}, PARAMS_END,
                                   key_of(x, k));
PLUCK(k) <- ARROW_BEGIN{params: 1}(PARAM{index: 0}, BIND{name: x}, PARAMS_END,
                                   BLOCK_BEGIN, RETURN(key_of(x, k)), BLOCK_END);

# (a, b) => a.k - b.k ascending; (a, b) => b.k - a.k descending. One key, both sides.
COMPARE_BY_KEY(k, false) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a},
                                                   PARAM{index: 1}, BIND{name: b}, PARAMS_END,
                                                   BINARY{op: SUBTRACT}(key_of(a, k), key_of(b, k)));
COMPARE_BY_KEY(k, true) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a},
                                                  PARAM{index: 1}, BIND{name: b}, PARAMS_END,
                                                  BINARY{op: SUBTRACT}(key_of(b, k), key_of(a, k)));
COMPARE_BY_KEY(k, false) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a},
                                                   PARAM{index: 1}, BIND{name: b}, PARAMS_END,
                                                   BLOCK_BEGIN, RETURN(BINARY{op: SUBTRACT}(key_of(a, k), key_of(b, k))), BLOCK_END);
COMPARE_BY_KEY(k, true) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a},
                                                  PARAM{index: 1}, BIND{name: b}, PARAMS_END,
                                                  BLOCK_BEGIN, RETURN(BINARY{op: SUBTRACT}(key_of(b, k), key_of(a, k))), BLOCK_END);
```

Why these rules are sound:

- **No shadowing.** The hedge is exact, so the arrow has nothing between its
  parameter list and its body: no `const`, no block statement, no nested
  arrow. A name read in the body can only be a parameter, and the variable
  ties it to that parameter.
- **No hidden behaviour.** A default (`(x = d) => x.k`), a pattern
  (`({k}) => k`), a rest parameter (`(...x) => ...`), a third parameter, an
  optional chain (`x => x?.k`), an index (`x => x["k"]`) and a captured name
  (`x => y.k`) each add, change or replace an event, so none of them
  matches. Each has a negative spec case (section 9).
- **Duplicate parameters** (`(a, a) => a.k - a.k`) do not compile, so a
  match there is never used. No `a != b` check is needed.

### 3.3 Not in the first set: key equality

`x => x.k === c`, with `c` a literal, would give `filter`, `find`,
`findIndex`, `some` and `every` a C path. It needs one more attribute type,
`literal` (a kind, with its number or text), so that one nonterminal can
match STRING, NUMBER, TRUE, FALSE and NULL:

```
shape KEY_EQUALS(key: text, value: literal);
nonterminal constant(value: literal);
constant(v) <- STRING{value: v};
constant(v) <- NUMBER{value: v};
...
KEY_EQUALS(k, c) <- ARROW_BEGIN{params: 1}(PARAM{index: 0}, BIND{name: x}, PARAMS_END,
                                          BINARY{op: STRICT_EQUAL}(key_of(x, k), constant(c)));
```

`NAME undefined` and `NaN` are not literals here, because a module can
rebind `undefined`, and `-0` is a UNARY. This shape lands with its first
consumer, not before. Nothing in the commits of section 9 uses it, and the
repository removes what nothing uses. Section 11 designs it as KEY_COMPARE
and ELEMENT_COMPARE, with every comparison operator and the type named
`operand`, and 11.7.3 implements it.

---

## 4. The generator

### 4.1 Input and output

- **Input:** `src/shapes.rules`, and `include/staticjs.h` for the list of
  `sjs_event_kind` and `sjs_operator` names.
- **Output** (checked in, with a "generated by `mise run shapes`; do not
  edit" banner):
  - `src/shapes.h`: what the rest of `src/` and the tests may include. It
    holds the shape ids, their argument kinds and their names.
    `src/internal.h` includes it, for `sjs_function_info`.
  - `src/shapes_table.h`: the matcher's tables. Only `src/matcher.c`
    includes it, so the static tables appear once in every build and once
    in check-recursion's amalgamation.
- **Code:** `mise/shapes.rb` (module `StaticJS::Shapes`: lexer, parser,
  checks, normalisation, emitter), Ruby 4, beside `mise/staticjs.rb`.
- **Tasks** (Ruby file tasks in `mise/tasks/`, house style):
  - `mise run shapes` writes both headers.
  - `mise run shapes --check` writes nothing and fails if either header
    differs from what it would write ("run `mise run shapes` and commit the
    result"), as tinyxml's `generated` does.
  - `mise run shapes-test` is the generator's self-test, in the style of
    `check-acronyms-test`: a list of cases and a `failures` array, with no
    test framework. Its small rules file and the C it must produce are in
    `mise/shapes-test/`.
  - `mise run test` (`mise/tasks/test/_default`) runs `shapes --check` and
    `shapes-test` before `check-parameters`. Its `#MISE sources` already
    cover `src/**` and `mise/**`.

### 4.2 Normalisation

As in iburg, nested patterns become fresh, numbered nonterminals (`_1`,
`_2`, ...), so that every rule has one of four forms:

| form | written | example |
|---|---|---|
| leaf | `nt <- KIND{tests}` | `_1(x) <- NAME{name: x}` |
| postfix | `nt <- KIND{tests}(nt1, ..., ntN)` | `key_of(x, k) <- MEMBER{key: k}(_1(x))` |
| bracket | `nt <- OPEN{tests}(sym1, ..., symM)`, each sym a nonterminal or an inner-event pattern | `PLUCK(k) <- ARROW_BEGIN{params: 1}(PARAM{index: 0}, _2(x), PARAMS_END, key_of(x, k))` |
| chain | `nt <- nt2` | `value(x) <- key_of(o, x)` |

A fresh nonterminal's attributes are the variables its subtree binds that
the pattern around it uses, in the order the matcher meets them. An
equality between two occurrences is checked at the lowest rule where both
are available: a leaf's or postfix's own tests, or the rule that holds both
children. Identical normalised rules are merged, so `NAME{name: x}` under
both `key_of`s, and `BIND{name: a}` and `BIND{name: b}`, are one
nonterminal each; so are `a.k - b.k` and `b.k - a.k`, whose attributes are
the same three variables in another order.

A rule's variables are numbered in the order the matcher meets them: the
node's own tests, then its children left to right. Each test and each link
of a child's attribute says whether it is the variable's first occurrence
(bind it) or a later one (compare), so the matcher keeps no record of what
is bound. Each rule's row in the table carries its normalised form as a
comment.

### 4.3 Checks (errors name `shapes.rules:LINE`)

- Every `sjs_event_kind` in `include/staticjs.h` is declared exactly once.
  Every `close` kind but END closes exactly one `open` kind, and an
  `open`'s CLOSE is declared `close`. END is declared `close`, and closes
  the program.
- A pattern has the right number of children for its event's action.
  `inner` events appear only inside a bracket's child list, `close` events
  never appear in patterns, and a shape is never a child.
- **No rule on an `open N` event with N > 1**, and a rule on one with
  N = 1 (a call, a logical or conditional operator, an if) has a node, not
  a separator, as its first child: the node before the open, its callee or
  left operand, which the matcher folds into the marker when the bracket
  opens (11.9.2, section 13). It keeps that node's cell only where a rule
  can still match; in a dead region the bracket is part of the region, as
  before. (Until section 13, every N > 0 was refused.)
- Tests name declared fields. Constants have the field's type (an operator
  name for `operator`). Every variable in a head is bound, and a variable's
  occurrences agree in type (`name` and `read` agree with each other). A
  head's text comes from its pattern, never a constant.
- **Closure check.** A `read` variable (a name an expression reads) is
  bound from a `name` (a BIND) in the same rule, or, in a nonterminal's
  rule, handed up as a `read` attribute for a rule above to bind. In a
  shape rule it must be bound. Without it, a rule could match `x => y.k`
  with `y` a capture, or hide a read name inside a nonterminal.
- Every shape rule's pattern is an `ARROW_BEGIN` bracket, because the
  compiler records shapes on functions. The check goes when another node
  kind gets shapes.
- Every shape and every declared nonterminal has a rule, and every
  declared nonterminal is used.
- Chain rules form no cycle. The generator emits them in topological order,
  so the matcher closes over them in one pass.
- Size limits fit the table's types: at most 63 nonterminals (a cell keeps
  a 64-bit mask of what it derives), at most 32 bracket rules per open kind
  (the `alive` mask), at most 32 variables per rule, and costs at most
  65534.

### 4.4 `src/shapes.h` (generated)

```c
#pragma once
#include <stdint.h>

typedef enum {
    SJS_SHAPE_NONE = 0,
    SJS_SHAPE_PLUCK,          /* key: text */
    SJS_SHAPE_COMPARE_BY_KEY, /* key: text, descending: flag */
    SJS_SHAPE_COUNT
} sjs_shape;

/* The most arguments a shape has. */
#define SJS_SHAPE_ARGS 2u

/* What a shape's argument is: a text (recorded as a string constant), a
 * flag (0 or 1), a count, or a number (a number constant). */
enum { SJS_SHAPE_ARG_NONE = 0, SJS_SHAPE_ARG_TEXT, SJS_SHAPE_ARG_FLAG, SJS_SHAPE_ARG_COUNT, SJS_SHAPE_ARG_NUMBER };

/* Argument `i` of `shape`: SJS_SHAPE_ARG_NONE past its last. */
static inline uint8_t sjs_shape_arg_kind(uint32_t shape, uint32_t i) { /* a switch */ }
/* "PLUCK", ...: for tests and messages. */
static inline const char *sjs_shape_name(uint32_t shape) { /* a switch */ }
```

Inline functions, rather than tables, keep `-Wunused-const-variable` quiet in
every translation unit that includes the header and uses none of them. The
function is `sjs_shape_arg_kind`, not `sjs_shape_arg`, because the matcher's
argument value is a type (`sjs_shape_value`), and C keeps one namespace for
both.

### 4.5 `src/shapes_table.h` (generated)

```c
#define SHAPE_NONTERMINALS 9u   /* 0 is none; shapes first (1..SJS_SHAPE_COUNT-1), then the rest */
#define SHAPE_SLOTS 16u         /* the larger of a cell's (every nonterminal's attributes) and a
                                 * marker's (the variables of one open kind's bracket rules) */
#define SHAPE_VARIABLES 3u      /* the most variables one rule has */
#define SHAPE_COSTS 9u          /* the larger of the nonterminals and one open kind's bracket rules */
#define SHAPE_TEXT_MAX 64u      /* a longer captured text fails its test */
#define SHAPE_INFINITE 0xFFFFu  /* costs saturate below it */
#define SHAPE_CONSTANT 0xFFu    /* a test or a result with a constant, not a variable */
#define SHAPE_RULE_COUNT 12u
#define SHAPE_CHAIN_COUNT 0u
#define SHAPE_NUMBER_COUNT 0u
#define SHAPE_TEXT_COUNT 0u

enum { SHAPE_LEAF = 1, SHAPE_POSTFIX, SHAPE_POSTFIX_COUNT, SHAPE_OPEN, SHAPE_INNER, SHAPE_CLOSE };
enum { SHAPE_FIELD_COUNT = 1, SHAPE_FIELD_NUMBER, SHAPE_FIELD_TEXT };
enum { SHAPE_FORM_LEAF = 1, SHAPE_FORM_POSTFIX, SHAPE_FORM_BRACKET, SHAPE_FORM_CHAIN };

typedef struct { uint8_t action, arity, item, close; uint16_t first_rule, rule_count; } shape_event;
typedef struct { uint8_t field, variable, bind; uint32_t constant; } shape_test;
typedef struct { uint8_t nonterminal, kind, test_count, link_count; uint16_t first_test, first_link; } shape_child;
typedef struct { uint8_t variable, bind; } shape_link;      /* attribute i of a child, in order */
typedef struct { uint8_t variable; uint32_t constant; } shape_result;
typedef struct {
    uint8_t lhs, form, kind, child_count, test_count, first_slot, variable_count;
    uint16_t cost, first_test, first_child, first_result, first_variable;
} shape_rule;

static const shape_event shape_events[SJS_EV_END + 1];     /* by designated initialisers */
static const shape_rule shape_rules[SHAPE_RULE_COUNT];     /* each with its normalised form */
static const shape_test shape_tests[];
static const shape_child shape_children[];
static const shape_link shape_links[];
static const shape_result shape_results[];
static const uint8_t shape_variable_fields[];              /* SHAPE_FIELD_* of each rule's variables */
static const uint16_t shape_chain_rules[];                 /* in topological order */
static const uint8_t shape_slot[SHAPE_NONTERMINALS];       /* a nonterminal's first slot in a cell */
static const uint8_t shape_attribute_count[SHAPE_NONTERMINALS];
static const uint8_t shape_slot_field[];                   /* SHAPE_FIELD_* of each slot of a cell */
static const double shape_numbers[];
static const sjs_str shape_texts[];
```

Leaf and postfix rules are grouped by event kind, and bracket rules by open
kind, so `shape_events[kind]` gives a contiguous range; chain rules come
last. A kind no rule mentions has `rule_count` 0. C has no empty arrays, so
an unused one holds one placeholder that its count never reaches.

### 4.6 The generator's tests (`mise run shapes-test`)

- A small rules file (`mise/shapes-test/small.rules`, with a small
  `staticjs.h`) and the exact C it must produce (`small_shapes.h` whole,
  and `small_table.txt`, the table's sizes and data), covering every rule
  form, fresh nonterminals, merged duplicates, two chain rules written in
  the opposite order to the one they run in, costs, and a number and a text
  constant.
- Each check in 4.3 has a rules snippet that must fail with its message and
  line, 37 in all, among them the size limits on bracket rules and costs.
- `shapes --check` fails on missing and stale headers and passes on fresh
  ones. The test runs it on a copy in a temporary directory (`--root`),
  never on `src/`.
- The real `src/shapes.rules` generates; the checks make every declaration
  used.
- Two generations give identical bytes.

---

## 5. The runtime matcher

### 5.1 Interface: `src/matcher.h` (private)

```c
typedef struct {
    uint8_t kind;          /* SJS_SHAPE_ARG_* */
    uint32_t count;        /* a flag or a count */
    double number;
    sjs_str text;          /* valid during the report only */
} sjs_shape_value;

/* A node that derives a shape: what, which node, from where, with what. */
typedef struct {
    sjs_shape shape;
    uint32_t node;         /* the event's node: for now always an arrow function */
    sjs_position start;
    sjs_shape_value args[SJS_SHAPE_ARGS];
} sjs_shape_match;

typedef struct sjs_shape_reporter_vtable {
    /* SJS_OK to go on; any other result is sjs_matcher_event's. */
    sjs_result (*report)(void *self, const sjs_shape_match *match);
} sjs_shape_reporter_vtable;

typedef struct { void *self; const sjs_shape_reporter_vtable *v; } sjs_shape_reporter;

typedef struct {
    const sjs_allocator *allocator;   /* the context's counting allocator */
    uint32_t max_nesting;             /* sizes its bound: 4 * max_nesting + 4 records */
    sjs_shape_reporter reporter;
} sjs_matcher_config;

typedef struct { ... } sjs_matcher;   /* its owner holds it: the compiler, in itself */

void sjs_matcher_init(sjs_matcher *matcher, const sjs_matcher_config *config);
/* Match one event, before its sink sees it: SJS_OK; SJS_ERR_NOMEM; or the
 * reporter's result. An unreadable stream is SJS_OK: the matcher stops. */
sjs_result sjs_matcher_event(sjs_matcher *matcher, const sjs_event *event);
void sjs_matcher_free(sjs_matcher *matcher);
```

**Changed from the first draft:** the matcher is not a sink of its own with
a downstream. Its owner calls `sjs_matcher_event` first and then handles
the event itself, so the compiler's own sink is the tee (6.1). A separate
sink returned the matcher's out-of-memory to the parser as a sink's
refusal, which left the compiler with no error to report, and cost an
indirect call per event. The struct is public to its owner, so that the
compiler holds it in itself and the matcher allocates nothing until it
needs records; `peak` and `stopped` are fields the tests read.

A report about a node arrives during the node's last event, before the
owner handles that event: the compiler learns that a function has a shape
before it sees the function's ARROW_END.

### 5.2 Data

The matcher keeps a stack of records, and the text they captured:

```c
/* A cell (a complete node) or a marker (an open bracket), of one stride. */
typedef struct {
    uint64_t derived;      /* cell: a bit per nonterminal it derives */
    uint32_t node;
    uint32_t text_base;    /* its first byte in the text */
    uint32_t outer;        /* marker: the marker around it */
    uint32_t alive;        /* marker: a bit per bracket rule of its kind still matching */
    uint32_t children;     /* marker: children folded so far */
    uint8_t kind, marker, dead;
    uint16_t cost[SHAPE_COSTS];       /* cell: per nonterminal; marker: per bracket rule */
    shape_value slots[SHAPE_SLOTS];   /* cell: by shape_slot; marker: by each rule's first_slot */
} shape_record;

/* A slot: what its field says, so 8 bytes: a number, a count, or a text,
 * as an offset and a length in the text (or the event's own text, or
 * none). */
typedef union { double number; uint32_t count; struct { uint32_t offset, length; } text; } shape_value;
```

A cell keeps, for each nonterminal it derives, the cost and that
derivation's attributes in the nonterminal's slots. A marker keeps, for
each bracket rule of its open kind still alive, the cost so far and the
variables bound so far. The first draft kept, per cell, the rule that
derived each nonterminal; nothing reads it until rules emit code, so it is
gone.

**Text** captured from events (a key, a parameter's name) is copied, since
`sjs_event.text` lives only during the call. It goes into the text stack in
record order, and every record knows where its text begins. When records
are replaced by one (a postfix node, a bracket's close), the text the new
record still refers to is moved down to the region's start, in increasing
offset order (a forward copy, as the destination is never past the
source), and the stack is cut after it. When cells are folded into a
marker, their text stays, now the marker's, until the marker closes or
dies: a marker that can still match has at most as many children as its
longest rule, so what it keeps is bounded, and the fold moves nothing. A
text longer than SHAPE_TEXT_MAX is not captured, and its test fails: a key
that long only loses the fast path. The first 64 bytes of text are in the
matcher itself; past them the text moves to the heap.

### 5.3 Per event

One fact makes most events free. Every rule wants each child of a bracket
to derive some nonterminal, or to be a given separator. So a child that
derives nothing kills every rule of its bracket, and once a bracket can
match no more, nothing inside it is a child of any rule again: its leaves,
its operators and the brackets inside it can only end up folded into it.
The only exception is a bracket inside it that rules look at, which can
derive a shape of its own (an arrow inside a call). The matcher calls such
a dead bracket a **dead region**, and keeps of it one record, its marker,
or none at all for the program, which no rule matches.

```
on_event(e):                                  # sjs_matcher_event
    if stopped: return OK
    if e.kind is not an event kind, or e.node == 0 but e is not END:
        stop; return OK                       # 5.5
    if the innermost marker is dead (a dead region):
        if e.node != its node and e is not an open: return OK
    r = step(e)
    if r == UNREADABLE: stop, free the stacks; return OK
    return r                                  # OK, out of memory, or the reporter's result

step(e):
    m = the innermost marker (the program, when there is none)
    if e.node == m.node:                      # an event of m's bracket
        if action(e) == CLOSE:
            if m is the program: return END ? OK : UNREADABLE
            if e.kind != close_of(m.kind): return UNREADABLE
            return finish(m)
        if m is dead: return OK
        fold the cells above m, then the separator e, into m
        if m died: doom(m)
        return OK
    if action(e) == OPEN: return open(e)
    if m is dead: return OK
    LEAF, POSTFIX N, POSTFIX_COUNT:
        the top N records must be cells above m, else UNREADABLE
        no rule on e.kind:  doom(m); return OK
        cell = derive(e, those N)             # iburg's labeller at one node
        cell derives nothing: doom(m); return OK
        replace them by cell; created(cell)
    INNER, CLOSE: return UNREADABLE           # an event of no open bracket

open(e):
    if m is alive and fewer than arity(e) cells are above it: return UNREADABLE
    no rule on e.kind, or arity(e) > 0 and m is dead:   # it will derive nothing
        if m is alive: doom(m)
        return OK                             # in a dead region: part of it
    the bracket rules of e.kind whose open tests pass are alive
    arity(e) is 1: fold the top cell into them as child 0   # section 13
    none alive: if m is alive: doom(m); return OK
    push a marker for e, in the top cell's place when arity(e) is 1

finish(m):                                    # m's bracket closes
    if m is alive: fold the cells above it into it
    cell = what m's rules derive, if each alive rule has all its children
    pop m
    cell derives nothing: if the marker around is alive, doom it
    else: created(cell)

created(cell):
    if cell derives a shape: report(cheapest shape, e.node, e.start, its attributes)
    if the innermost marker is dead: drop cell
    elif item(cell.kind): fold the cells above the innermost marker into it

doom(m):  m can match no more: drop every record and every byte of text above
          its own start, and keep m, dead, as a dead region.
```

- **`derive(e, children)`**: for each leaf or postfix rule on `e.kind`
  with as many children, its own tests run against `e`'s fields:
  constants compare, variables bind or compare. Each child must derive the
  rule's nonterminal at that position; its attributes are linked into the
  rule's variables. The cost is the rule's plus the children's,
  saturating. If the cell does not derive the rule's lhs yet, or does
  only at a higher cost, the rule's derivation replaces it, with the lhs's
  attributes from `shape_results`. Then the chain rules run once, in
  topological order. Ties go to the rule first in the table.
- **Folding** child number `m.children` into each alive bracket rule `r`:
  if `r` has no child at that position, or the position wants a nonterminal
  the child does not derive, or a separator the child is not (or that
  fails its tests), `r` dies. Otherwise its variables are linked and its
  cost accumulates. A marker with no rule alive is dead.
- **Reports** come from `created`, before the closing event reaches the
  owner, with the shape's attributes as `sjs_shape_value`s, a text pointing
  into the text stack. A reporter result other than SJS_OK stops the
  event: it is returned, and the owner does not handle the event.
- **Not checked in a dead region:** what is passed over is not read, so a
  postfix with too few operands, a stray separator or a stray close there
  does not stop the matcher. Node 0 and unknown kinds stop it anywhere.

Nothing recurses: rules are table rows, children a loop, chain rules one
pass in a fixed order. Every function has at most three parameters (the
step's state is one struct). File-scope names begin with `matcher_` or
`shape_`, so they mean one thing in check-recursion's amalgamation.

### 5.4 Example

`xs.map(x => x.k)`:

| event | node | what happens | records after it |
|---|---|---|---|
| NAME xs | 1 | the program is a dead region: passed over | (none) |
| MEMBER map | 2 | passed over | |
| CALL_BEGIN | 3 | an open no rule looks at, in a dead region: passed over | |
| ARROW_BEGIN(1) | 4 | rules look at it; PLUCK×2 pass `params: 1`: a marker | arrow⟨4⟩ |
| PARAM(0) | 4 | a separator of the arrow: PARAM{index 0} passes | arrow |
| BIND x | 5 | a cell deriving `_2(x)`; an item, so folded at once | arrow |
| PARAMS_END | 4 | a separator | arrow |
| NAME x | 6 | a cell deriving `_1(x)` | arrow, _1(x) |
| MEMBER k | 7 | replaces it by a cell deriving `key_of(x, k)` | arrow, key_of(x, k) |
| ARROW_END | 4 | close: key_of folded as child 4 (`x` = `x`); the block-body PLUCK dies, the other derives PLUCK(k): **report PLUCK, node 4, start 7, key "k"**; the program is dead, so the cell is dropped | (none) |
| ARGUMENT, CALL_END, END | 3, 3, 0 | passed over; END closes the program | |

### 5.5 Streams it cannot read

Where a bracket can still match, a stream that cannot be the tree of
section 2 (a close that does not match the open bracket, a separator of no
bracket, too few cells for a postfix), and anywhere node 0 on an event
other than END, an event kind out of range, or the bound exceeded (5.6),
make the matcher stop matching, free its stacks and let everything pass
from then on. It never fails the compile for this: the compiler reports a
malformed stream itself ("the events are out of order"), and a stream it
accepts compiles the same, untagged. Reports made before the stop stand;
each one covered a complete subtree. matcher_test checks that the parser
never makes the matcher stop (every spec source, the deepest programs), and
api_test's `compile_events` streams, which carry node 0, compile and fail
exactly as before.

### 5.6 Memory

- **Records.** A record is a marker of a bracket a rule can still match, a
  cell of a node inside one, or a marker that died and stands for its dead
  region. Separators, closes and items fold everything above their
  bracket, so neither width nor the number of statements adds records, and
  a dead region holds none for the brackets in it. Records ≤ 2 × (frames +
  operators) + 2 ≤ **4 × max_nesting + 4**, as the first draft bounded
  them; in practice only nested arrows that can still match reach depth.
  matcher_test measures it: at most 5 records over every spec source, and
  at max_nesting nested arrows 256 records, arrays, calls, operators,
  templates and patterns none.
- **Per record:** with the first rule set, 184 bytes (a 40-byte header, 9
  costs, 16 slots of 8 bytes).
- **Allocations:** none until a record is needed, so a program without an
  arrow a rule can match allocates nothing. The record stack's first
  allocation is 1 KiB (5 records), and grows by doubling. The text's first
  64 bytes are in the matcher, which the compiler holds in itself; past
  them, one more allocation. Memory goes through the context's counting
  allocator, so `sjs_context_live_bytes` sees it and `spec_test --full`
  fails each allocation.
- **Beyond the bound** the matcher stops matching (5.5) instead of
  allocating further.

Precomputed BURS state tables would replace the cost vectors with one state
number per record. They are worth it only if a compile profile shows the
labeller hot; section 6.5 has the profile.

---

## 6. The compiler records the shape

### 6.1 Wiring

- `sjs_compiler` holds an `sjs_matcher`, initialised in
  `sjs_compiler_init`, with the context's counting allocator,
  `limits.max_nesting`, and the compiler as reporter; with `no_shapes`
  (section 8) it matches the rewrites alone (section 12). Before rewrites,
  it held `bool matching` and made no matcher with `no_shapes`. The
  compiler's sink function hands each event to `sjs_matcher_event` before
  it handles it itself: that is the tee.
  `sjs_compiler_sink()` and `sjs_compile()` are unchanged, and every sink a
  compiler gives out goes through the tee.
- A matcher failure (out of memory) is the compiler's own: the sink sets
  `c->error` with `fail_result`, as for any allocation of the compiler, and
  returns SJS_ERR_PROGRAM, so `sjs_compiler_error` reports it.
- `scope` gains `uint32_t node`, set by `begin_function` from ARROW_BEGIN.

### 6.2 Recording

The reporter, `on_shape(c, match)`, runs during the ARROW_END that closes
the matched function, before the compiler sees that ARROW_END:

1. If the compiler has failed, return its error.
2. Find the innermost function scope. If it is the program's, or its
   `node` is not `match->node`, ignore the report. Every shape is an
   arrow's, reported at its ARROW_END, so this does not happen unless the
   parser's ids are wrong; matcher_test checks, for a list of sources, that
   the compiler tags as many functions as the matcher reports.
3. On the function's `sjs_function_info`, set `shape` and convert each
   argument by its kind:
   - A **text** becomes `string_constant()`, the same interned, `NAMED`
     string constant GET_MEMBER uses for that key (constants are one per
     text, so this adds none). `string_constant` keeps its signature:
     `on_shape` passes it a MEMBER event built from the text and
     `match->start`.
   - A **flag** or a **count** is stored as the integer.
   - A **number** becomes the index of a number constant (`number_index`,
     split out of `number_constant`; no shape has one yet).

`close_function_scope` sets `info` field by field and leaves the shape
alone.

### 6.3 Where the shape and its operand live

`src/internal.h`:

```c
typedef struct {
    uint32_t code;
    uint32_t captures;
    uint16_t params;
    uint16_t locals;
    uint16_t capture_count;
    uint8_t flags;          /* SJS_FUNCTION_* (was uint16_t; REST is its only flag) */
    uint8_t shape;          /* sjs_shape the matcher found it computes, or SJS_SHAPE_NONE */
    /* The shape's arguments, by sjs_shape_arg(shape, i): a text's string
     * constant (an index into the program's constants), a flag, a count. */
    uint32_t shape_args[SJS_SHAPE_ARGS];
} sjs_function_info;
```

This grows from 16 to 24 bytes per function. The key lives in the
program's constants, so it stays valid as long as any closure of the
function, which holds its program. A consumer reads
`SJS_AS_STRING(closure->program->constants[fn->shape_args[0]])`: interned,
`SJS_STRING_NAMED`, and pointer-equal to the key GET_MEMBER reads with.

### 6.4 Seeing which functions got which shape

The spec format gains `@shapes`, before a case's source, like `@global`. It
lists the shape of each function of the program, in function index order
(the order of the arrows' first tokens), the main program excluded. `-`
means no shape:

```
=== toSorted by a key, descending, and a pluck
@shapes COMPARE_BY_KEY(score, true) PLUCK(name)
[{name: "a", score: 1}, {name: "b", score: 3}].toSorted((a, b) => b.score - a.score).map(r => r.name)
--- value
["b","a"]
```

`spec_test` reads `program->functions[i].shape` and `shape_args` through
`internal.h`, as `names_test` reads `sjs_names`. It prints text arguments as
the constant's text and flags as `true` or `false`, with `sjs_shape_name`.
A case without `@shapes` does not check shapes. `diff-node.mjs` skips
`@shapes` lines. The runs pushed whole, a byte at a time and in chunks
must tag the same functions with the same shapes.

### 6.5 Cost

Measured on machine `172a3dd3bd74` (4-core cloud VM), against the [nodes]
commit:

- **Instructions** (callgrind, a program compiling one source 200 times,
  shapes on against `no_shapes`, `-O2`): `bench/modules/power.js` +4.7%;
  a guard written as an arrow, `(pending, _event) => pending.has(...) &&
  ...`, +7.4%, almost all of it folding the arrow's parameters into the
  four comparator rules that stay alive until its body; a guard with no
  arrow nothing measurable: every event is in the program's dead region,
  and nothing is allocated.
- **`mise run bench-compare 2476e5f compile --quick`**: compile throughput
  2% to 6% lower, most rows within the noise; one allocation more per
  module compile (the records; `campaign.js` two, its text outgrowing the
  matcher's 64 bytes) and none per guard; peaks up by 152 bytes for a guard
  (the matcher in the compiler) and by about 1.2 KiB for a module (the
  first 1 KiB of records).

The first version, which kept a cell for every node and compacted text at
every fold, cost 15% to 20% of compile throughput. What brought it down:
dead regions (5.3), which make most events a comparison or two; leaving
folded text in place; walking only the alive rules' bits; a mask of what a
cell derives; and allocating nothing until a bracket a rule can match
opens, with 1 KiB of records first and the first 64 bytes of text in the
matcher. What is left is per child of an arrow whose rules are alive, per
alive rule. If a profile shows it, the next step is to share the rules'
common prefixes (a trie of child sequences per open kind, so that the four
comparators fold their five common children once), before precomputed BURS
states (section 10).

---

## 7. The first consumers

Both fast paths sit in `src/builtins.c`, in the loop built-ins' steps.

### 7.1 Common rules

- **Set up once per loop.** In `sjs_loop_start` (`shape_start`), if the
  callback is a closure whose function has the shape this built-in uses
  (COMPARE_BY_KEY for TO_SORTED, PLUCK for MAP), the frame records it.
  `vm_frame` gains `uint8_t shape`, `uint8_t shape_flag` (descending, or
  "the key is on Object.prototype" for PLUCK), `const sjs_string
  *shape_key` and `uint32_t shape_guess` (where the last object had the
  key). **Changed from the first draft:** none of them grows the frame.
  `shape` and `shape_flag` sit in the padding after `entered`,
  `shape_guess` in the padding at the end, and `shape_key` shares its
  storage with `closure` (an anonymous union), which only code frames use.
  A frame stays 112 bytes, so a vm's frames (`max_frames` of them, 256 by
  default) take no more memory; a 120-byte frame would have added 2 KiB
  to every vm.
- **Enter the callback exactly where the bytecode would.** `enter_callback`
  becomes `sjs_vm_enter_callback(vm, loop)` in `vm.h`, and
  `sjs_vm_call_back` calls it as before. Before the first fast step, the
  fast path calls it too, at the same step the bytecode path would have made
  its first call. So a closure already running, a full frame stack, or no
  room for the callback's slots faults at the same place, with the same
  error, on both paths. Once it is entered, the run loop's `clear_callback`
  undefines the slots each step, as before; they are already undefined.
- **Stack room.** The callback's body pushes values above its slots:
  COMPARE_BY_KEY's two (`a.k`, then `b.k`), PLUCK's one. When the bytecode
  would fault on `max_stack` there, the fast path must not succeed. So a
  fast step requires `vm->stack_capacity - vm->sp >= need`, with `need`
  (2 and 1) a constant beside each fast path, at least what today's
  bytecode pushes. It may be more than the bytecode needs (a fused
  instruction would push less), but never less: too much only falls back.
  `api_test` pins it (section 9, [consumers]).
- **One element or one comparison per step.** A fast step does its unit of
  work and returns the new `LOOP_AGAIN` ("the loop is still on top; step it
  again"). `sjs_vm_run` needs no change for it: it finishes a loop only on
  LOOP_DONE. So the evaluator still pauses within a sort or a map on any
  amount of fuel, and a step's work stays constant. A step that takes the
  bytecode's answer for one element goes on to the next element as before,
  which may then be a fast one.
- **Fallback is the bytecode, unchanged.** When a precondition fails, the
  step calls back exactly as today (`call_back`). The bytecode then gives
  the value, or faults with its own message, position and subject.
- **Fuel.** A fast step charges what it looked up, one unit per lookup,
  which docs/builtins.md asks for. With the run loop's unit for the step,
  that is less than the bytecode it replaces. The figures below are today's
  bytecode, and `api_test` pins both sides exactly. The lookups are charged
  with the step's unit, as a built-in's cost is with its call's, so a run
  on one unit of fuel at a time can use three in one fast comparison: such
  a run pauses once per fast step, where the bytecode paused once per
  instruction.

### 7.2 `toSorted` with COMPARE_BY_KEY

In `sort_step`'s phase 2, where it is about to `call_back(x, y)`. Neither
`x` nor `y` is undefined there, because undefined elements are placed
without a call, as before. The fast comparison applies when all of these
hold:

1. `frame->shape == SJS_SHAPE_COMPARE_BY_KEY` (from `sjs_loop_start`).
2. The callback is entered (7.1).
3. The stack has room for 2.
4. `x.kind == SJS_OBJECT` and `y.kind == SJS_OBJECT`. Array elements never
   carry an internal tag.
5. Both have an own property `key`. It is found as GET_MEMBER finds it:
   first a guess kept in the frame, then the index or a scan, comparing key
   pointers first and `sjs_str_equal` second. An own property is exactly
   what `sjs_property_get` returns for an object.
6. Both values are numbers.

Then the step computes `d = descending ? ky - kx : kx - ky`. This is C's
double subtraction with the same operands, in the same order, that
`apply_binary`'s `x - y` sees (left operand `a.k` for ascending, `b.k` for
descending; `a` is `x`, `b` is `y`). It takes the next element from the
right run iff `d > 0`, through the helper that phase 1 now also uses after
`sjs_vm_to_number`, so the two cannot drift apart. It charges 2 and returns
LOOP_AGAIN.

Why the result is identical:

- **Value.** `d` is bit-for-bit the double the bytecode returns, and
  `sjs_vm_to_number` of a number is that number. So the decision, and with
  it the order, is the same. That covers NaN (`NaN > 0` is false: keep
  left), `Infinity - Infinity`, `-0 - 0` (-0, not > 0) and equal keys
  (0: keep left), which is why the sort stays stable.
- **Faults.** Every case where the bytecode faults falls back, so the
  bytecode faults with its own error:
  - a missing key (`undefined - 1` faults, "arithmetic in staticjs is on
    numbers");
  - a key found on Object.prototype ("staticjs objects have no prototype");
  - non-number values, strings included, where JavaScript would give NaN
    and staticjs faults;
  - null, numbers, booleans.
- **Non-objects that work.** Arrays and strings with `.length`, and sets
  with `.size`, fall back too and give the bytecode's value. Only own
  object properties take the fast path.
- **Mixed arrays** take the fast path for some pairs and the bytecode for
  others. Each comparison is decided alone, and the comparator is pure.
- **Fuel.** Today's bytecode costs 7 per comparison: the step plus 6
  instructions (TAKE_LOCAL, GET_MEMBER, TAKE_LOCAL, GET_MEMBER, BINARY,
  RETURN). A fast comparison costs 3: the step plus 2 lookups. A comparison
  that falls back costs what it did. Merging without a comparison stays
  free within a step, as before.
- **Receiver reuse** (`sjs_value_unique` of the receiver at the end) is
  unaffected. The callback's slots hold elements, not the receiver, and are
  cleared each step on both paths.

### 7.3 `map` with PLUCK

In `sjs_loop_step`, where MAP is about to `call_element(i)`. The fast
element applies when all of these hold:

1. `frame->shape == SJS_SHAPE_PLUCK`.
2. The callback is entered.
3. The stack has room for 1.
4. `x = a->items[i]` has `x.kind == SJS_OBJECT`.
5. Either `x` has an own property `key`, whose value is taken with
   `sjs_ref`, or it has none and the key is not on Object.prototype
   (`shape_flag`, computed once from `sjs_name_of(key)` and
   `SJS_NAME_OBJECT`), so the value is undefined.

The value goes to `out->items[out->length++]` (the output is sized when the
loop starts, as now), `frame->index++`, the step charges 1 and returns
LOOP_AGAIN. Otherwise it calls back as today.

- **Value:** the property `sjs_property_get` returns for an object, with
  the same reference count the bytecode leaves (the value referenced once
  more, the element untouched).
- **Faults:** null, undefined, numbers, booleans, a missing key on
  Object.prototype. All fall back and fault as the bytecode does. Arrays,
  strings and sets fall back and give their `length` or `size`.
- **Fuel:** today's bytecode costs 4 per element (the step, TAKE_LOCAL,
  GET_MEMBER, RETURN), the fast path 2.

`call_back`'s arguments (element, index, array) are not built on the fast
path. The callback never reads the index or the array, and nothing else can
observe them.

### 7.3A `Object.groupBy` with PLUCK

`Object.groupBy(items, x => x.k)` is a second consumer of PLUCK, with no new
rule: the callback's shape is the same, and the call's extra argument (the
index) is one a one-parameter arrow never reads. The preconditions are 7.3's
(the loop is entered, the stack has room for 1, the element is an object, the
key is its own property or absent and not on Object.prototype), with one
more: the plucked value must be a primitive that ToPropertyKey accepts. An
object, array or set key is not a precondition failure the C path may
decide; it calls the bytecode, which gives the value, and the grouping step
then faults on the non-primitive as it does for any callback. The C path
takes the same group step as the bytecode path (one helper), so the key's
text, the group's order and the faults cannot drift. Fuel: 2 per element
against 4.

### 7.3B `Array.from` with PLUCK

`Array.from(items, x => x.k)` is a third consumer of PLUCK, with no new rule.
The loop walks an array: when `items` is a set, a string or `{ length: n }`,
the built-in first makes the array of its elements (members in order, code
points, n undefined values) and the loop walks that copy, so 7.3's
preconditions hold unchanged (loop entered, room for 1, element an object,
own key or absent and not on Object.prototype). Elements that are not
objects (every element of a string, every `undefined` of `{ length: n }`)
fall back to the bytecode, which faults as `x.k` does. The output is sized
to the array when the loop starts, as map's is, and the step is map's. The
callback gets (element, index) and nothing else: a third parameter reads
undefined, not the array, because Array.from passes no array (as
Object.groupBy). Fuel: 2 per element against 4.

### 7.3C `Map.groupBy`, and an iterator's helpers

`Map.groupBy(items, x => x.k)` is a fourth consumer of PLUCK, with no new
rule: 7.3A's step, into a map instead of an object. The plucked value must
be a primitive group_key_ok accepts, as for Object.groupBy; a BigInt or a
`"__proto__"`, which a map keys as they are, falls back to the bytecode,
whose answer the same group step takes, so only speed differs. Fuel: 2 per
element against 4.

An iterator's `map`, `filter`, `reduce`, `some`, `every` and `find`
(`m.values().map(v => v.k)`) add no row: the loop's start makes the array
of what the iterator yields and runs the array's MAP, FILTER, REDUCE,
SOME, EVERY or FIND loop over it (`iterated`), so their rows apply as they
are, PLUCK, KEY_COMPARE, ELEMENT_COMPARE, SUM and SUM_KEY. Its `forEach`
and `flatMap` run no shape, as a map's forEach and the array's flatMap
run none. The callback gets (value, counter), and reduce's (accumulator,
value, counter), as JavaScript's helpers pass them; a C path never builds
them.

An array's `reduceRight` adds no row either. SUM and SUM_KEY tag its
callbacks as they tag reduce's, but `sum_step` adds from the first element
and reduceRight's bytecode from the last, and floating-point addition in
another order gives another sum (`[0.1, 0.2, 0.3]` gives 0.6 from the
right, 0.6000000000000001 from the left), so its callbacks always run
the bytecode.

### 7.4 Measuring

- The existing cases `eval.array.toSorted.rows` (`(a, b) => a.kwh - b.kwh`),
  `eval.array.pipeline` (`.map(r => r.kwh)`) and `eval.string.join`
  (`rows.map(r => r.id)`) take the fast paths.
- New: `eval.array.map.pluck` (`rows.map(r => r.kwh)`, L sizes) and
  `eval.array.toSorted.rows.desc` (`(a, b) => b.kwh - a.kwh`), with values
  checked against Node.
- `mise run bench-compare <the commit before>` goes in the commit body,
  with the machine hash. Claim only what it flags.

---

## 8. The off switch and the differential test

Both landed with [matcher] (section 9), so that the matcher arrived under
the test that guards it.

### 8.1 The switch

It is a field of the public compiler config, so it needs no global state
and no internal hook:

```c
typedef struct {
    sjs_context *context;
    sjs_goal goal;
    const sjs_str *globals;
    uint32_t global_count;
    sjs_modules modules;   /* optional: v may be NULL */
    /* Compile without matching shapes: ... false, which a designated
     * initialiser that leaves it out gives, matches. */
    bool no_shapes;
} sjs_compiler_config;
```

With `no_shapes`, `sjs_compiler_init` makes a matcher that matches the
rewrites alone (`rewrites_only`, section 12.6): the labeller never runs, so
no function is tagged, no record is allocated and no C path is taken, and
the anchored lane, which keeps its first cells in the matcher, costs a
comparison or two per event. A rewrite is semantics, so the switch leaves
it on: `no_shapes` turns off fast paths only. (It made no matcher at all
before rewrites.) `sjs_compile_params.compiler` carries the field, so `sjs_compile()`
honours it too. A positional initialiser of the five older fields still
means "on", but `-Wextra` warns about the missing field, so every caller in
the repository now names it (`api_test`, `spec_test`) or uses designated
initialisers (`bench.c`, which must also build against older headers for
`bench-compare`).

The matcher's records come from the context's allocator, like the
compiler's own memory. An embedder whose allocator refuses past a budget
can see a program that compiles with `no_shapes` within a few hundred
bytes of that budget (the records, at most
1 KiB at first) can fail with `SJS_ERR_NOMEM` with shapes on. That is the
one outcome matching can change, and only at compile time: what a compiled
program computes, its faults and its fuel follow THE RULE. The differential
test runs without a memory limit, so it does not see that boundary.

### 8.2 The differential test

- **`spec_test`**, fast pass. Every case runs as before (whole, bytes,
  chunks, with shapes on), and once more whole with `no_shapes`. The run
  without shapes must compile to the same program, but for its functions'
  tags (a hash of the code, numbers, constants' kinds, strings' bytes and
  numbers, functions, captures and lines), give the identical outcome text (the written value; or the
  fault's code, line:column, message and subject; or the compile error),
  tag no function, and use no less fuel than the run with shapes
  (`sjs_vm_fuel_used`). The three runs with shapes must tag the same
  functions with the same shapes. The bytes and chunks runs pause on every
  unit of fuel and every 13, so once the fast paths land they resume fast
  loops mid-sort and mid-map.
- **`spec_test --full`** fails every allocation with shapes on, which
  covers the matcher's and the compiler's recording.
- **`api_test`**:
  - `no_shapes` saves exactly the matcher's one allocation (its records)
    on a program with shapes, gives the same value, and a designated
    initialiser that leaves the field out matches;
  - a program with shapes fails each allocation in turn, in the fast pass;
  - `test_fuel_pinned`'s program has no shape and keeps its 194.
- **`bench --smoke`**: unchanged. It checks values, and those do not move.

---

## 9. The commits

Three commits, in this order, each passing `mise run test`. The first plan
had a fourth, [differential], with the off switch and the both-ways spec
run, to land before the fast paths; [matcher] carries both instead, so the
matcher itself arrived under the test that guards it. This document landed
with [nodes] and is updated as each later commit lands.

### 1. [nodes] `feat(staticjs): give each syntax event its node's id and start`

Changes: `include/staticjs.h` (`sjs_event.node`, `.start`, documented as in
1.1), `src/parser.c` (1.3), `README.md` ("Push and stream": what the two
fields are for), `docs/matcher.md`. The compiler is unchanged.

Tests:

- `api_test`: `log_event` logs `node` and `start` in a mode of its own
  (`KIND#node@offset`, or `@line.column`), so the existing event tests keep
  their expected text, and a new `test_event_nodes` checks exact streams
  for one source per row of the table in 1.2. Among them:
  - `a.b + c(d)[e]` (postfix starts at their first operand);
  - `x => x.k` and `(a, b = c => 1) => { return a; }` (an arrow's id across
    PARAMS_END, its body and a nested arrow in a default);
  - `` `s${"x"}t` `` (pieces against a string literal);
  - an else-if chain (one id for every IF, ELSE and IF_END);
  - `const [a = b] = c` (held, rotated events keep their ids);
  - an import with three specifiers;
  - END with node 0.
- `spec_test`: every case's stream also passes through a checking tee, test
  code with its own small table of bracket pairs, which checks:
  - `start.offset <= at.offset` on every event;
  - each bracket's BEGIN, separators and END share one id that no other
    node uses;
  - ids are unique across nodes and never 0 but on END;
  - an operator's markers and its last event share an id.

  It runs in all three push modes, and the three must send the same kinds,
  ids and starts (a hash of them), so chunking cannot change ids.
- `bench-compare` for `compile.*` and the parser-only rows, in the body.

### 2. [matcher] `feat(staticjs): match callback shapes in the event stream and record them on functions`

Changes:

- `src/shapes.rules`, and the generated `src/shapes.h` and
  `src/shapes_table.h`;
- `mise/shapes.rb`, `mise/tasks/shapes`, `mise/tasks/shapes-test` with its
  fixtures in `mise/shapes-test/`, and the `test` task running
  `shapes --check` and `shapes-test`;
- `src/matcher.h` and `src/matcher.c`;
- the compiler wiring and recording (section 6);
- `sjs_function_info.shape` and `shape_args` (with `flags` shrunk to
  `uint8_t`);
- `sjs_compiler_config.no_shapes` and the differential run (section 8),
  which the first plan held for a commit of their own, [differential];
- the `@shapes` spec directive, in `spec_test.c` and `diff-node.mjs`;
- `README.md`, `AGENTS.md`, `src/README.md` and this document.

Nothing reads a shape yet, so no value and no fuel can change.

Tests:

- `mise run shapes-test` (4.6), and `mise run shapes --check` in every
  `mise run test`.
- `tests/spec/shapes.spec` with `@shapes`, 43 cases, each with its value or
  fault, which `diff-node` checks against Node:
  - **Positives:** `x => x.k`, `(x) => x.k`, `x => (x.k)`,
    `x => { return x.k; }` with and without the semicolon, a body with
    empty statements (which send no events), a key that is also a method
    name (`length`), a missing key, a fault on null, a pluck never called,
    a key of 64 bytes; the four comparators, and `(b, a) => a.k - b.k`
    (descending: positions decide, not names), a comparator that faults on
    strings; nested functions (an outer function untagged and an inner one
    tagged, and a tagged one before an untagged one), a shaped arrow in a
    parameter default, one in a held `const` pattern's default, and two in
    an untagged comparator.
  - **Negatives:** `x => y.k` and a comparator reading a captured name;
    `(x = 1) => x.k`, `({k}) => k`, `(...x) => x.length`, `(x, i) => x.k`,
    `(a, b, c) => a.k - b.k`; `x => x?.k`, `x => x["k"]`, `x => x.k.j`,
    `x => -x.k`; `(a, b) => a.k - b.j`, `(a, b) => a.k - a.k`,
    `(a, b) => b.k - b.k`, `(a, b) => a.k + b.k`;
    `x => { const y = x; return y.k; }`;
    `x => { { const x = {k: 5}; return x.k; } }` (shadowing in a block);
    `x => { if (x) return x.k; }`; a statement after the return; a
    function returning a function; a key of 65 bytes, longer than the
    matcher captures. A `"use strict"` directive before the return is not
    among them: staticjs refuses that body ("an expression statement has
    no effect in staticjs").
- `tests/matcher_test.c`, a new internal test program in
  `StaticJS::TESTS`. It includes `src/matcher.h` and drives a parser into a
  matcher through a tee of its own:
  - **Reports:** for a list of sources, the exact reports (shape, node,
    start, arguments), pushed whole and a byte at a time, and the compiler
    tags as many functions as were reported.
  - **Never stops** on any spec source (1,093), nor on the deepest programs
    the limits allow, for 18 kinds of nesting, and the records stay within
    4 × max_nesting + 4.
  - **Width:** an array of 100,000 members, a module of 2,000 consts, each
    a pluck, and a template of 1,000 substitutions, each hold at most 2
    records.
  - **Unreadable streams:** node 0, an unknown kind, a stray close, a close
    of another kind, too few operands, a stray separator, and more records
    than the bound make it stop, free what it holds, and keep returning
    SJS_OK.
  - **Out of memory:** each of the matcher's allocations (its records, and
    its text past the bytes it has in itself) fails in turn; each run ends
    with SJS_ERR_NOMEM, and nothing leaks.
- `spec_test` with and without shapes (8.2); `spec_test --full` covers the
  compiler with the matcher. `mise run asan` runs all of it under the
  sanitizers.
- `bench-compare` for `compile.*`, in the body (6.5).

### 3. [differential]

Merged into [matcher] (section 8).

### 4. [consumers] `perf(staticjs): sort by a key and pluck a key in C when the callback's shape allows`

Landed as planned, but for the frame layout (7.1). What the tests pin:
5 comparisons of `[{k:3},{k:1},{k:2},{k:0}]` cost 54 units in bytecode and
34 in C; a map of 4 plucked elements 34 and 26. The `max_stack` edges are
where the bytecode cannot enter the callback, cannot push `a.k`, and
cannot push `b.k` (`x.k` for a pluck), and the C path faults at each of
them with the bytecode's own message and position. "A key that is an
array index" has no case of its own beyond `x => x[0]`, which is an INDEX,
not a MEMBER, so no rule matches it: a MEMBER's key is always a name.

Changes:

- `src/vm.h`: `vm_frame.shape`, `shape_flag`, `shape_key` (in a union
  with `closure`) and `shape_guess`; `LOOP_AGAIN`; `sjs_vm_enter_callback`.
- `src/vm.c`: `enter_callback` made shared.
- `src/builtins.c`: the two fast paths, and the merge decision helper.
- `bench/bench.c`: two cases.
- `docs/builtins.md`: a section "Shapes: a C path for a callback", with the
  rules of 7.1 and a checklist line.
- `README.md` ("What it guarantees": shapes change speed and fuel, never a
  value or a fault).

Tests:

- **Spec cases first**, in `tests/spec/shapes.spec`, each with `@shapes`,
  checked against Node by `diff-node`, and run both ways by the
  differential test:
  - **Sort:** ascending and descending; equal keys stay in order
    (stability), including keys 0 and -0 on objects told apart by another
    property; `Infinity` keys; a single NaN key.
  - **Sort faults:** a missing key; a string key; a key found on
    Object.prototype (`toString`); an own `toString` holding a number (works).
  - **Sort elements:** null and number elements (faults); undefined
    elements (placed last, never compared); arrays or strings by `length`
    (bytecode, works).
  - **Sort, mixed:** an array where some pairs are fast and one pair
    faults at its exact position; an empty array; one element.
  - **Map:** a present key; a missing key (undefined); a missing
    `toString` (fault); null, undefined and number elements (faults);
    strings, arrays and sets by `length` or `size`; values that are objects
    (shared, not copied); an empty array.
  - **Pause and resume:** the bytes and chunks modes resume both loops at
    every unit.
- **`api_test`:**
  - **Fuel, pinned:** `[{k:3},{k:1},{k:2},{k:0}].toSorted((a, b) => a.k -
    b.k)` and a map of 4 plucked elements, each with shapes on and with
    `no_shapes`. The figures are pinned exactly, on below off, and the
    pauses at 1 unit per run are counted.
  - **Limit edges, both ways, identical:** `max_stack` set so the
    comparator's second push is the one that does not fit (the bytecode
    faults on `max_stack`, and so must the shaped run), and one less tight
    (both succeed); the same for PLUCK's one push; `max_frames` exactly
    full when the loop's callback would be entered.
  - **Reentrancy** needs no case of its own, and none could be written: a
    shaped body calls nothing, so it is never running when a loop takes it.
    Both paths go through the same `sjs_vm_enter_callback` regardless, so
    the check would fire identically if it could.
- **`mise run test --full`** passes: the fast paths allocate nothing new,
  but the sweep runs every spec case through them. The body says it passed.
- **`bench-compare`** for `eval.array.toSorted.rows`, `.rows.desc`,
  `eval.array.map.pluck`, `eval.array.pipeline`, `eval.string.join`, and
  `eval.guard` (placement), quoted with the machine hash. The fuel column
  shows the drop.

---

## 10. Later

These come after the commits of section 9, in the order a profile asks for
them.

- **KEY_EQUALS** (3.3) for `filter`, `find`, `findIndex`, `some`, `every`:
  implemented in section 11 as KEY_COMPARE and ELEMENT_COMPARE.
- **The compiler's own peepholes as rules,** once a non-arrow node can
  carry a report: OBJECT_LITERAL's gathered prefix (an `OBJECT_BEGIN` hedge
  of static KEYs), and `call_begin`'s METHOD_MEMBER rewrite (a `CALL_BEGIN`
  whose callee is a MEMBER). That needs the compiler to accept a report for
  the node it is compiling, by id, which the [nodes] ids already allow.
- **Constant folding and superinstructions,** as further rule sets with
  costs, where competing rules choose.
- **Shared rule prefixes,** if the matcher shows in a compile profile
  (6.5): a trie of each open kind's child sequences, so that rules with a
  common prefix (the four comparators' five children) fold it once.
- **Precomputed BURS states,** only if the labeller still shows after that.
- **Rules on brackets that take the nodes before them** (a call, `&&`,
  `?:`, an if), which the generator refuses today (4.3): the matcher would
  have to keep the cells before such an open even in a dead region.
  Section 11.9.2 designs this for a bracket that can still match, where
  the cells are kept already.
- **Key caching in the sort** (each element's key read once). It is not in
  the first commit, because it would charge fuel for keys of elements that
  are never compared.

---

## 11. The second rule set: numeric sorts, predicates and sums

Status: item 1, the numeric sort, is implemented (11.6), item 2, the
predicates (11.7.3), and item 3, the sums (11.8.1). Items 1 to 3 are
one commit each (11.11);
item 4, set membership through a captured name, is a design only (11.9),
for the reasons given there.

### 11.1 Why

A comparison with QuickJS and Node `--jitless` (outside the repository)
found staticjs at about half of Ignition's speed on callback loops. Entering
and leaving a callback costs about 200 machine instructions per element,
against about 64 per bytecode instruction, and `toSorted((a, b) => a - b)`
is slower than QuickJS. A shape removes the callback from the loop: the
element is decided in C, and the callback's frame is never run. Section 7
did this for `map` with PLUCK and `toSorted` with COMPARE_BY_KEY. This
section adds the other callbacks that the benchmarks and `bench/modules`
use most:

| item | shape | matches | consumer | fuel per element or comparison: bytecode, C |
|---|---|---|---|---|
| 1 | COMPARE_NUMERIC(descending) | `(a, b) => a - b`, `(a, b) => b - a` | `toSorted` | 5, 1 |
| 2 | KEY_COMPARE(key, op, value) | `x => x.k OP c`, `x => c OP x.k` | `filter`, `find`, `findIndex`, `findLast`, `findLastIndex`, `some`, `every` | 6, 2 |
| 2 | ELEMENT_COMPARE(op, value) | `x => x OP c`, `x => c OP x` | the same | 5, 1 |
| 3 | SUM(element_first) | `(a, x) => a + x`, `(a, x) => x + a` | `reduce` | 5, 1 |
| 3 | SUM_KEY(key, element_first) | `(a, x) => a + x.k`, `(a, x) => x.k + a` | `reduce` | 6, 2 |
| 4 | SET_HAS, SET_HAS_KEY, SET_HAS_BOTH (11.9) | `x => s.has(x.k)`, `x => s.has(x.k) && !t.has(x.k)`, ... | the predicate loops | design only |

OP is `===`, `!==`, `<`, `<=`, `>` or `>=`, and `c` a literal: a number, a
string, `true`, `false` or `null`. Every shape also matches its block form,
`x => { return ...; }`, as PLUCK and COMPARE_BY_KEY do. The bytecode column
is the loop's step plus the callback's instructions (each one unit), so
`x => x.k === 2` is the step, TAKE_LOCAL, GET_MEMBER, NUMBER_, BINARY and
RETURN. The C column is the step plus one unit per property looked up
(7.1).

### 11.2 Two new types in the rule language

Item 1 and item 3 need nothing new. Item 2 needs two types, which land in
its commit.

- **`operator` as a shape parameter.** A shape records the operator of its
  comparison: `shape KEY_COMPARE(key: text, op: operator, value: operand)`.
  Nonterminal attributes can already be operators, and a head can already
  give an operator constant; only `PARAM_TYPES` lacks it. Its argument kind
  is the new `SJS_SHAPE_ARG_OPERATOR`, stored as the `sjs_operator`'s
  number, like a count. It has its own kind so that `spec_test` and
  `matcher_test` print it by name: `KEY_COMPARE(priority, STRICT_EQUAL, 2)`.
- **`operand`: a value fixed before the callback runs.** In this item, a
  literal: a number, a string, `true`, `false` or `null`. Item 4 would add
  a name read from outside the function (11.9), which is why the type is
  not called `literal`. Section 3.3 sketched it as one nonterminal over five
  leaf kinds. It is a field type, an attribute type and a parameter type:

  ```
  event NUMBER leaf value: number, operand: operand;
  event STRING leaf value: text, operand: operand;   # a template's pieces are the template's own events
  event TRUE leaf operand: operand;
  event FALSE leaf operand: operand;
  event NULL leaf operand: operand;
  ```

  The field reads the event as a value. NUMBER gives its number, STRING
  its text, and TRUE, FALSE and NULL only their kind. The generator allows
  an `operand` field only on a `leaf` event, and the matcher's field reader
  fails the test on any kind but these five, so a declaration on another
  leaf matches nothing. An operand has no constant syntax: a test or a
  head that gives one is refused ("an operand comes from its pattern,
  never a constant"). An operand variable agrees only with operands. A
  template's STRING pieces are separators of their template (section 2),
  so `` `a${b}` `` gives no operand. A template without a substitution is
  a single STRING event, as the parser sends a string literal, so
  `` `a` `` is the operand `"a"`: the same value, compiled to the same
  CONSTANT (found when 11.7 was implemented; `shapes.spec` has it).

**In the matcher**, an operand is captured into the text stack, so every
path that keeps, moves and drops text keeps, moves and drops operands too,
unchanged:

- Its bytes are a kind byte (`SJS_OPERAND_NUMBER`, `_TEXT`, `_TRUE`,
  `_FALSE`, `_NULL`, in `src/internal.h`), then the payload: the double's 8
  bytes for a number, the string's bytes for a text, nothing for the
  others. The slot is a text slot (`offset`, `length` with the kind byte).
- `matcher_field` builds the bytes in the step (`matcher_step.operand[1 +
  SHAPE_TEXT_MAX]`), with a third sentinel offset, `SHAPE_TEXT_OPERAND`,
  beside `SHAPE_TEXT_EVENT`. `matcher_text` and both settle paths read the
  step's bytes for that sentinel, as they read the event's text for the
  other. One event can need both, when one rule tests a STRING's `value`
  and another its `operand`.
- `matcher_text_slots` counts `SHAPE_FIELD_OPERAND` slots as text.
  `matcher_use` compares two operands by their bytes. For numbers, equal
  bits is `==`, because a NUMBER event's value is never NaN or -0: `NaN` is
  a NAME, and `-0` is a UNARY over 0. A string longer than `SHAPE_TEXT_MAX`
  fails its test, as a long key does, and only loses the fast path.
- The report's `sjs_shape_value` gains `uint8_t operand` (the kind). It
  fills `number` (copied out of the bytes with `sjs_copy`) or `text`.

**In the compiler**, `on_shape` stores an operand in one `uint32_t`
`shape_args` entry: `kind | index << 4`, through
`sjs_shape_operand(kind, index)`, `sjs_shape_operand_kind(arg)` and
`sjs_shape_operand_index(arg)` in `src/internal.h`. Four bits leave room
for item 4's kinds. A number's index is `number_index`'s and a text's is
`string_constant`'s. Both find the constant the literal's own NUMBER_ or
CONSTANT instruction already made, because the body is compiled before its
ARROW_END reaches the matcher, so no constant is added, and the
differential test's program hash checks that. True, false and null have
index 0. An index of 2^28 or more is not recorded: `on_shape` leaves the
function untagged.

With three parameters (KEY_COMPARE), `SJS_SHAPE_ARGS` becomes 3, and
`sjs_function_info` grows from 24 to 28 bytes. Item 4's SET_HAS_BOTH would
make it 4, and 32 bytes.

### 11.3 The rules

These are appended to `src/shapes.rules`, after COMPARE_BY_KEY, with the
five event declarations of 11.2 changed in place. The three items' rules
were run through the generator, with `number` standing in for `operand`
and `operator` added to its parameter types. They generate, and pass every
check of 4.3, the closure check included.

```
# ---- 1. Numbers in toSorted ------------------------------------------------

shape COMPARE_NUMERIC(descending: flag);               # (a, b) => a - b, or b - a

# (a, b) => a - b ascending; (a, b) => b - a descending. Both names are the parameters.
COMPARE_NUMERIC(false) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a},
                                                 PARAM{index: 1}, BIND{name: b}, PARAMS_END,
                                                 BINARY{op: SUBTRACT}(NAME{name: a}, NAME{name: b}));
COMPARE_NUMERIC(true) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a},
                                                PARAM{index: 1}, BIND{name: b}, PARAMS_END,
                                                BINARY{op: SUBTRACT}(NAME{name: b}, NAME{name: a}));
COMPARE_NUMERIC(false) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a},
                                                 PARAM{index: 1}, BIND{name: b}, PARAMS_END,
                                                 BLOCK_BEGIN, RETURN(BINARY{op: SUBTRACT}(NAME{name: a}, NAME{name: b})), BLOCK_END);
COMPARE_NUMERIC(true) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a},
                                                PARAM{index: 1}, BIND{name: b}, PARAMS_END,
                                                BLOCK_BEGIN, RETURN(BINARY{op: SUBTRACT}(NAME{name: b}, NAME{name: a})), BLOCK_END);

# ---- 2. Predicates against a literal ----------------------------------------

shape KEY_COMPARE(key: text, op: operator, value: operand);   # x => x.key OP c, or c OP x.key
shape ELEMENT_COMPARE(op: operator, value: operand);          # x => x OP c, or c OP x

nonterminal fixed(value: operand);       # a value fixed before the callback runs: here, a literal
nonterminal key_test(object: read, key: text, op: operator, value: operand);
nonterminal element_test(element: read, op: operator, value: operand);

fixed(c) <- NUMBER{operand: c};
fixed(c) <- STRING{operand: c};
fixed(c) <- TRUE{operand: c};
fixed(c) <- FALSE{operand: c};
fixed(c) <- NULL{operand: c};

# x.key OP c. One rule per operator: no other operator may match.
key_test(x, k, STRICT_EQUAL, c)     <- BINARY{op: STRICT_EQUAL}(key_of(x, k), fixed(c));
key_test(x, k, STRICT_NOT_EQUAL, c) <- BINARY{op: STRICT_NOT_EQUAL}(key_of(x, k), fixed(c));
key_test(x, k, LESS, c)             <- BINARY{op: LESS}(key_of(x, k), fixed(c));
key_test(x, k, LESS_EQUAL, c)       <- BINARY{op: LESS_EQUAL}(key_of(x, k), fixed(c));
key_test(x, k, GREATER, c)          <- BINARY{op: GREATER}(key_of(x, k), fixed(c));
key_test(x, k, GREATER_EQUAL, c)    <- BINARY{op: GREATER_EQUAL}(key_of(x, k), fixed(c));
# c OP x.key, recorded as x.key OP' c: the operator mirrored (11.7.2 says why that is exact).
key_test(x, k, STRICT_EQUAL, c)     <- BINARY{op: STRICT_EQUAL}(fixed(c), key_of(x, k));
key_test(x, k, STRICT_NOT_EQUAL, c) <- BINARY{op: STRICT_NOT_EQUAL}(fixed(c), key_of(x, k));
key_test(x, k, GREATER, c)          <- BINARY{op: LESS}(fixed(c), key_of(x, k));
key_test(x, k, GREATER_EQUAL, c)    <- BINARY{op: LESS_EQUAL}(fixed(c), key_of(x, k));
key_test(x, k, LESS, c)             <- BINARY{op: GREATER}(fixed(c), key_of(x, k));
key_test(x, k, LESS_EQUAL, c)       <- BINARY{op: GREATER_EQUAL}(fixed(c), key_of(x, k));

# x OP c, and c OP x mirrored.
element_test(x, STRICT_EQUAL, c)     <- BINARY{op: STRICT_EQUAL}(NAME{name: x}, fixed(c));
element_test(x, STRICT_NOT_EQUAL, c) <- BINARY{op: STRICT_NOT_EQUAL}(NAME{name: x}, fixed(c));
element_test(x, LESS, c)             <- BINARY{op: LESS}(NAME{name: x}, fixed(c));
element_test(x, LESS_EQUAL, c)       <- BINARY{op: LESS_EQUAL}(NAME{name: x}, fixed(c));
element_test(x, GREATER, c)          <- BINARY{op: GREATER}(NAME{name: x}, fixed(c));
element_test(x, GREATER_EQUAL, c)    <- BINARY{op: GREATER_EQUAL}(NAME{name: x}, fixed(c));
element_test(x, STRICT_EQUAL, c)     <- BINARY{op: STRICT_EQUAL}(fixed(c), NAME{name: x});
element_test(x, STRICT_NOT_EQUAL, c) <- BINARY{op: STRICT_NOT_EQUAL}(fixed(c), NAME{name: x});
element_test(x, GREATER, c)          <- BINARY{op: LESS}(fixed(c), NAME{name: x});
element_test(x, GREATER_EQUAL, c)    <- BINARY{op: LESS_EQUAL}(fixed(c), NAME{name: x});
element_test(x, LESS, c)             <- BINARY{op: GREATER}(fixed(c), NAME{name: x});
element_test(x, LESS_EQUAL, c)       <- BINARY{op: GREATER_EQUAL}(fixed(c), NAME{name: x});

# x is parameter 0, the only one, a plain name.
KEY_COMPARE(k, o, c) <- ARROW_BEGIN{params: 1}(PARAM{index: 0}, BIND{name: x}, PARAMS_END,
                                               key_test(x, k, o, c));
KEY_COMPARE(k, o, c) <- ARROW_BEGIN{params: 1}(PARAM{index: 0}, BIND{name: x}, PARAMS_END,
                                               BLOCK_BEGIN, RETURN(key_test(x, k, o, c)), BLOCK_END);
ELEMENT_COMPARE(o, c) <- ARROW_BEGIN{params: 1}(PARAM{index: 0}, BIND{name: x}, PARAMS_END,
                                                element_test(x, o, c));
ELEMENT_COMPARE(o, c) <- ARROW_BEGIN{params: 1}(PARAM{index: 0}, BIND{name: x}, PARAMS_END,
                                                BLOCK_BEGIN, RETURN(element_test(x, o, c)), BLOCK_END);

# ---- 3. Sums in reduce ---------------------------------------------------------

shape SUM(element_first: flag);                        # (a, x) => a + x, or x + a
shape SUM_KEY(key: text, element_first: flag);         # (a, x) => a + x.key, or x.key + a

SUM(false) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                     BINARY{op: ADD}(NAME{name: a}, NAME{name: x}));
SUM(true) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                    BINARY{op: ADD}(NAME{name: x}, NAME{name: a}));
SUM(false) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                     BLOCK_BEGIN, RETURN(BINARY{op: ADD}(NAME{name: a}, NAME{name: x})), BLOCK_END);
SUM(true) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                    BLOCK_BEGIN, RETURN(BINARY{op: ADD}(NAME{name: x}, NAME{name: a})), BLOCK_END);
SUM_KEY(k, false) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                            BINARY{op: ADD}(NAME{name: a}, key_of(x, k)));
SUM_KEY(k, true) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                           BINARY{op: ADD}(key_of(x, k), NAME{name: a}));
SUM_KEY(k, false) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                            BLOCK_BEGIN, RETURN(BINARY{op: ADD}(NAME{name: a}, key_of(x, k))), BLOCK_END);
SUM_KEY(k, true) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                           BLOCK_BEGIN, RETURN(BINARY{op: ADD}(key_of(x, k), NAME{name: a})), BLOCK_END);
```

Why these rules are sound, beyond 3.2 (exact hedges, no shadowing, no
hidden behaviour):

- **One rule per operator.** A pattern cannot test an operator against a
  set, and `BINARY{op: o}` would also match `x.k + 1`, `x.k - 1` and `"k"
  in x`, so each comparison operator has its own rule and the head gives
  the operator. That is 24 postfix rules on BINARY. They cost a test of
  `op` each, and only for a BINARY in a bracket that can still match
  (5.3). The rest of the program is a dead region.
- **Mirroring.** `c < x.k` is recorded as `x.k > c`, so a consumer handles
  one orientation. 11.7.2 shows that the answer and the faults are the
  same. Strict equality is symmetric and keeps its operator.
- **The literal side is never a name** in this item, so no BINARY can
  derive `key_test` or `element_test` both ways. Item 4 changes that
  (11.9.3).
- **Which names are which.** COMPARE_NUMERIC's and SUM's patterns tie each
  NAME to a BIND by position. `(a, b) => a - a`, `(a, b) => b - b` and
  `(a, x) => a + a` derive the BINARY's nonterminal, but the arrow rule's
  second binding (`b` or `x`) differs from the name read, so nothing
  matches. Duplicate parameters do not compile (3.2).
- **Order matters for sums.** SUM records which operand the element is,
  because the C path adds in the bytecode's order (11.8). Addition of two
  doubles is commutative, NaN payloads aside, but there is no reason to
  rely on that.

Each shape is reported for a closing ARROW_END, so the compiler's recording
(6.2) is unchanged, but for the operand and operator conversions of 11.2.

### 11.4 The generated tables, and what they cost the matcher

The draft rules, run through the generator (with the stand-in types):

| after | nonterminals | rules | ARROW_BEGIN bracket rules | `SHAPE_SLOTS` | `SHAPE_COSTS` | record (x86_64) |
|---|---|---|---|---|---|---|
| today | 9 | 12 | 6 | 16 | 9 | 184 bytes |
| item 1 | 12 | 18 | 10 | 24 | 12 | 248 bytes |
| item 2 | 19 | 53 | 14 | 40 | 19 | 392 bytes |
| item 3 | 27 | 67 | 22 | 59 | 27 | 560 bytes |

All of these are within the generator's limits (63 nonterminals, 32 bracket
rules per open kind). A record's stride grows for two reasons (4.5). A
cell has a slot for every attribute of every nonterminal, 59 after item 3.
A marker has a slot for every variable of every bracket rule of its kind:
the arrow's 22 rules hold 58, and 38 of them are the parameters' names,
which every rule binds again. Fewer records fit in the first 1 KiB (one at
560 bytes, against five today), so a compile with nested arrows that a
rule can match allocates more often. A cell copy (`made` in `matcher_node`)
moves more bytes. Every one-parameter arrow keeps 6 rules alive until its
body (PLUCK, KEY_COMPARE, ELEMENT_COMPARE, two forms each), where today it
keeps 2.

So each item's commit quotes `mise run bench-compare <the commit before it>
compile --quick`, and then the full run, for `compile.guard.*`,
`compile.module.*` and the parser-only rows. Item 3's commit also adds
`_Static_assert(MATCHER_FIRST_RECORDS >= 1, ...)` to `matcher.c`: past 1 KiB
per record, today's formula gives 0 and the first allocation would hold no
record.

**[slots], if a commit's table flags compile time.** The generator can
share slots in both kinds of record, and only the generator and the
matcher's slot indexing change:

- **Cells, by event kind.** A cell is made by one event kind: a leaf's, a
  postfix's, or a bracket's open. Two nonterminals that no single kind can
  derive can share slots. Each nonterminal's kinds are those its rules are
  rooted at, plus, through chain rules, those of the nonterminals it is
  chained from. A shared slot is sound because a cell writes and reads only
  the slots of nonterminals it derives (`matcher_accept`,
  `matcher_text_slots`).
- **Markers, by binding site.** A variable's first occurrence is at a child
  position, as an attribute of the nonterminal derived there (or a field
  of the separator there). Two rules that bind at the same site bind the
  same value, because a cell keeps one derivation per nonterminal. So each
  site gets one slot, and each rule gets a table from its variables to
  slots, `shape_variable_slot[first_variable + v]`, in place of
  `first_slot + v`, which `matcher_tests`, `matcher_links`, `matcher_accept`
  and `matcher_text_slots` read through. All parameter names then share 2
  slots.

Counted by hand for the three items, that is about 25 slots, not 59, and a
record of about 290 bytes. [slots] lands before the item whose commit shows
the regression, or before item 4 regardless, whose rules would take the
record past 800 bytes. Its tests are `shapes-test`'s table fixture, which
shows the sharing, and `matcher_test`'s reports, which must stay
identical.

### 11.5 Consumers: what all of them share

Section 7.1's rules hold for each consumer: the loop's frame records the
shape once, the callback is entered where the bytecode would first enter
it, every precondition is checked on every step, the stack must have room,
there is one unit of work per step (returning LOOP_AGAIN), and the lookups
are charged. The changes common to all three items:

- **`shape_start` takes a table** from built-in to the shapes it runs.
  MAP runs PLUCK; TO_SORTED runs COMPARE_BY_KEY and COMPARE_NUMERIC;
  FILTER, FIND, FIND_INDEX, FIND_LAST, FIND_LAST_INDEX, SOME and EVERY run KEY_COMPARE and
  ELEMENT_COMPARE; REDUCE runs SUM and SUM_KEY. `Object.groupBy` runs PLUCK, as MAP does (7.3A), and so do `Array.from` (7.3B) and `Map.groupBy` (7.3C), and an iterator's map, filter, reduce, some, every and find run as MAP, FILTER, REDUCE, SOME, EVERY and FIND. The table replaces the
  conditional chain, which three more cases would make unreadable.
  **Implemented with item 1** as `shape_runners`, rows of `{ builtin,
  shape }` that `shape_start` scans; a switch on the shape then reads its
  key (PLUCK, COMPARE_BY_KEY) and its flag. COMPARE_NUMERIC has no key, so
  its loop frame leaves `shape_key` (the code frame's `closure`) alone.
  `shape_flag` means, by shape: PLUCK and KEY_COMPARE, "the key is on
  Object.prototype"; COMPARE_BY_KEY and COMPARE_NUMERIC, descending; SUM
  and SUM_KEY, the element is the left operand.
- **The frame stays 112 bytes.** Measured with `offsetof` on x86_64 at
  23e85a1: bytes 64 to 66 are `entered`, `shape` and `shape_flag`, 67 is
  padding, and toSorted's fields, then `shape_guess`, run from 68 to 112.
  The new `uint8_t shape_op` (KEY_COMPARE's and ELEMENT_COMPARE's
  operator) takes byte 67.
  The operand, an `sjs_value shape_value` (16 bytes, 8-aligned), shares
  storage with toSorted's merge state. `shape_guess` moves to 68, and an
  anonymous union at 72 holds either `{ width, left, mid, right, i, j, k,
  scratch_count }` (32 bytes) or `shape_value`. `scratch` stays at 104,
  outside the union, because `sjs_loop_free` reads it on every loop.
  `scratch_count` is read only when `scratch` is set, which only toSorted
  does. The static assert keeps the 112.
- **The operand is borrowed.** `shape_start` makes `shape_value` from the
  function's argument: `sjs_num(program->numbers[i])`, the string
  constant `program->constants[i]`, or true, false or null, with no
  reference taken. The closure in LOOP_CALLBACK holds its program, and the
  program holds its constants, until the loop frame is gone. That is the
  same argument as for `shape_key`.
- **Shared answers.** The bytecode's answer and the C path's go through
  the same helpers, as `sort_take` does for the sort. `predicate_answer(vm,
  frame, truthy)` decides the element at `frame->index`: keep it (filter),
  or finish with true, false, the element, or its index (some, every,
  find, findLast, findIndex, findLastIndex), or go on. `sjs_loop_step`'s phase-1 switch
  calls it too, so the two paths cannot drift. It returns LOOP_DONE,
  LOOP_FAULT (filter's `array_add` failing), or LOOP_AGAIN for "go on",
  which phase 1 reads as "carry on in this step".
- **Where the C step runs.** The C step is in `sjs_loop_step`, after the
  end-of-array check and before `call_element`, as PLUCK's is. reduce's
  sits before its own `call_back`. A loop whose last element took the C
  path finishes on the next step, exactly as one whose last element took
  the bytecode: the bytecode's next step reads the answer and finds the
  end, so the counts of steps are equal. A predicate that answers early
  (some, every, the finds) is one step cheaper in C: the C step finishes
  in the step that decides, where the bytecode reads the answer in a step
  of its own.

### 11.6 Item 1: toSorted with COMPARE_NUMERIC

In `sort_step`'s phase 2, beside COMPARE_BY_KEY's branch, where `x` and `y`
are the pair (never undefined; those are placed without a comparison):

1. `frame->shape == SJS_SHAPE_COMPARE_NUMERIC`.
2. The callback is entered.
3. The stack has room for `COMPARE_STACK` (2): `a` and `b`.
4. `x.kind == SJS_NUMBER` and `y.kind == SJS_NUMBER`.

Then `sort_take(frame, descending ? y - x : x - y)`. That is the double
`apply_binary` returns for `a - b` (or `b - a`), with `a` as `x`, and it
goes through the helper the bytecode's answer goes through. The step
charges nothing beyond its own unit and returns LOOP_AGAIN. Otherwise it
calls back.

- **Value.** As in 7.2: the same double, so the same decision. A NaN
  difference (a NaN element, or `Infinity - Infinity`) is not `> 0`, so
  the left element is kept. That is how JavaScript reads a NaN comparator
  result: as +0. 0 and -0 compare equal and keep their order.
- **Faults.** Strings, null, booleans, objects and arrays fall back. The
  bytecode faults ("arithmetic in staticjs is on numbers") for the first
  pair that has one, in the merge's order, on both paths.
- **Fuel.** 5 per comparison in bytecode (the step, TAKE_LOCAL,
  TAKE_LOCAL, BINARY, RETURN), 1 in C.

**Implemented** as `sort_numbers` in `src/builtins.c`, beside
`sort_by_key`: one branch in `sort_step` enters the callback for either
comparator shape, then calls the one the frame recorded. The generated
tables are item 1's row of 11.4 (12 nonterminals, 18 rules, 24 slots, 12
costs). `api_test` pins the fuel: `[3, 1, 2, 0].toSorted((a, b) => a - b)`
costs 40 without shapes and 20 with them, 5 × 4 apart, and pauses 39 and
19 times on one unit per run; `max_stack` from 6 to 10 and `max_frames` 2
and 3 give the same faults at the same places both ways. Compiling
`power.js` and the synthetic module takes one more allocation and 1,024
more bytes at the peak (the matcher's second block of records, now that a
record is larger); the guards' compiles are unchanged. At 8 runs no
`compile.*` row is flagged, but every module row reads 3 to 5% slower,
within the runs' noise: [slots] (11.4) is the first thing to measure if
item 2's commit moves them further.

### 11.7 Item 2: the predicate loops

#### 11.7.1 Preconditions and the C step

In `predicate_step(vm, frame)`, for element `e` (`frame->index`, or
`length - 1 - index` for findLast and findLastIndex):

1. `frame->shape` is KEY_COMPARE or ELEMENT_COMPARE (set at the start).
   For `<`, `<=`, `>` and `>=` with an operand that is `true`, `false` or
   `null`, `shape_start` sets no shape. Every such comparison faults in
   staticjs ("staticjs compares two numbers or two strings"), so the
   bytecode runs the whole loop and faults at the first element.
2. The callback is entered (`sjs_vm_enter_callback`, on the first step).
3. The stack has room for `PREDICATE_STACK` (2): the bytecode pushes `x`
   (then `x.k` in its place) and `c`, in either order.
4. **The value `v`.** For ELEMENT_COMPARE, `v` is the element, whatever it
   is. For KEY_COMPARE, the element must be `SJS_OBJECT`. `v` is its own
   property's value (`own_entry`, as for PLUCK), or undefined when it has
   none and the key is not on Object.prototype (`shape_flag`). Any other
   element (null, undefined, a number, a boolean, an array, a string, a
   set, a function), and a missing key that is on Object.prototype, call
   the bytecode for that element.
5. **The answer**, `compare_operand(frame, v)`: true, false, or "the
   bytecode decides":
   - `===` and `!==`: if `v` is a primitive (`sjs_is_primitive`), `equal =
     sjs_strict_equal_primitive(v, c)`, the function `apply_binary` calls.
     Otherwise `equal` is false, as `apply_binary` gives for a primitive
     against an object. `c` is a literal, so it is always a primitive, and
     `===` never faults here. `!==` is `!equal`.
   - `<`, `<=`, `>`, `>=`: both numbers, compared as C doubles with the
     operator's own C operator; or both strings, by
     `sjs_utf16_compare(v, c)` against 0. Anything else is decided by the
     bytecode, which faults.
6. With the answer, `predicate_answer`; then `frame->index++` unless it
   finished. The step charges 1 lookup for KEY_COMPARE and none for
   ELEMENT_COMPARE, and returns LOOP_AGAIN, or what `predicate_answer`
   returned.

When the bytecode decides, the step calls `call_element` exactly as today.

#### 11.7.2 Why the result is identical

- **Value.** `v` is what TAKE_LOCAL (and GET_MEMBER) leave on the stack,
  and `c` is what NUMBER_, CONSTANT, TRUE_, FALSE_ or NULL_ push: the same
  number by its index, or the same interned string. The comparison is
  `apply_binary`'s own expression for the same operator and operand types.
  That covers NaN (every relational answer false, `NaN === NaN` false,
  `NaN !== c` true), `-0 === 0` (true, `==` on doubles), `-0 < 0` (false)
  and `-0 <= 0` (true), Infinity, strings compared by bytes for equality,
  and by UTF-16 code units for order. The last is not code-point order:
  `"\u{1F600}" < "\uFFFF"` holds, because U+D83D comes before U+FFFF.
- **Mirroring.** For `c OP x.k`, the bytecode computes `c OP v` and the C
  path `v OP' c`. For numbers, `c < v` and `v > c` are the same IEEE
  comparison, NaN included. For strings, `sjs_utf16_compare` (src/text.c)
  is antisymmetric: swapping its operands swaps every `-1` and `1` it can
  return, and keeps 0. So `cmp(c, v) < 0` holds exactly when `cmp(v, c) >
  0`. For mixed types both orientations fault, and the C path does not
  decide those; the bytecode runs in its own orientation and gives its own
  fault, position and subject.
- **Faults.** The C path never faults. Each fault the bytecode can raise
  for these bodies is a case the C path leaves to it: a property of a
  non-object (`x.k` on null, a number), a key on Object.prototype, and a
  relational comparison of mixed types. So the fault comes from the same
  instruction for the same element. Identity comparison of two objects
  cannot arise, because `c` is a literal.
- **Short-circuit.** some, every, find, findIndex, findLast and findLastIndex stop at the
  element where `predicate_answer` finishes. That is the element where the
  bytecode's phase 1 finishes, because both read the same truthiness of
  the same answer. Elements after it are never read, on either path, so a
  faulting element past the answer does not fault.
- **Filter's output** grows through the same `array_add` with the same
  reference taken, so with `spec_test --full` the same allocation fails at
  the same element. The C path itself allocates nothing.
- **Fuel.** Bytecode: KEY_COMPARE costs 6 per element (the step, TAKE_LOCAL,
  GET_MEMBER, a literal instruction, BINARY, RETURN), ELEMENT_COMPARE 5.
  C: 2 and 1. A string comparison costs no fuel per byte in either, as
  `apply_binary` charges none.

#### 11.7.3 Implemented

As designed above, with these details and findings:

- **Generator.** `operand` is a field, attribute and parameter type (class
  `:operand`, field `SHAPE_FIELD_OPERAND`), `operator` a parameter type.
  The checks of 11.2 are in: an operand field on an event that is not a
  leaf, an operand constant in a test or a head, and an operand variable
  compared with another type each fail at their line (`shapes-test`, whose
  small rules file now reads an operand beside a number of the same event
  and has a shape with an operator and an operand parameter). A message
  now says "an operand", not "a operand". When a shape has an operator
  parameter, `src/shapes.h` also gets `sjs_shape_operator_name`, for the
  tests' printing. The tables are 11.4's item 2 row: 19 nonterminals, 53
  rules, 40 slots, 19 costs; a record is 392 bytes, so the first 1 KiB
  holds 2.
- **Matcher.** `matcher_operand` reads the event into the step's
  `operand` bytes; the slot points at them with `SHAPE_TEXT_OPERAND`, and
  `matcher_settle_event` (now given the sentinel to copy) and
  `matcher_settle` copy them as they copy the event's text.
  `matcher_report_operand` fills `sjs_shape_value.operand`, `number` and
  `text`. `matcher_test`'s out of memory sweep runs a 64-byte string
  operand, which alone outgrows the matcher's 64 bytes (three allocations:
  the records, their growth, the text), and a short one (two).
- **Compiler.** `on_shape` records an operand as `sjs_shape_operand(kind,
  index)`; `shape_text` builds the event `string_constant` finds a key's
  or a string's constant by. The differential test's program hash shows
  no constant is added.
- **Evaluator.** `apply_binary`'s strict equality and order are now
  `sjs_vm_strict_equal` and `sjs_vm_order` (`src/vm.c`, declared in
  `src/vm.h`), which `predicate_compare` calls too: one comparison for
  both paths, `SJS_VM_UNDECIDED` where the bytecode faults.
- **Consumers.** `predicate_start` (from `shape_start`) leaves an order
  against true, false or null untagged; `predicate_step`,
  `predicate_compare` and `predicate_answer` in `src/builtins.c`, which
  `sjs_loop_step`'s phase 1 shares for every predicate loop. The frame
  stays 112 bytes, with `shape_op` at 67, `shape_guess` at 68, the union
  at 72 and `scratch` at 104.
- **Templates.** A template without a substitution is one STRING event, so
  `` x => x.k === `a` `` is KEY_COMPARE with the operand `"a"`; the same
  value and the same CONSTANT, so it is a positive case, not a near-miss.
  `1e999` is refused as unsupported, so the spec uses `1e21`.
- **Fuel.** As 11.7.2 says per element. A loop that answers early (some,
  every, the finds) saves one step more: the bytecode reads the answer that
  finishes it in a step of its own (`api_test`:
  `[1, 2, 3, 4].some(x => x === 3)` is 30 units without shapes and 17 with,
  3 × 4 + 1 apart).
- **Allocations.** One more record growth where a matched arrow holds a
  third record: `test_no_shapes` now counts two matcher allocations, and
  compiling `power.js` one more (116 to 117 allocations, peak 20,846 to
  21,142 bytes). A function's info is 28 bytes, from 24.

`bench-compare 0f7f013 --quick`, machine 172a3dd3bd74 (Xeon 2.10GHz, 4
cores, a noisy VM), medians of 4 alternating runs. The unflagged rows:

| case | n | before | after | change | fuel per op |
|---|---|---|---|---|---|
| eval.array.pipeline | 10,000 | 12.92 M elem/s | 20.02 M elem/s | +55.0% | 95,018 -> 55,018 |
| eval.array.findIndex | 1,000 | 21.99 M elem/s | 55.34 M elem/s | +151.7% | 6,007 -> 2,007 |

Flagged noisy but consistent with the fuel: eval.array.filter +132% to
+136% (60,007 -> 20,007 at 10,000), .every +165% to +176% (50,007 ->
10,007), .some +174% to +189%, .findIndex at 10,000 +155%,
.pipeline at 1,000 +68%. The other eval.array rows, eval.guard.* and
compile.* are within the noise; no compile row is flagged slower, so
[slots] (11.4) is still not needed. compile.* hold 8 to 1,120 more bytes
(the larger function info, and power.js's extra growth).

### 11.8 Item 3: reduce with SUM and SUM_KEY

In REDUCE's branch of `sjs_loop_step`, before it moves the accumulator into
the call. With `acc` the value in LOOP_OUTPUT and `e` the element at
`frame->index`, the C step applies when all of these hold:

1. `frame->shape` is SUM or SUM_KEY.
2. The callback is entered: on the first step that would call it, which is
   index 0 with an initial value, or 1 without one. A reduce without an
   initial value over an empty array faults in `sjs_loop_start`, before
   any of this, as today. Over one element, no step calls back, so nothing
   is entered on either path.
3. The stack has room for `SUM_STACK` (2): `a` and `x` (then `x.k`).
4. `acc.kind == SJS_NUMBER`.
5. SUM: `e.kind == SJS_NUMBER`. SUM_KEY: `e` is an object with an own
   property `key` whose value is a number. A missing key falls back
   whether or not the key is on Object.prototype: `acc + undefined` faults
   either way.

Then `acc` becomes `element_first ? v + acc : acc + v`, with C's double
addition and the operands in the bytecode's order (`add` in vm.c computes
`a.as.number + b.as.number`, left operand first). It is written into
LOOP_OUTPUT in place: a number holds no reference. Then `frame->index++`,
1 lookup is charged for SUM_KEY (none for SUM), and the step returns
LOOP_AGAIN. Otherwise the bytecode runs, for this element only.

- **Value.** The same doubles, added in the same order, element by
  element. Each step adds one element to the result of the step before,
  on either path, so the rounding is the bytecode's
  (`[0.1, 0.2, 0.3].reduce((a, x) => a + x, 0)` is
  0.6000000000000001 both ways). -0 survives where it does in the bytecode
  (`-0 + -0` is -0, `-0 + 0` is 0), and NaN and `Infinity + -Infinity`
  are C's.
- **Strings and mixed types** fall back per element. Once the accumulator
  is a string (an initial `""`, or a string element), every later step
  falls back, because `acc` is no longer a number. The bytecode then
  concatenates, or faults. `[1, 2, "3", 4]` gives `"334"`, with the first
  two additions in C. `[1, true]` faults at `true` with the bytecode's
  "staticjs adds two numbers, or a string and a primitive".
- **Fuel.** Bytecode: 5 per element for SUM (the step, TAKE_LOCAL,
  TAKE_LOCAL, BINARY, RETURN), 6 for SUM_KEY. C: 1 and 2.
- **The accumulator's reference.** The bytecode path moves `acc` out of
  LOOP_OUTPUT into the call. The C path leaves it there, as a number,
  which nothing can observe.

#### 11.8.1 Implemented

As designed above, with these details:

- **Rules and tables.** The eight rules of 11.3, as drafted. The tables
  are 11.4's item 3 row: 27 nonterminals, 67 rules, 59 slots, 27 costs. A
  record is 560 bytes on x86_64, so the first 1 KiB holds one, and
  `matcher.c` asserts that it holds at least one.
- **Consumer.** `sum_step` in `src/builtins.c`, called from REDUCE's
  branch of `sjs_loop_step` after it enters the callback (on the first
  step that would call it) and before it moves the accumulator out.
  `shape_start` reads SUM_KEY's key and flag as it reads COMPARE_BY_KEY's,
  and SUM's flag as COMPARE_NUMERIC's. The frame's fields are unchanged.
- **Faults at the right element.** Every fault of these bodies comes from
  the bytecode for the element that raises it, so a bad key before a null
  faults with "staticjs adds two numbers, or a string and a primitive",
  and a null first with "cannot read a property of null" (`shapes.spec`).
  A number element for SUM_KEY falls back too, and the bytecode faults
  with "staticjs reads properties of objects, arrays, strings, sets, maps and iterators".
- **Spec numbers.** `1e308` is refused as unsupported, like `1e999`, so
  the overflow case adds `Number.MAX_VALUE` to itself.
- **Fuel** (`api_test`): `[1, 2, 3, 4].reduce((a, x) => a + x, 0.5)` costs
  36 without shapes and 20 with them (4 × 4 apart; 35 and 19 pauses on one
  unit per run), and a SUM_KEY over four objects 44 and 28 (43 and 23
  pauses). `max_stack` from 5 to 9 and `max_frames` 2 and 3 give the same
  faults at the same places both ways, with and without an initial value.
- **Existing cases on the path:** `eval.array.reduce` and the reduce of
  `eval.array.pipeline` (SUM); new, `eval.array.reduce.key` (SUM_KEY).
  `[0, -0, 0].toSorted((a, b) => a + b)`, a near-miss of COMPARE_NUMERIC,
  is now tagged SUM, and toSorted runs its bytecode.

`bench-compare 6deba94 --quick`, machine 172a3dd3bd74 (Xeon 2.10GHz, 4
cores, a noisy VM), medians of 4 alternating runs. The one unflagged
eval row:

| case | n | before | after | change | fuel per op |
|---|---|---|---|---|---|
| eval.array.pipeline | 10,000 | 22.80 M elem/s | 31.79 M elem/s | +39.4% | 55,018 -> 35,018 |

Flagged noisy but consistent with the fuel: eval.array.reduce +306% to
+316% (50,008 -> 10,008 at 10,000), .reduce.key +185% to +270% (60,008 ->
20,008), .pipeline at 1,000 +45%. eval.guard.* are within the noise. No
compile row is flagged slower at 8 runs, but every compile.module row
reads 2 to 4% slower, and power.js, campaign.js and the synthetic module
take one more allocation and 1,024 to 2,048 more bytes at the peak (the
matcher's records, one per 1 KiB now); the guards' compiles are unchanged.
That is the drift 11.6 and 11.7.3 saw, now larger: [slots] (11.4) comes
next, before item 4.

### 11.9 Item 4, design only: captured names, set membership and conjunctions

`campaign.js`'s `unsent` is `batch.filter(n => pending.has(n.id) &&
!sent.has(n.id))`, and the `eval.guard.window` and `eval.set.has` benches
are `n => pending.has(n.id)` and `id => pending.has(id)`. A C path for them
needs three things the matcher lacks, so it is designed here and not
implemented with items 1 to 3.

#### 11.9.1 Why it is not one commit

1. **Rules on brackets that take the nodes before them.** `s.has(x)` is a
   CALL_BEGIN (`open 1`: the callee `s.has` is its first child), and `p &&
   q` is a LOGICAL (`open 1`). The generator refuses rules on both (4.3),
   because the matcher keeps no cells in a dead region (5.3). This changes
   one of the matcher's invariants, with its own generator check, and the
   bound and width tests of `matcher_test`. It should land and be reviewed
   alone.
2. **A free name as an operand**, resolved by the compiler, which needs
   `read_name` split into a lookup and an emit (11.9.3).
3. **The consumers**: three shapes, with set lookups and their fuel, and
   `SJS_SHAPE_ARGS` at 4.

Each of these is about as large as one of items 1 to 3, and the second and
third build on the first. So item 4 is three commits ([brackets],
[names], [members]), after [slots] (11.4).

#### 11.9.2 [brackets]: rules on `open N` events

Status: implemented with section 13's pushes, as designed here, but for
the marker: it is built aside, the cell folded into it, and written in the
cell's place only if a rule is still alive, so that a call no rule wants
dooms the bracket around it at once, as before.

- **Generator.** Drop the refusal of 4.3. Keep `N <= 1` as a check (every
  `open N` kind has N = 1 today), so the matcher moves at most one record.
  In a pattern, the N first children of the bracket are its first N
  patterns, as CALL_BEGIN's declaration already says ("the callee is its
  first child").
- **Matcher.** In `matcher_open`, when the innermost bracket can still
  match and the event has rules and N = 1, the last cell (its callee, or
  its left operand) is the bracket's first child. The matcher copies it
  aside, writes the new marker at its index, with the cell's `text_base`
  (its text stays where it is and is now the marker's), and folds the copy
  into the marker as child 0. A marker no rule survives dooms the bracket
  around it, as today. In a dead region nothing changes: the cell was
  never kept, and a bracket there can be no rule's child.
- **Bound.** The open replaces one cell by one marker, so 5.6's bound
  holds.
- **Tests.** `matcher_test`'s never-stops sweep and bound checks for every
  nesting kind, with calls and logical operators that rules look at;
  `shapes-test`'s fixture with a rule on an `open 1` kind; and the out of
  memory sweep.

#### 11.9.3 [names]: a name from outside as an operand

- **Rules.** `event NAME leaf name: read, operand: operand;`, and the
  operand kind `SJS_OPERAND_NAME` (the name's text). An operand is not a
  `read`, so the closure check (4.3) is unchanged: a rule may read a name
  as an operand without binding it. The compiler then decides what the
  name is.
- **Compiler.** `on_shape` resolves a NAME operand as `read_name` would,
  through a lookup split out of `read_name`. The lookup emits nothing and
  notes no read, and `read_name` calls it, so the two cannot disagree.
  - a capture of the closing function becomes `SJS_OPERAND_CAPTURE`, its
    index in the function's captures. The body read the name, so the
    capture is already in `pending_captures`.
  - a B_CONSTANT binding (an import) becomes `SJS_OPERAND_CONSTANT`.
  - a global becomes `SJS_OPERAND_GLOBAL`.
  - `undefined` becomes `SJS_OPERAND_UNDEFINED`, and `NaN` and `Infinity`
    become `SJS_OPERAND_NUMBER`, with the index `read_name` made.
  - A local of the closing function (its parameter: `x => x.has(x)` binds
    the set's name to `x`), a namespace or a built-in function: the report
    is ignored and the function stays untagged.
- **Loop start.** `shape_start` reads the operand once:
  `closure->captures[i]`, `program->constants[i]`, `vm->globals[i]`, or
  undefined. Every binding a closure captures is const (a `let` is never
  captured), a value is never changed once shared
  (`docs/mutation.md`), and globals are fixed for an evaluation
  (`sjs_run_params`), so the value cannot change while the loop runs. It is borrowed, from the closure in LOOP_CALLBACK, its program, or
  the embedder.
- **KEY_COMPARE and ELEMENT_COMPARE with a name.** The rule `fixed(c) <-
  NAME{operand: c};` gives them `x.k === limit` and `x.k !== undefined`.
  The operand can now be an object, so `compare_operand` decides `===`
  only when `v` or `c` is a primitive. With two objects the bytecode
  faults ("=== on two objects compares identity"). The relational
  operators already leave mixed types to the bytecode. A relational
  operator with an operand of the wrong kind falls back per element, since
  `shape_start` cannot refuse it from the source alone.
- **The one ambiguity.** With `fixed` on NAME, `y === x` derives
  `element_test` twice: as `x := y, c := x` from the direct rule, and as
  `c := y, x := x` from the mirrored one. A cell keeps one derivation per
  nonterminal, and on a tie the first (5.3), so the mirrored one is lost
  and `x => y === x` would never match. The mirrored element rules
  therefore derive a nonterminal of their own, `element_test_mirrored`,
  with two ELEMENT_COMPARE arrow rules of their own. Key tests are not
  affected, because `key_of` is never a NAME.

#### 11.9.4 [members]: SET_HAS, SET_HAS_KEY and SET_HAS_BOTH

```
shape SET_HAS(set: operand, negated: flag);                   # x => s.has(x), x => !s.has(x)
shape SET_HAS_KEY(set: operand, key: text, negated: flag);    # x => s.has(x.k), x => !s.has(x.k)
shape SET_HAS_BOTH(first: operand, second: operand, key: text, negated: count);
                                                    # x => [!]s.has(x.k) && [!]t.has(x.k); bit 0 the first, bit 1 the second

nonterminal has_of(set: operand);                              # s.has, s a name
nonterminal member(element: read, set: operand, negated: flag);
nonterminal key_member(object: read, key: text, set: operand, negated: flag);
nonterminal both(object: read, key: text, first: operand, second: operand, negated: count);

has_of(s) <- MEMBER{key: "has"}(NAME{operand: s});
member(x, s, false) <- CALL_BEGIN(has_of(s), NAME{name: x}, ARGUMENT);
member(x, s, true) <- UNARY{op: NOT}(member(x, s, false));
key_member(x, k, s, false) <- CALL_BEGIN(has_of(s), key_of(x, k), ARGUMENT);
key_member(x, k, s, true) <- UNARY{op: NOT}(key_member(x, k, s, false));
both(x, k, s, t, 0) <- LOGICAL{op: AND}(key_member(x, k, s, false), key_member(x, k, t, false));
both(x, k, s, t, 1) <- LOGICAL{op: AND}(key_member(x, k, s, true), key_member(x, k, t, false));
both(x, k, s, t, 2) <- LOGICAL{op: AND}(key_member(x, k, s, false), key_member(x, k, t, true));
both(x, k, s, t, 3) <- LOGICAL{op: AND}(key_member(x, k, s, true), key_member(x, k, t, true));
# and the six arrow rules, each in both body forms, as KEY_COMPARE's
```

The events are `NAME s, MEMBER has, CALL_BEGIN, <argument>, ARGUMENT,
CALL_END` (an ARGUMENT follows each argument) and `<left>, LOGICAL,
<right>, LOGICAL_END`. One key on both sides is required by the shared
`k`. Different keys, `||`, and a third conjunct do not match.

The consumers are the six predicate loops. At loop start, each set operand
must be an `SJS_SET`; otherwise no shape is set. Per element, the value is
the element (SET_HAS), or the object's own property or undefined, as in
11.7.1 (one lookup, even for SET_HAS_BOTH, whose bytecode looks the key up
twice). The answer is `sjs_set_contains(set, v) != negated`, the leaf's own
call, which takes any value (an object is never a member). For
SET_HAS_BOTH the second set is consulted only when the first answer is
true, as `&&` does. Fuel: the step, the key lookup, and `lookup_cost(set)`
for each `has` made. That is what each CALL charges, so it is less than
the bytecode's ten-plus instructions. The stack room is the compiled
body's peak, pinned in `api_test`: CAPTURE, then METHOD_MEMBER's receiver
and method, then the argument.

The benches: `eval.set.has` and `eval.guard.window` (globals) take the
path. A new `eval.array.filter.unsent`, `batch.filter(n => pending.has(n.id)
&& !sent.has(n.id))` with global sets (L sizes), covers SET_HAS_BOTH.
`bench/modules/campaign.js` covers captures through `compile.*` and the
module's spec case.

### 11.10 Tests and benchmarks

Every case goes in `tests/spec/shapes.spec` with `@shapes`, its value or
fault checked against Node by `mise run diff-node`, and is run both ways by
the differential test (8.2). The bytes and chunks runs pause at every unit
and every 13, so they resume each C loop mid-way. `spec_test` prints the
new argument kinds: an operator by its `sjs_operator` name, an operand as
JavaScript (`2`, `"open"`, `true`, `null`).

**Item 1 (COMPARE_NUMERIC)**

- **Positives:** ascending and descending, in both body forms;
  `(b, a) => a - b` (descending: positions decide); `(a, b) => (a - b)`
  (parentheses send nothing).
- **Near-misses, each untagged:** `(a, b) => a - a`, `(a, b) => b - b`,
  `(a, b) => a + b`, `(a, b) => a - b - 0`, `(a, b) => -a - b`,
  `(a, b) => a.k - b`, `(a, b, c) => a - b`, `(a = 0, b) => a - b`,
  `(a, b) => a - c` (a capture), `a => a - 1`, and
  `(a, b) => { const d = a - b; return d; }`.
- **Edges:** NaN (a single NaN element, the twin of COMPARE_BY_KEY's
  single NaN key; if Node's order for an inconsistent comparator ever
  differs, keep it to two elements); `Infinity` and `-Infinity`, with a
  NaN from `Infinity - Infinity`; 0 and -0 in both orders (stable; the
  writer prints `-0`); equal numbers; undefined elements (last, never compared);
  a string pair (fault), a string among numbers (fault at the first such
  pair), null and booleans (fault); empty and one-element arrays.
- **Shape and consumer:** `[1, 2, 3].reduce((a, b) => a - b)` is tagged
  COMPARE_NUMERIC, and reduce runs its bytecode.

**Item 2 (KEY_COMPARE, ELEMENT_COMPARE)**

- **Positives:** each of the six operators in both orientations, for a
  key and for the element; each operand kind (an integer, `1.5`, `1e21`,
  `0x10`, `""`, a non-ASCII string, a string of 64 bytes, `true`,
  `false`, `null`); the block form. (`1e999` was refused as unsupported
  then, "this number needs more precision than staticjs reads", so Infinity
  was only the name `Infinity`, a near-miss until item 4; literals are read
  exactly since, and `1e999` is an Infinity literal.)
- **Near-misses:** `x => x.k === y` (a capture, until item 4);
  `x => x.k === undefined` and `x => x === NaN` (names, until item 4);
  `x => x.k === -1` (a UNARY); `x => x.k === "a" + "b"`;
  `` x => x.k === `a${1}` `` (a template with a substitution: `` `a` ``
  alone is a STRING, and matches); a string literal of 65 bytes;
  `x => x.k + 1` and `x => x.k - 1` (not comparisons); `x => "k" in x`;
  `x => x.k.j === 1`; `x => x?.k === 1`; `x => x["k"] === 1`;
  `(x, i) => x.k === 1`; `x => 1 === 2`; `x => x.k === x.j`;
  `x => x === x`; `x => !(x.k === 1)`; `x => (x.k === 1) === true`;
  `x => x.k === 1 && true`; `({k}) => k === 1`;
  `x => { const y = 1; return x.k === y; }`.
- **NaN:** `{k: NaN}` against `=== 1`, `!== 0`, `< 1`, `>= 0`; and NaN
  elements against `> 0`.
- **-0:** `{k: -0}` against `=== 0`; `[-0].find(x => x >= 0)` gives `-0`;
  `-0 < 0` and `-0 <= 0`.
- **undefined and missing keys:** `[{}, {k: 1}].filter(x => x.k === 1)`;
  `[{}].filter(x => x.k !== null)` (true); `[{}].some(x => x.k < 1)`
  (fault: undefined against a number); `[undefined, 1].filter(x => x === 1)`;
  a key on Object.prototype (`x.toString === 1`: fault); an own `toString`
  holding 1 (works).
- **Mixed types and strings against numbers:** `[1, "1", true, null,
  {}].filter(x => x === 1)` and `... x === "1"` (no fault); `["10",
  "9"].filter(x => x < "9")`; `[1, "a"].filter(x => x < 2)` (fault at
  element 1, where JavaScript would give false); `"\u{1F600}"` against
  `"\uFFFF"` in both orientations (UTF-16 order); `[1].filter(x => x <
  true)` (no C path; fault); a property holding an object or an array
  against `===` (false) and `<` (fault).
- **Non-object elements for a key:** null and numbers (faults); arrays and
  strings by `length`, sets by `size` (bytecode, works).
- **Objects with an index:** an object of 9 keys, past `SJS_INDEX_FROM`,
  so `own_entry` goes through `sjs_object_find`.
- **Empty arrays:** each of the six methods.
- **Short-circuit at the right element:**
  `[{k: 1}, {k: 2}, null].find(x => x.k === 2)` (no fault),
  `findIndex` (1), `[null, {k: 2}, {k: 3}].findLast(x => x.k === 2)`,
  `some` before a faulting element, `every` stopping at its first false.
  And faults at the right element, told apart by their messages:
  `[{k: 1}, {k: "a"}, null].some(x => x.k > 5)` faults with "staticjs
  compares two numbers or two strings", not with null's property fault;
  `[{k: 1}, null, {k: "a"}]` gives the other fault; findLast gives the
  same faults from the end.
- **Shape and consumer:** `xs.map(x => x.k > 1)` is tagged, and map runs
  its bytecode.

**Item 3 (SUM, SUM_KEY)**

- **Positives:** both orders, both forms, for each shape.
- **Near-misses:** `(a, x) => a + a`, `(a, x) => a - x` (that is
  COMPARE_NUMERIC), `(a, x) => a + x + 0`, `(a, x, i) => a + x`,
  `(a, x) => a + x.k.j`, `(a, x) => a + x[0]`, `(a, x) => a.k + x`.
- **Edges:** with and without an initial value; one element and no
  initial value (no call); an empty array with and without one (0, and
  the fault); `[0.1, 0.2, 0.3]` (rounding order); -0 with an initial -0
  and with 0; NaN; `Infinity` with `-Infinity`; an initial `""`, string
  elements in both orders (`["a", "b"].reduce((a, x) => x + a, "")` is
  `"ba"`); `[1, 2, "3", 4]` (`"334"`); `[1, true]`, `[1, null]` and
  `[1, undefined]` (faults at that element); `[1, "x", true]` (`"1xtrue"`);
  for SUM_KEY, a missing key (fault), a key on Object.prototype (fault),
  arrays by `length` (bytecode, works), string values (concatenation).

**`api_test`** (`test_shape_paths`), for each new shape:

- **Fuel, pinned both ways,** with the difference checked exactly.
  `[3, 1, 2, 0].toSorted((a, b) => a - b)` makes 5 comparisons, so shapes
  on is 5 × 4 = 20 units below off. A filter of 4 objects by `x.k > 2`:
  16. `[1, 2, 3, 4].some(x => x === 3)`: 3 elements, 13, not 12: the
  bytecode reads the answer that finishes the loop in a step of its own,
  where the C step finishes in the step that decides. The two reduces
  over 4 elements: 16 each. The pauses at 1 unit per run are counted, as
  [consumers] does (section 9).
- **Limit edges, identical both ways:** `max_stack` where the body's second
  push does not fit (C must fault as the bytecode does), and one larger
  (both succeed), for each of COMPARE_STACK, PREDICATE_STACK and
  SUM_STACK; `max_frames` exactly full when each loop would enter its
  callback.

**`matcher_test`:** exact reports for one source per new shape, with
operator and operand arguments; a string operand that outgrows the
matcher's 64 inline bytes of text, for the out of memory sweep; the
never-stops sweep and the record bound hold over the new spec sources.

**`shapes-test`:** the small rules file gains an `operand` field and an
`operator` parameter, and the table fixture shows them. New error
snippets: an `operand` field on a non-leaf event; an operand constant in a
test and in a head; an operand variable compared with a number.

**`bench/bench.c`.** Cases already on these paths: `eval.array.toSorted`
(COMPARE_NUMERIC ascending), `eval.array.filter` (`r.priority === 2`,
KEY_COMPARE), `eval.array.reduce` (`(a, x) => a + x`, SUM) and
`eval.array.pipeline` (KEY_COMPARE `>`, PLUCK, SUM). New cases, unit
element, L sizes, smoke size 10:

| case | source | smoke value | larger sizes |
|---|---|---|---|
| `eval.array.toSorted.desc` (item 1) | `xs.toSorted((a, b) => b - a)` | `[4.5,4,3.5,3,2.5,2,1.5,1,0.5,0]` | new CHECK_SORTED_DESC |
| `eval.array.every` (item 2, ELEMENT_COMPARE) | `xs.every(x => x >= 0)` | `true` | new CHECK_SAME: the smoke value at every size |
| `eval.array.some` (item 2, KEY_COMPARE `<`) | `rows.some(r => r.kwh < 0)` | `false` | CHECK_SAME |
| `eval.array.findIndex` (item 2, a string `===`) | `rows.findIndex(r => r.id === "none")` | `-1` | CHECK_SAME |
| `eval.array.reduce.key` (item 3, SUM_KEY) | `rows.reduce((a, r) => a + r.kwh, 0)` | `166.5` (Node gives the same) | new CHECK_SUM_KWH: `row_kwh(i)` summed in order |

The predicate cases scan every element and find nothing, so they measure
the loop and not where an answer happens to be. Each commit lists its cases
in the header comment of `bench.c`, runs `bench --smoke`, and quotes `mise
run bench-compare <the commit before it>` for its cases, for
`eval.array.*`, for `eval.guard.*` (placement, docs/builtins.md 8.3), and
for `compile.*` (11.4), with the machine hash. The fuel column shows the
drop. Claim only what the table flags.

### 11.11 Commits

Each commit passes `mise run test` and `mise run test --full` (its body
says so), and touches `vm.c`, `builtins.c` or the matcher, so each quotes
`bench-compare` (11.10). Each updates `docs/builtins.md` section 9 (the
shapes and their consumers, the checklist line unchanged) and this
section's status.

0. **[doc]** `docs(staticjs): design the numeric sort, predicate and sum
   shapes`: this section, and the pointers to it from the status line,
   3.3 and 10.

1. **[numeric]** `perf(staticjs): sort numbers in C when the comparator is
   (a, b) => a - b or b - a`.
   - `src/shapes.rules`, `src/shapes.h` and `src/shapes_table.h`
     regenerated.
   - `src/builtins.c`: the `shape_start` table (11.5) and the comparison
     (11.6).
   - Tests as in 11.10, item 1; `eval.array.toSorted.desc`.
   - No generator, matcher or compiler change.

2. **[predicates]** `perf(staticjs): decide x.key OP literal and x OP
   literal in C in filter, find, findIndex, findLast, some and every`.
   - `mise/shapes.rb`: the `operand` and `operator` types, with
     `mise/shapes-test/` (11.2).
   - `src/matcher.c` and `src/matcher.h`: operand slots and reports.
   - `src/compiler.c`: `on_shape`'s operand and operator arguments.
   - `src/internal.h`: the operand kinds and their encoding.
   - `src/vm.h`: `shape_op`, and `shape_value` in its union (11.5).
   - `src/builtins.c`: `predicate_answer`, `compare_operand` and
     `predicate_step`.
   - `tests/spec_test.c` and `tests/matcher_test.c`: printing the new
     arguments.
   - Tests as in 11.10, item 2; three bench cases.
   - This is the largest commit. The generator and matcher change cannot
     land before the shapes that use it, because the repository keeps
     nothing that nothing uses.

3. **[sums]** `perf(staticjs): sum numbers and a key's numbers in C in
   reduce`.
   - Rules and headers; `src/builtins.c`, the reduce step (11.8).
   - `_Static_assert` on `MATCHER_FIRST_RECORDS` (11.4).
   - Tests as in 11.10, item 3; `eval.array.reduce.key`.

- **[slots]**, conditional: before whichever commit's `compile.*` rows
  regress, and in any case before item 4 (11.4).

4. **Design only (11.9):** [brackets], then [names], then [members], each
   its own commit with its own tests, after [slots]. The next session that
   takes item 4 starts from 11.9 and updates it with what the
   implementation finds.

---

## 12. Rewrites: matches that change the code

Status: implemented, with three rewrites. MUTATE_PATH: a method that
changes its receiver, called on a path from a variable (`o.items.push(x)`,
docs/mutation.md, "Methods through a path"). APPEND and APPEND_SUM:
`s += e` and `s = s + e1 + e2`, appends to a `let` whose read the compiler
makes a move (docs/mutation.md, "Appending to a let"; 12.8). The matcher's
rules decide the form; the compiler decides what needs scopes, and rewrites
what it compiled.

### 12.1 A rewrite, not a tag

A shape tags a function, and the code is the same either way (the first
property at the top): `no_shapes` can drop a tag, and only the fuel moves.
MUTATE_PATH is different in kind. Without it, `o.items.push(x)` is an
ordinary call that faults by name; with it, the call changes `o.items`. So
it changes what a program means, and the compiler emits other bytecode
where it matches. That is a rewrite:

- it runs with shapes on and off, and its match is not recorded anywhere
  but in the code;
- the differential test (8.2) still holds: both runs rewrite, so they
  compile to the same program, give the same outcome and tag the same
  functions (none with `no_shapes`);
- `no_shapes` keeps its meaning as the off switch of fast paths.

The form could have been one more hand-written look-back in the compiler
(the habit the top of this document describes). As rules, it is declared
in one place, with the generator's checks, and the next desugaring adds
rules, not another matcher.

### 12.2 The anchored lane

The labeller cannot match it. Most of a program is in a dead region (5.3),
which keeps no cells, so a rule rooted at a name read anywhere never sees
the name; and a rule on a bracket that takes the nodes before it, as a
call takes its callee, is refused (4.3) for the same reason. Keeping a cell
for every node would bring back the 15% to 20% of compile throughput that
dead regions saved (6.5).

But the rewrite's pattern is a left spine: a name, a member or an index of
it, a member of that, a call of that. Each node's first operand is the
node before it, every node of the spine starts where the name does
(`sjs_event.start`: an operator's, a member's, an index's or a call's node
starts with its first operand), and the other children (an index's key, a
call's arguments) can be anything. So the first operand can be found by
its start rather than by its place on a stack, and nothing else needs a
cell. The matcher runs such rules in a second, small automaton beside the
labeller, the **anchored lane** (`src/matcher.c`), on every event, in a
dead region too:

```
lane(e):
    no anchored rule on e, and e starts after the top cell: nothing    # most events
    pop the cells that start after e's node does   # they ended inside it
    top is a marker of e's node:
        a separator: `...` takes it; the close: the marker's rules derive a cell
    top starts where e's node does:
        pop it: it is e's first operand, or inside it
    e has anchored rules:
        a leaf derives a cell; a postfix event, or an open that takes one
        node, derives from the operand: a cell, or a marker of the
        bracket's rules that want the operand
    a cell: its chain rules, then a report for each rewrite it derives
```

Why the operand is right: nodes that start at one position are nested (a
node has a token, and siblings do not overlap); an event comes after its
children; so a cell still on the lane that starts where an event's node
starts ended before the event inside that node, and is its first operand
when the event takes one. An operator that is not a spine node (`(a +
b).k`, `f(x).k`) pops the cell as its own operand, so the member after it
finds none. The compiler finds an assignment's target the same way
(docs/mutation.md, "Assignment").

- **Cost:** an event no anchored rule looks at that starts after the top
  cell costs a comparison; any other pops what it ends and compares two
  starts. The lane holds a cell only for a node that derives an anchored
  nonterminal (here, every name read, and the spine above it), and pops it
  at the next event of an enclosing node. `mise run bench-compare 1ac1e956
  compile --quick` (machine `172a3dd3bd74`): compile throughput 2% to 6%
  lower, one row (`compile.guard.event.whole`, -6.3%) past 5% in every run,
  the rest within the noise; no allocation more; peaks up by 168 bytes per
  compile, the lane's 128 bytes of cells and its fields in the matcher. The
  first version, which ran the whole lane step on every event, cost 4% to
  11%.
- **Memory:** a cell is 16 bytes (start, node, a bit per anchored
  nonterminal, or for a marker a bit per bracket rule alive). The first 8
  are in the matcher itself; past them, the heap, doubling. Starts increase
  up the lane, so it holds at most one cell per enclosing node: within the
  labeller's bound, 4 × max_nesting + 4, which matcher_test checks on
  every spec source and the deepest programs (255 cells at 255 nested
  path calls; at most 3 over the spec).
- **Unreadable streams:** node 0, or more cells than the bound, stop the
  lane, independently of the labeller; reports made before stand. The
  parser's streams never stop it (matcher_test).

### 12.3 Rules

`rewrite NAME;` declares a rewrite; `NAME() <- pattern;` is a rule for it.
The nonterminals a rewrite's rules use, and theirs in turn, are
**anchored**: the lane matches their rules, the labeller never sees them,
and no shape's rule may use one. An anchored rule is

- a leaf: `named() <- NAME;`
- a postfix event over its first operand, whose other children are `_`,
  any node: `stepped() <- INDEX(path(), _);`
- a bracket that takes the node before it, then `...`, whatever it holds
  up to its close: `MUTATE_PATH() <- CALL_BEGIN(mutating(), ...);`
- a chain rule: `path() <- stepped();`

Its tests compare a field with a constant (`MEMBER{key: "push"}`); it
binds no variable, hands up no attribute and has no cost: a node derives a
set of anchored nonterminals, and a rewrite reports where it ends. The
generator checks each form (4.3), with `mise/tasks/shapes-test`; `_` and
`...` anywhere else are an error.

The rules of MUTATE_PATH (`src/shapes.rules`):

```
rewrite MUTATE_PATH;
named() <- NAME;
path() <- named();
path() <- stepped();
stepped() <- MEMBER(path());
stepped() <- INDEX(path(), _);
mutable() <- stepped();
mutable() <- CALL_BEGIN(chaining(), ...);
chaining() <- MEMBER{key: "sort"}(mutable());       # and reverse, fill, add
mutating() <- chaining();
mutating() <- MEMBER{key: "push"}(mutable());       # and pop, shift, unshift, splice, delete, clear
MUTATE_PATH() <- CALL_BEGIN(mutating(), ...);
```

A receiver needs a step (`stepped`): a variable read directly is the
compiler's own mutation (docs/mutation.md, "The rule"). An optional chain,
a computed method name, or a path from a call or an operator is no
`mutable`, so it reports nothing. A call of a method that returns its
receiver is `mutable` again, so a chain reports each of its calls.

The generator writes `sjs_rewrite` and `sjs_rewrite_name` into
`src/shapes.h`, and into `src/shapes_table.h` the lane's tables:
`shape_anchored_events` (each kind's rules), `shape_anchored_rules` (the
nonterminal derived, the form, the anchored nonterminal of the first child,
the tests, in `shape_tests`), and `shape_anchored_chain_rules`. Anchored
nonterminals are numbered from 1, the rewrites first, in a `uint32_t`.

### 12.4 The report

```c
typedef struct {
    sjs_rewrite rewrite;
    uint32_t node;         /* the node it ends: the call's */
    sjs_position start;    /* where the node begins: the path's root */
} sjs_rewrite_match;
```

The reporter's `rewrite` callback (NULL ignores them) gets it during the
event that ends the node, the call's CALL_END, before the compiler
handles that event. That is early enough: the compiler has compiled the
node as an ordinary call, and has not yet emitted the call itself. It is
enough position: the node id names the call's construct, which the
compiler keeps open until then, and the compiler kept the path the call's
callee is from its CALL_BEGIN (12.5). So the report needed no more than
the node id and the start, which every report has; nothing the parser
sends changed.

### 12.5 The compiler acts on it

- At a CALL_BEGIN whose callee is a member on the path on top, ending at
  the GET_MEMBER just compiled, the compiler holds that path until the
  call ends (`paths_before`, `path.held`), so that its steps' pcs outlive
  the arguments; the call's construct keeps its node, the callee's pc and
  what was done with the root's value so far. A call it is not reported
  for drops the path at its CALL_END.
- `on_rewrite` marks the open call whose node the report names. At its
  CALL_END, `path_mutation_end` checks what needs scopes: the root is a
  variable the function owns (docs/mutation.md, "The rule"), and nothing
  in the keys or arguments changed it. Then it rewrites what it compiled,
  in place, instruction for instruction of the same size, as it rewrites
  an assignment's target: the root's read becomes a RECEIVER, each step a
  key, the method's METHOD_MEMBER a PATH_METHOD; and it emits PATH_CALL
  (and PATH_SORTED) instead of CALL. A chain's report finds the last
  PATH_CALL just before its callee and sets its CHAIN flag, as
  `mutation_begin` does for a variable's chain.
- A report the compiler cannot use (no path held, no chain before it) is
  ignored: the call stays an ordinary one. The matcher decides only the
  form.

### 12.6 The off switch

`sjs_matcher_config.rewrites_only` (the compiler passes `no_shapes`): the
lane runs, the labeller does not. With shapes, both run on each event, the
lane first; they stop independently on a stream they cannot read.

### 12.7 For the next rewrite

What MUTATE_PATH built is general:

- **Rules:** a `rewrite` and its anchored rules, for any form that is a
  left spine of leaves, postfix events and brackets over one node, with
  constant tests. The lane, the tables, the generator's checks and the
  report need no change.
- **The compiler:** a case in `on_rewrite` for the construct (or, for a
  postfix rewrite, the node the event itself is), and a retroactive
  rewrite where the node ends, as the paths already allow.

What it does not do yet, and what each would need:

- **Attributes** (a key's text, a count): the lane's cells would keep
  slots and text as the labeller's records do. MUTATE_PATH needs none, as
  the compiler reads the method's name from its own constant.
- **Children other than the first:** `_` matches any node; a rule that
  looks at an index's key, or a call's arguments, needs the labeller's
  cells, which a dead region does not keep.
- **Brackets that take no node before them** (an array or object literal,
  an arrow) are the labeller's, which can match them in a dead region
  already; a rewrite of one would be a labeller rule whose report the
  compiler takes as a rewrite, matched with `no_shapes` too. A template
  with any number of parts is neither's: the labeller's rules have fixed
  children, and the lane anchors a node at its first operand's start,
  where a template's first substitution does not begin. That is why
  `` s = `${s}...` `` is found by the compiler in its code (12.8). The lane
  could take it with a marker for such a bracket that checks the first
  child to end inside it, counting its separators (a template's head piece
  is one).

### 12.8 APPEND and APPEND_SUM

The second and third rewrites, for appending to a `let`
(docs/mutation.md, "Appending to a let"), needed rules only, and the
`ASSIGN` event's operator as a field (`op: operator`, 0 for `=`):

```
rewrite APPEND;
rewrite APPEND_SUM;
APPEND() <- ASSIGN{op: ADD}(named(), ...);       # s += e
summed() <- BINARY{op: ADD}(named(), _);         # s + e1
summed() <- BINARY{op: ADD}(summed(), _);        # s + e1 + e2 ...
APPEND_SUM() <- summed();
```

`ASSIGN` is a bracket over the node before it, its target, so APPEND is
MUTATE_PATH's form, reported at ASSIGN_END for the assignment's construct.
A plain assignment's value is its second child, which no anchored rule can
look at; so APPEND_SUM is reported at each `+` of a spine from a name,
anywhere (`a + x` in a reduce callback too, beside its SUM shape: the
matcher_test cases of `a + ...` list it), and the compiler keeps the last
spine's root read, found in its paths when the innermost `+` is reported,
and the pc of its last `+`. At ASSIGN_END of `s = ...` it takes the spine
when that `+` is the value's last instruction and its root is the value's
first, a read of `s`. Each report costs the compiler a look through its
open paths (a few), at the innermost `+` of a spine only.

## 13. Calls in a callback: pushes into reduce's accumulator

`rows.reduce((acc, r) => { acc.push(r.kwh); return acc; }, [])` builds an
array with owned mutation (docs/mutation.md): the accumulator moves into
the callback, which holds it alone, `push` changes it in place, and
`return acc` moves it back. That costs a call per element: in bytecode
`RECEIVER`, `METHOD_MUTATE`, the argument (`TAKE_LOCAL`, `GET_MEMBER`),
`MUTATE`, `POP`, `TAKE_LOCAL` and `RETURN`, and the loop's step, 10 units
of fuel and about 1,170 instructions per element (callgrind, 4380a1b),
of which the callback's frame, the mutation's checks and the method
lookup take most.

### 13.1 Rules

The callback's body is a call, so its rule is on CALL_BEGIN, an `open 1`
event: the callee is the node before it. That is 11.9.2's [brackets]:
the generator accepts a rule on an `open 1` event whose first child is a
node; `matcher_open` folds the top cell (the callee) into the new marker
as child 0, in the cell's place, with the cell's text; a dead region
passes such a bracket over, as its callee was never kept. A record
replaces a record, so 5.6's bound holds (`matcher_test`: 255 records at
252 nested pushes in a callback, the deepest the parser takes).

```
shape PUSH_KEY(key: text);    # (a, x) => { a.push(x.key); return a; }
shape PUSH();                 # (a, x) => { a.push(x); return a; }

nonterminal push_of(receiver: read);
push_of(a) <- MEMBER{key: "push"}(NAME{name: a});
PUSH_KEY(k) <- ARROW_BEGIN{params: 2}(PARAM{index: 0}, BIND{name: a}, PARAM{index: 1}, BIND{name: x}, PARAMS_END,
                                      BLOCK_BEGIN, EXPRESSION_STATEMENT(CALL_BEGIN(push_of(a), key_of(x, k), ARGUMENT)),
                                      RETURN(NAME{name: a}), BLOCK_END);
PUSH() <- ... the same with NAME{name: x} as the argument
```

Exactly two parameters, one statement that pushes one argument (a spread
is SPREAD_ARGUMENT, not ARGUMENT), and `return` of the accumulator. A
shape may now have no arguments (`PUSH()`, 3.1). The tables grow to 35
nonterminals and 72 slots: a record is 680 bytes on x86_64, so the first
1 KiB still holds one.

What it costs a compile: a call in a region a rule can still match (an
arrow's body) pushes a marker where it used to doom the region, and the
marker dies at once unless its callee is `name.push`; in a dead region
nothing changes.

### 13.2 The C step

`push_step` in `src/builtins.c` (MUTATION), called from REDUCE's branch of
`sjs_loop_step` when `sum_step` declines, after the callback is entered (on
the first step that would call it, so that a full frame stack or no room
for its slots faults where the bytecode does). A push's callback returns
its accumulator, so the accumulator keeps the initial value's kind:
`push_start` drops the shape when the loop starts unless it is an array,
and `sum_step`, unchanged, never sees a push loop's number, so a sum pays
nothing for pushes (its instructions per element are the same as before,
callgrind). With `acc` in LOOP_OUTPUT and
`x` the element at `frame->index`, it pushes in C when all of these hold:

1. `acc` is an array that LOOP_OUTPUT holds alone (`sjs_value_unique`).
   The bytecode moves the accumulator into the callback's slot and
   releases the loop's copy, so `MUTATE` sees the same count: one holder
   unless a variable, an element, the array reduced or anything else
   holds it too, and then the bytecode faults ("held elsewhere").
2. The stack has room for `PUSH_STACK` (3): the bytecode's reference to
   the slot, the method and the argument.
3. `acc` is neither frozen nor sealed (`SJS_INTEGRITY`), where push faults.
4. `acc` has room for one more element (`length < capacity`). So C never
   allocates: the element that grows the array, one in every doubling,
   runs in bytecode, which grows it or fails as it does today.
5. PUSH_KEY: `x` is an object with an own property `key`
   (`own_entry`, as the other shapes find a key). A missing key falls
   back, whether the bytecode then pushes `undefined` or faults on a name
   of Object.prototype; so does any other kind of element (an array's or
   a string's `length`, a set's `size`, null's fault).

Then the value is referenced and appended, as `array_insert` appends it,
`frame->index++`, and the step charges 1 unit for PUSH, 2 for PUSH_KEY
(the lookup, and push's unit for the element added) beyond the step's.
The accumulator stays in LOOP_OUTPUT, where `return a` would have put it.
Anything else runs the bytecode for this element only: an object's own
`push` (called as any method), a Set, a number or a string (faults), and
every fault above, at the callback's position.

- **Value.** The same array, with the same elements in the same order:
  the bytecode's `MUTATE` appends the same value to the same array.
- **Fuel** (`api_test`, `test_mutation_push_paths`): 10 units per element
  in bytecode for PUSH_KEY (the step, RECEIVER, METHOD_MUTATE, TAKE_LOCAL,
  GET_MEMBER, MUTATE with push's unit, POP, TAKE_LOCAL, RETURN), 9 for
  PUSH; 3 and 2 in C, one pause. `max_stack` from 5 to 10 and
  `max_frames` 2 and 3 fault at the same places both ways.
- **Profile** (callgrind, `bench --only=eval.array.push-reduce
  --size=10000`, the evaluation and the release of its result): 1,173
  instructions per element before, 220 after, mostly the loop's step and
  its dispatch, the key's lookup and the evaluator's loop.

