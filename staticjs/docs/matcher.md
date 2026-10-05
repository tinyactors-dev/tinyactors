# Shapes: a tree-pattern matcher on the event stream

Status: design (option A). Section 1, node identity in the events, is
implemented ([nodes]); the rest is not yet. Section 9 lists the four
commits that implement it, each with its tests.

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
not part of the four commits.

Three properties hold throughout:

- **A match only tags.** The compiler emits the same bytecode for every
  function whether or not it matched. A match records a shape on the
  function's `sjs_function_info`. A built-in may use it for a C fast path
  and must call the bytecode whenever the fast path's preconditions do not
  hold.
- **The rule.** With shapes on or off, a program gives identical values and
  identical faults (code, message, subject and position). Fuel can go down,
  never up.
- **Matching can be switched off** per compiler, without global state, so
  the tests run the whole spec both ways and compare (section 8).

```
bytes --> parser --events--> matcher (tee) --same events--> compiler
                                |                              ^
                                +--- match(shape, node, args) -+
```

The matcher does not depend on the compiler. Its checks see only the events
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

- **The compiler** ignores `start`. Before the matcher lands it ignores
  `node` too. From the [matcher] commit on, it reads `node` once per
  function: `begin_function` stores ARROW_BEGIN's `node` in the function's
  scope (`scope.node`), so that a match report names the function it is
  for. Errors and line tables keep using `at`.
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
file         = { event | shape | nonterminal | rule } ;
event        = "event" KIND action [ "item" ] [ field { "," field } ] ";" ;
action       = "leaf" | "postfix" ( INT | "count" ) | "open" INT KIND
             | "inner" | "close" ;
field        = lower ":" type ;            (* a name for a member of the event *)
type         = "count" | "operator" | "number" | "text" | "name" | "read" ;
shape        = "shape" UPPER "(" param { "," param } ")" ";" ;
param        = lower ":" ( "text" | "flag" | "count" | "number" ) ;
nonterminal  = "nonterminal" lower "(" [ attribute { "," attribute } ] ")" ";" ;
attribute    = lower ":" type ;
rule         = head "<-" pattern [ "cost" INT ] ";" ;
head         = ( lower | UPPER ) "(" [ term { "," term } ] ")" ;
pattern      = KIND [ "{" test { "," test } "}" ] [ "(" [ pattern { "," pattern } ] ")" ]
             | lower [ "(" [ term { "," term } ] ")" ] ;    (* a nonterminal *)
test         = lower ":" term ;            (* a field of the event *)
term         = lower                       (* a variable *)
             | INT | STRING | "true" | "false" | UPPER ;  (* a constant; UPPER: an sjs_operator *)
```

`KIND` is an `sjs_event_kind` without `SJS_EV_`. Comments run from `#` to
the end of the line.

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
consumer, not before. Nothing in the four commits uses it, and the repository
removes what nothing uses.

---

## 4. The generator

### 4.1 Input and output

- **Input:** `src/shapes.rules`, and `include/staticjs.h` for the list of
  `sjs_event_kind` and `sjs_operator` names.
- **Output** (checked in, with a "generated by `mise run shapes`; do not
  edit" banner):
  - `src/shapes.h`: what the rest of `src/` and the tests may include. It
    holds the shape ids, their argument kinds and their names.
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
    test framework.
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
the pattern around it uses. An equality between two occurrences is checked
at the lowest rule where both are available: a leaf's or postfix's own
tests, or the bracket rule that holds both children. Identical normalised
rules are merged, so `NAME{name: x}` under both `key_of`s is one
nonterminal.

### 4.3 Checks (errors name `shapes.rules:LINE`)

- Every `sjs_event_kind` in `include/staticjs.h` is declared exactly once.
  Every `close` kind closes exactly one `open` kind, and an `open`'s CLOSE
  is declared `close`.
- A pattern has the right number of children for its event's action.
  `inner` events appear only inside a bracket's child list, and `close`
  events never appear in patterns.
- Tests name declared fields. Constants have the field's type (an operator
  name for `operator`). Every variable in a head is bound, and a variable's
  occurrences agree in type (`name` and `read` agree with each other).
- An `open N` event with N > 0 captures no text. Its marker goes below cells
  that already hold text, so a capture there would break the text stack's
  order (section 5.2).
- **Closure check.** In a shape rule, every `read` variable is also bound
  from a `name` (a BIND) in the same rule. Without it, a rule could match
  `x => y.k` with `y` a capture.
- Every shape rule's pattern is an `ARROW_BEGIN` bracket, because the
  compiler records shapes on functions. The check goes when another node
  kind gets shapes.
- Chain rules form no cycle. The generator emits them in topological order,
  so the matcher closes over them in one pass.
- Size limits fit the table's types: at most 255 nonterminals, at most 32
  bracket rules per open kind (the `alive` mask), at most `SHAPE_VARIABLES`
  variables per rule, and costs below 0xFFFF.

### 4.4 `src/shapes.h` (generated)

```c
#pragma once
#include <stdint.h>

typedef enum {
    SJS_SHAPE_NONE = 0,
    SJS_SHAPE_PLUCK,           /* key: text */
    SJS_SHAPE_COMPARE_BY_KEY,  /* key: text, descending: flag */
    SJS_SHAPE_COUNT
} sjs_shape;

/* The most arguments any shape has. */
#define SJS_SHAPE_ARGS 2u

/* What a shape's argument is: a text (recorded as a string constant), a
 * flag (0 or 1), a count, or a number (a number constant). */
enum { SJS_SHAPE_ARG_NONE = 0, SJS_SHAPE_ARG_TEXT, SJS_SHAPE_ARG_FLAG, SJS_SHAPE_ARG_COUNT, SJS_SHAPE_ARG_NUMBER };

/* Argument `i` of `shape`; SJS_SHAPE_ARG_NONE past its last. */
static inline uint8_t sjs_shape_arg(sjs_shape shape, uint32_t i) { /* a switch */ }
/* "PLUCK", ...: for tests and messages. */
static inline const char *sjs_shape_name(sjs_shape shape) { /* a switch */ }
```

Inline functions, rather than tables, keep `-Wunused-const-variable` quiet in
every translation unit that includes the header and uses none of them.

### 4.5 `src/shapes_table.h` (generated)

A sketch: names may change in the implementation, but not what the tables
say.

```c
#define SHAPE_NONTERMINALS 6u   /* shapes first (1..SJS_SHAPE_COUNT-1), then the rest; 0 is none */
#define SHAPE_SLOTS 10u         /* attribute slots of a record: the larger of a cell's
                                 * (sum of every nonterminal's attributes) and a marker's
                                 * (sum of the variables of one open kind's bracket rules) */
#define SHAPE_VARIABLES 4u      /* the most variables one rule has */
#define SHAPE_TEXT_MAX 64u      /* a longer captured text fails its test */
#define SHAPE_INFINITE 0xFFFFu

enum { SHAPE_LEAF = 1, SHAPE_POSTFIX, SHAPE_POSTFIX_COUNT, SHAPE_OPEN, SHAPE_INNER, SHAPE_CLOSE };
enum { SHAPE_FIELD_COUNT = 1, SHAPE_FIELD_NUMBER, SHAPE_FIELD_TEXT };
#define SHAPE_CONSTANT 0xFFu    /* a test that compares with a constant, not a variable */

/* How an event builds the tree, and which rules look at it. */
typedef struct {
    uint8_t action;        /* SHAPE_LEAF ... SHAPE_CLOSE */
    uint8_t arity;         /* SHAPE_POSTFIX: operands; SHAPE_OPEN: cells before it that it takes */
    uint8_t item;          /* its node is a whole child of its bracket, never an operand */
    uint8_t close;         /* SHAPE_OPEN: the sjs_event_kind that closes it */
    uint16_t first_rule;   /* leaf and postfix rules on this event, or bracket rules on this open */
    uint16_t rule_count;
} shape_event;

/* A test of one field of an event: bind or check `variable`, or compare
 * with `constant` (a count, or an index into shape_numbers or shape_texts). */
typedef struct {
    uint8_t field;
    uint8_t variable;
    uint16_t constant;
} shape_test;

/* A bracket rule's child: a cell deriving `nonterminal`, or the inner event
 * `kind` (nonterminal 0) passing its tests. */
typedef struct {
    uint8_t nonterminal;
    uint8_t kind;
    uint8_t test_count;
    uint8_t link_count;
    uint16_t first_test;
    uint16_t first_link;
} shape_child;

/* A child's attribute bound to one of the rule's variables. */
typedef struct {
    uint8_t attribute;
    uint8_t variable;
} shape_link;

/* One attribute of what a rule derives: a variable, or a constant. */
typedef struct {
    uint8_t variable;      /* SHAPE_CONSTANT: use `constant` */
    uint16_t constant;
} shape_result;

typedef struct {
    uint8_t lhs;           /* the nonterminal derived */
    uint8_t form;          /* leaf, postfix, bracket, chain */
    uint8_t kind;          /* the leaf's or postfix's event, or the bracket's open */
    uint8_t child_count;
    uint8_t variable_count;
    uint8_t test_count;    /* tests on the node's own event (the open's, for a bracket) */
    uint8_t first_slot;    /* bracket rules: its variables' first slot in a marker */
    uint16_t cost;
    uint16_t first_test;
    uint16_t first_child;  /* postfix and bracket rules: in shape_children */
    uint16_t first_result; /* in shape_results: one per attribute of lhs */
} shape_rule;

static const shape_event shape_events[SJS_EV_END + 1];
static const shape_rule shape_rules[];
static const shape_child shape_children[];
static const shape_test shape_tests[];
static const shape_link shape_links[];
static const shape_result shape_results[];
static const uint16_t shape_chain_rules[];                  /* in topological order */
static const uint8_t shape_slot[SHAPE_NONTERMINALS];        /* a nonterminal's first slot in a cell */
static const uint8_t shape_attribute_count[SHAPE_NONTERMINALS];
static const uint8_t shape_slot_field[SHAPE_SLOTS];         /* SHAPE_FIELD_*: what a cell's slot holds */
static const double shape_numbers[];
static const sjs_str shape_texts[];
```

Postfix and leaf rules are grouped by event kind, and bracket rules by open
kind, so `shape_events[kind]` gives a contiguous range. A kind no rule
mentions has `rule_count` 0, and the matcher skips the work for it.

### 4.6 The generator's tests (`mise run shapes-test`)

- A small rules file and the exact C it must produce (a golden string in
  the test), covering every rule form, fresh nonterminals, merged
  duplicates, a chain rule and the topological order.
- Each check in 4.3 has a rules snippet that must fail with its message and
  line.
- `shapes --check` fails on a stale header and passes on a fresh one. The
  test runs it on a copy in a temporary directory, never on `src/`.
- The real `src/shapes.rules` parses and every one of its rules is used.
- Its own run is deterministic: two generations give identical bytes.

---

## 5. The runtime matcher

### 5.1 Interface: `src/matcher.h` (private)

```c
#pragma once
#include "internal.h"   /* and through it shapes.h */

typedef struct {
    uint8_t kind;          /* SJS_SHAPE_ARG_* */
    uint32_t count;        /* a flag or a count */
    double number;
    sjs_str text;          /* valid during the report only */
} sjs_shape_arg;

/* A node that derives a shape: what, which node, from where, with what. */
typedef struct {
    sjs_shape shape;
    uint32_t node;         /* the event's node: for now always an arrow function */
    sjs_position start;
    sjs_shape_arg args[SJS_SHAPE_ARGS];
} sjs_shape_match;

typedef struct sjs_shape_reporter_vtable {
    /* SJS_OK to go on; any other result stops the matcher, which returns it. */
    sjs_result (*report)(void *self, const sjs_shape_match *match);
} sjs_shape_reporter_vtable;

typedef struct { void *self; const sjs_shape_reporter_vtable *v; } sjs_shape_reporter;

typedef struct {
    const sjs_allocator *allocator;   /* the context's counting allocator */
    uint32_t max_nesting;             /* sizes its bound (5.6) */
    sjs_sink downstream;              /* gets every event, unchanged, after the matcher */
    sjs_shape_reporter reporter;
} sjs_matcher_config;

typedef struct sjs_matcher sjs_matcher;

sjs_result sjs_matcher_init(sjs_matcher **out, const sjs_matcher_config *config);
/* The sink to give the parser: a tee. Valid while the matcher is. */
sjs_sink sjs_matcher_sink(sjs_matcher *matcher);
void sjs_matcher_destroy(sjs_matcher *matcher);
/* For matcher_test: the most records it held at once, and whether it stopped. */
uint32_t sjs_matcher_peak(const sjs_matcher *matcher);
bool sjs_matcher_stopped(const sjs_matcher *matcher);
```

The tee's event function matches the event first, and only then forwards
it. So a report about a node arrives while the downstream still has that
node open: the compiler learns that a function has a shape before it sees
the function's ARROW_END. A downstream result other than SJS_OK is returned
unchanged. The downstream keeps its own error, as the parser expects of a
sink.

### 5.2 Data

The matcher has two stacks of records, both `sjs_buf`, grown through the
allocator and freed by `destroy`:

```c
/* A record: a cell (a complete node) or a marker (an open bracket). Records
 * have a fixed stride: this header, then SHAPE_SLOTS values. */
typedef struct {
    uint32_t node;
    uint8_t kind;          /* the event that made it: a leaf's, a postfix's, a bracket's open */
    uint8_t marker;
    uint8_t dead;          /* a cell deriving nothing, or a marker with no candidate left */
    uint16_t children;     /* marker: children folded so far */
    uint32_t outer;        /* marker: the index of the marker around it */
    uint32_t alive;        /* marker: a bit per bracket rule of its kind still matching */
    uint32_t text_base;    /* its first byte in the text stack */
    uint16_t cost[SHAPE_COSTS];  /* cell: per nonterminal; marker: per bracket rule.
                                  * SHAPE_COSTS = max(nonterminals, rules per open kind) */
    uint8_t rule[SHAPE_NONTERMINALS]; /* cell: the rule that derived each nonterminal */
} shape_record;

/* A slot: what its type says (static, from the table), so 8 bytes. */
typedef union {
    double number;
    uint32_t count;
    struct { uint32_t offset, length; } text;   /* in the text stack */
} shape_value;

struct sjs_matcher {
    const sjs_allocator *allocator;
    sjs_sink downstream;
    sjs_shape_reporter reporter;
    uint32_t bound;        /* records at most: 4 * max_nesting + 4 */
    uint32_t top;          /* the innermost marker */
    uint32_t peak;
    bool stopped;
    sjs_buf records;       /* shape_record + SHAPE_SLOTS shape_value each */
    sjs_buf text;          /* captured text, in record order */
};
```

A cell keeps, for each nonterminal it derives, the cost, the rule, and that
derivation's attributes in the nonterminal's slots (`shape_slot[nt]`). A
marker keeps, for each bracket rule of its open kind that is still alive,
the cost so far, and the variables bound so far in the rule's slots
(`first_slot`).

**Text** captured from events (a key, a parameter's name) is copied, since
`sjs_event.text` lives only during the call. It goes into the text stack in
record order, and every record knows where its text begins. When records
are replaced (a reduction, a fold), the text that the new record's
attributes still reference is moved down to the region's start, in
increasing offset order (a forward copy, as the destination is never past
the source), and the stack is cut after it. A text longer than
SHAPE_TEXT_MAX is not captured, and its test fails. A key that long only
loses the fast path.

### 5.3 Per event

```
on_event(e):
    if stopped: return forward(e)
    r = step(e)
    if r == CANNOT_READ: stopped = true; free both stacks   # 5.5
    elif r != SJS_OK: return r                             # out of memory, or the reporter's result
    return forward(e)

step(e):
    m = the innermost marker (the root marker, node 0, is pushed at init)
    if e.node == m.node:                                   # an event of the open bracket
        fold_cells_above(m)
        if action(e.kind) == CLOSE:
            if e.kind != close_of(m.kind): return CANNOT_READ
            cell = finish(m, e); replace m by cell; return created(cell, e)
        fold_separator(m, e); return OK
    if e.node == 0: return CANNOT_READ                     # only END belongs to the program
    switch action(e.kind):
      LEAF:          cell = derive(e, no children); push cell; return created(cell, e)
      POSTFIX N:     the top N records must be cells above m, else CANNOT_READ
                     cell = derive(e, those N); replace them by cell; return created(cell, e)
      POSTFIX_COUNT: as POSTFIX with N = (e.count != 0)
      OPEN N:        the top N records must be cells above m, else CANNOT_READ
                     insert a marker below them: node e.node, kind e.kind,
                     alive = the bracket rules of e.kind whose open tests pass
                     (their variables bound from e into the marker's slots)
      INNER, CLOSE:  return CANNOT_READ                    # an event of no open bracket

created(cell, e):
    if the cell derives a shape: report(cheapest shape, e.node, e.start, its attributes)
    if item(cell.kind): fold_cells_above(innermost marker)
    return OK
```

- **`derive(e, children)`** is iburg's labeller at one node. Every rule
  starts at SHAPE_INFINITE. For each leaf or postfix rule on `e.kind`, its
  own tests run against `e`'s fields: constants compare, variables bind.
  Then each child's cost for the rule's nonterminal at that position is
  read; if one is infinite, the rule is skipped. The child's attributes are
  linked into the rule's variables, and a variable already bound must be
  equal. The cost is the rule's cost plus the children's, saturating. If it
  is lower than the cell's cost for the rule's lhs, it replaces it, with the
  rule and the lhs's attributes from `shape_results`. Then the chain rules
  run once, in topological order. Variables live in a local array of
  SHAPE_VARIABLES values. A cell deriving nothing is marked `dead`. For an
  event kind with no rules, that is all that happens: one record with
  infinite costs.
- **`fold_cells_above(m)`** folds every cell above marker `m` into it, left
  to right, then cuts the records and the text back to the marker.
  **`fold_separator(m, e)`** folds an inner event the same way. Folding
  child number `m.children` into each alive bracket rule `r`:
  - If `r` has no child at that position, the rule dies.
  - If the position wants a nonterminal, the child must be a cell with a
    finite cost for it. Its attributes are linked into `r`'s variables.
  - If the position wants an inner event of some kind, the child must be
    that event, and the symbol's tests run on it.
  - A failed check kills the rule; otherwise the cost accumulates.

  `m.children` then counts one more. A marker with no rule alive is `dead`,
  and folding into it does nothing but count.
- **`finish(m, e)`**: each alive rule whose child count equals
  `m.children` derives its lhs at its accumulated cost plus its own. Then
  the chain rules run, as in `derive`. The new cell takes the bracket's node
  id. Only after `finish` is the close event forwarded: the downstream has
  not yet seen the node end when the report arrives.
- **Reports** come from `created`. The arguments are a shape nonterminal's
  attributes, as `sjs_shape_arg`, with text pointing into the text stack.
  The report is made before the closing event is forwarded, so the compiler
  still has the function open. A reporter result other than SJS_OK stops
  the event: it is returned, and nothing is forwarded.

Nothing recurses: rules are table rows, children are a loop, and chain
rules are one pass in a fixed order. Every function has at most three
parameters (`derive` and `finish` take a small context struct). File-scope
names begin with `matcher_` or `shape_`, so they mean one thing in
check-recursion's amalgamation.

### 5.4 Example

`xs.map(x => x.k)`:

| event | node | records after it (top right) |
|---|---|---|
| (init) | | root⟨0⟩ |
| NAME xs | 1 | root, xs |
| MEMBER map | 2 | root, ·map |
| CALL_BEGIN | 3 | root, call⟨3⟩, ·map (open 1: the marker goes below the callee) |
| ARROW_BEGIN(1) | 4 | root, call, ·map, arrow⟨4⟩ [PLUCK×2 alive: params 1] |
| PARAM(0) | 4 | (inner: separator 1, PARAM{index 0} passes) |
| BIND x | 5 | (item: folded at once as child 2; `x` bound) |
| PARAMS_END | 4 | (separator 3) |
| NAME x | 6 | root, call, ·map, arrow, _1(x) |
| MEMBER k | 7 | root, call, ·map, arrow, key_of(x, k) |
| ARROW_END | 4 | close: key_of folded as child 4 (`x` = `x`). The block-body PLUCK dies, the other finishes. The cell derives PLUCK(k): **report PLUCK, node 4, start 1:8, key "k"**, then forward ARROW_END |
| ARGUMENT | 3 | inner: fold ·map and the arrow into call⟨3⟩ |
| CALL_END | 3 | close: a dead cell |
| END | 0 | close of the root |

### 5.5 Streams it cannot read

Some streams cannot be the tree of section 2: a close that does not match
the open bracket, an inner event with no bracket, node 0 on an event other
than END, too few cells for a postfix, or the bound exceeded (5.6). On any
of these, the matcher stops matching, frees its stacks and forwards
everything from then on. It never fails the compile for this: the compiler
reports a malformed stream itself ("the events are out of order"), and a
stream it accepts compiles the same, untagged. Reports made before the stop
stand; each one covered a complete subtree. matcher_test checks that the
parser never makes the matcher stop (every spec source), and that
`compile_events`' hand-built streams compile and fail exactly as they do
without it.

### 5.6 Memory

- **Records.** A marker stands for a bracket the parser has open: a frame
  (array, object, call, `new`, template, arrow, if, block statement, const,
  pattern, default) or a pending operator (logical, the `:` of `?:`, an
  expression-bodied arrow). A cell that is not yet folded is an operand
  waiting for a pending operator or frame (the left side of `+`, the object
  of `a[`, a const's value before its pattern), or the operand being read.
  Separators, closes and items fold everything above their bracket, so
  neither width nor the number of statements adds records. So records ≤
  2 × (frames + operators) + 2 ≤ **4 × max_nesting + 4**. That is 1,028 at
  the default limits.
- **Per record:** a 20-byte header, 2 × SHAPE_COSTS + SHAPE_NONTERMINALS
  bytes, and 8 × SHAPE_SLOTS. With the first rule set (about 6 nonterminals
  and 10 slots) that is about 120 bytes: about 123 KB at the bound. A
  typical guard or module holds a few dozen records.
- **Text** ≤ records × text slots per record × SHAPE_TEXT_MAX. The generator
  computes the default-limit figure into a comment in `shapes_table.h`.
- **Allocations:** the matcher struct and the two stacks, three in all,
  each reserved once and grown by doubling. Memory goes through the
  context's counting allocator, so `sjs_context_live_bytes` sees it and
  `spec_test --full` fails each allocation.
- **Beyond the bound** the matcher stops matching (5.5) instead of
  allocating further. matcher_test compiles the deepest programs the limits
  allow (operators, brackets, if chains, patterns, each at max_nesting) and
  checks `sjs_matcher_peak` stays within the bound and that it never stopped.

Precomputed BURS state tables would replace the cost vectors with one state
number per record. They are worth it only if a compile profile shows the
labeller hot; until then, the dynamic programme stays.

---

## 6. The compiler records the shape

### 6.1 Wiring

- `sjs_compiler` gains `sjs_matcher *matcher`, which is NULL when matching
  is off (section 8). `sjs_compiler_init` makes it with the compiler's own
  internal sink as downstream, the compiler as reporter, and
  `context->limits.max_nesting`. `sjs_compiler_sink()` returns the
  matcher's sink when there is a matcher, and the compiler's own otherwise.
  Embedders keep calling `sjs_compiler_sink()`, and `sjs_compile()` needs no
  change.
- The parser's error path does not change: the compiler reports the
  matcher's failures (out of memory) as its own. The tee returns the
  compiler's result, and the compiler sets `c->error` with `fail_result`
  when the matcher's allocation fails.
- `scope` gains `uint32_t node`, set by `begin_function` from ARROW_BEGIN.

### 6.2 Recording

The reporter, `on_shape(c, match)`, runs during the ARROW_END that closes
the matched function, before the compiler sees that ARROW_END:

1. If the compiler has failed, return its error.
2. Find the innermost function scope. If it is not a function (scope 0 is
   the program), or `scope.node != match->node`, ignore the report. Today
   every shape is an arrow's, reported at its ARROW_END, so this never
   happens unless the parser's ids are wrong. matcher_test counts ignored
   reports and expects 0.
3. On the function's `sjs_function_info`, set `shape` and convert each
   argument by its kind:
   - A **text** becomes `string_constant()`, the same interned, `NAMED`
     string constant GET_MEMBER uses for that key (constants are one per
     text). `string_constant` keeps its signature: `on_shape` passes it a
     MEMBER event built from the text and `match->start`.
   - A **flag** or a **count** is stored as the integer.
   - A **number** would become `number_constant()` (no shape has one yet).

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
`@shapes` lines.

---

## 7. The first consumers

Both fast paths sit in `src/builtins.c`, in the loop built-ins' steps.

### 7.1 Common rules

- **Set up once per loop.** In `sjs_loop_start`, if the callback is a
  closure whose function has the shape this built-in uses (COMPARE_BY_KEY
  for TO_SORTED, PLUCK for MAP), the frame records it. `vm_frame` gains
  `uint8_t shape`, `uint8_t shape_flag` (descending, or "the key is on
  Object.prototype" for PLUCK) and `const sjs_string *shape_key`.
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
  amount of fuel, and a step's work stays constant.
- **Fallback is the bytecode, unchanged.** When a precondition fails, the
  step calls back exactly as today (`call_back`). The bytecode then gives
  the value, or faults with its own message, position and subject.
- **Fuel.** A fast step charges what it looked up, one unit per lookup,
  which docs/builtins.md asks for. With the run loop's unit for the step,
  that is less than the bytecode it replaces. The figures below are today's
  bytecode, and `api_test` pins both sides exactly.

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

### 8.1 The switch

It is a field of the public compiler config, so it needs no global state
and no internal hook. Every existing positional initialiser leaves it
`false`, which means matching is on:

```c
typedef struct {
    sjs_context *context;
    sjs_goal goal;
    const sjs_str *globals;
    uint32_t global_count;
    sjs_modules modules;   /* optional: v may be NULL */
    /* Compile without matching shapes (docs/matcher.md): no function is
     * tagged, and every built-in runs its callback's bytecode. A program
     * gives the same values and faults either way, and never uses more fuel
     * with shapes. For tests that compare the two, and for an embedder
     * ruling the fast paths out. */
    bool no_shapes;
} sjs_compiler_config;
```

With `no_shapes`, `sjs_compiler_init` makes no matcher, so it costs
nothing. `sjs_compile_params.compiler` carries the field, so `sjs_compile()`
honours it too. A public field also lets `bench` run any case both ways
when a question calls for it, though `bench-compare` across commits stays
the record.

### 8.2 The differential test

- **`spec_test`**, fast pass. Every case runs as today (whole, bytes,
  chunks, with shapes on), and once more whole with `no_shapes`. The two
  whole runs must have identical outcome text: the written value; or the
  fault's code, line:column, message and subject; or the compile error. The
  run with shapes must use no more fuel than the run without
  (`sjs_vm_fuel_used`, which `evaluate` now reports). Under `no_shapes`,
  every function's shape must be NONE. The bytes and chunks runs, with
  shapes on, already pause on every unit of fuel and every 13, so they
  resume fast loops mid-sort and mid-map.
- **`spec_test --full`** fails every allocation with shapes on, which
  covers the matcher's and the compiler's recording.
- **`api_test`**:
  - fuel pinned both ways for a key sort and a pluck;
  - `test_fuel_pinned`'s program has no shape and keeps its 194;
  - the limit edges (5.6, 7.1).
- **`bench --smoke`**: unchanged. It checks values, and those do not move.

---

## 9. The commits

Four commits, in this order, each passing `mise run test`. The task named
three ([nodes], [matcher], [consumers]). The fourth, [differential], holds
the off switch and the both-ways spec run. It lands before the fast paths,
so they arrive under the test that guards them. This document lands with
[nodes] and is updated as each later commit lands.

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
- `mise/shapes.rb`, `mise/tasks/shapes`, `mise/tasks/shapes-test`, and the
  `test` task running `shapes --check` and `shapes-test`;
- `src/matcher.h` and `src/matcher.c`;
- the compiler wiring and recording (section 6);
- `sjs_function_info.shape` and `shape_args` (with `flags` shrunk to
  `uint8_t`);
- the `@shapes` spec directive, in `spec_test.c` and `diff-node.mjs`;
- `src/README.md` (the new modules) and `docs/matcher.md`.

Nothing reads a shape yet, so no value and no fuel can change.

Tests:

- `mise run shapes-test` (4.6), and `mise run shapes --check` in every
  `mise run test`.
- `tests/spec/shapes.spec` with `@shapes`:
  - **Positives:** `x => x.k`, `(x) => x.k`, `x => (x.k)`,
    `x => { return x.k; }`, the four comparators, nested functions (an
    outer function untagged and an inner one tagged, and the reverse), a
    shaped arrow in a parameter default, and one in a held `const`
    pattern's default.
  - **Negatives:**
    - `x => y.k` (a capture);
    - `(x = 1) => x.k`, `({k}) => k`, `(...x) => x.k`, `(x, i) => x.k`;
    - `x => x?.k`, `x => x["k"]`;
    - `(a, b) => a.k - b.j`, `(a, b) => a.k - a.k`, `(a, b) => b.k - b.k`;
    - `(a, b) => a.k + b.k`;
    - `x => { const y = x; return y.k; }`;
    - `x => { { const x = {k: 5}; return x.k; } }` (shadowing in a block);
    - `x => { if (x) return x.k; }`;
    - a `"use strict"` directive before the return.

  Every case also has its value or fault, which `diff-node` checks against
  Node.
- `tests/matcher_test.c`, a new internal test program in
  `StaticJS::TESTS`. It includes `src/matcher.h` and drives a parser into a
  matcher, with a recording downstream and a recording reporter:
  - **Tee:** for every spec source, the downstream's events equal the
    parser's own, byte for byte, field for field.
  - **Reports:** for a list of sources, the exact reports (shape, node,
    start, arguments), and the compiler ignores none.
  - **Never stops** on any spec source, nor on the deepest programs the
    limits allow, and `sjs_matcher_peak` stays within 4 × max_nesting + 4.
  - **Width:** an array of 100,000 elements, a module of 2,000 consts and a
    template of 1,000 substitutions each peak at a handful of records.
  - **Unreadable streams:** `compile_events`' streams and hand-made bad
    ones (a stray close, node 0, too few operands) make it stop, and it
    keeps forwarding.
  - **Out of memory:** every allocation of a matching compile fails in
    turn, each ends with SJS_ERR_NOMEM, and nothing leaks.
- `spec_test --full` covers the compiler with the matcher. `mise run asan`
  runs all of it under the sanitizers.
- `bench-compare` for `compile.*`, in the body. Compile throughput and
  allocations per compile will move (three allocations more per compile,
  and the peak by the records used). The body says so and quotes the rows.
  If guards suffer, the first remedy is a small inline record stack in the
  matcher struct, spilling to the heap past it.

### 3. [differential] `test(staticjs): compile the spec with and without shapes and compare`

Changes: `sjs_compiler_config.no_shapes` (8.1), and `spec_test` running
every case both ways (8.2).

Tests:

- **The differential run itself.** With no consumer yet, the outcomes and
  the fuel are equal. The test asserts outcomes equal and fuel with shapes
  ≤ fuel without.
- `api_test`:
  - `no_shapes` makes no matcher (allocation count) and leaves every
    function untagged;
  - `sjs_compile` honours it;
  - a positional config initialiser still means "on".

### 4. [consumers] `perf(staticjs): sort by a key and pluck a key in C when the callback's shape allows`

Changes:

- `src/vm.h`: `vm_frame.shape`, `shape_flag` and `shape_key`; `LOOP_AGAIN`;
  `sjs_vm_enter_callback`.
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

These come after the four commits, in the order a profile asks for them.

- **KEY_EQUALS** (3.3) for `filter`, `find`, `findIndex`, `some`, `every`.
- **The compiler's own peepholes as rules,** once a non-arrow node can
  carry a report: OBJECT_LITERAL's gathered prefix (an `OBJECT_BEGIN` hedge
  of static KEYs), and `call_begin`'s METHOD_MEMBER rewrite (a `CALL_BEGIN`
  whose callee is a MEMBER). That needs the compiler to accept a report for
  the node it is compiling, by id, which the [nodes] ids already allow.
- **Constant folding and superinstructions,** as further rule sets with
  costs, where competing rules choose.
- **Precomputed BURS states,** only if the labeller shows in a compile
  profile.
- **Key caching in the sort** (each element's key read once). It is not in
  the first commit, because it would charge fuel for keys of elements that
  are never compared.
