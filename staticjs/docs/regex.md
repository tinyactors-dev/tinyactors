# Regular expressions: a linear-time automaton run as a loop frame

Status: being implemented, in the order of section 13. Step 1 is done:
the lexer reads a literal (`L_REGEX`), the parser classifies it
(`src/regex.c`, `sjs_regex_check`) and sends `SJS_EV_REGEXP`, and the
compiler makes an `SJS_REGEXP` constant with its source, flags and
program. Step 2 is done: the Pike VM (`src/regex_match.c`) runs `test`,
`search`, `split`, `match` with g, and `replace` and `replaceAll` with a
string replacement, as a leaf or a loop frame. Step 3's match results are
done: `exec`, `match` without g, `matchAll`, `d` indices, `$<name>`, and
`replace` with a function, a loop that calls back. Every bench case of
section 12.1 is in `bench/bench.c`, with `eval.regex.literal` besides.
Of 12.1's speedups, the literal-prefix skip, the anchored start and a
lazy DFA for `test` are built; the one-pass program and the predicate
shape are not (section 15 says why). Where the implementation departs from this design, section 15
says so. Line numbers below are of the commit this document was written
against (`dbc0c69e`), before any of it.

This document designs how staticjs could accept a subset of ECMAScript
regular expressions and match them exactly as Node 22 does, or fault, or
refuse them when compiled. It chose this design (called "Design C" in the
analysis that led to it) over two others:

- **A backtracking interpreter**, also run as a loop frame. It covers more
  of the language (backreferences, lookbehind), but its worst case is
  exponential: fuel can pause it, but cannot make `/(a+)+$/` on
  `"a".repeat(30) + "b"` finish within a reasonable budget.
- **Regular expressions in the main bytecode**, with new opcodes in
  `execute()`, or compiled into ordinary staticjs closures. The first needs
  backward jumps and grows the dispatch loop every program runs; the second
  cannot express a loop over the input at all, as section 2 explains.

How to read the labels:

- **Verified** claims cite a file and line in this tree, at the commit this
  document was written against (`dbc0c69e`).
- **Inferred** claims are reasoning about the specification or about other
  engines that has not been checked against Node or against code. Each is
  marked *(inferred)*. The prototypes of section 13 exist to check them.
- **Estimated** figures are guesses at cost, marked *(estimate)*. Only
  `bench-compare` can replace them.

---

## 1. Summary

- A regular expression literal is read by the lexer as one token, parsed by
  an iterative pattern parser when the program compiles, and compiled to a
  small program for a **Pike VM**: a Thompson automaton that runs all its
  threads in step over the input, in priority order.
- The compiled program is a constant of the staticjs program, an immutable
  value of a new kind, `SJS_REGEXP`.
- A match runs as a **loop frame** on the evaluator's existing frame stack.
  Each step advances the automaton over a bounded budget of work, charges
  fuel for what it did, and returns, so an evaluation pauses inside a match
  like any loop. The dispatch loop `execute()` gains no case.
- Short matches, most guards, run as a leaf in one C call (section 9.1).
- Matching costs time linear in the input: O(n × m × 2^d × s) for n
  positions, m instructions, s capture slots and d nested loops whose
  bodies can match empty, all fixed when the pattern compiles (section
  5.3). There is no catastrophic backtracking,
  and fuel stays proportional to time.
- What a linear-time automaton cannot do exactly (backreferences, and at
  first lookaround), and what needs Unicode tables staticjs does not have
  (`\p{...}`, the `v` flag, case-insensitive non-ASCII letters), is refused
  when compiled as `unsupported`.

---

## 2. Why not the main VM, and why not closures

**The main bytecode jumps only forward (verified).** The compiler's header
says so (`src/compiler.c:5`), and the last-read analysis that turns a
`LOCAL` into a `TAKE_LOCAL` depends on it: "every jump goes forward and a
function's body runs at most once per frame, so on any path through a frame
instructions run in increasing pc order" (`compiler.c:52-60`). The README
gives the same property as part of why every evaluation ends
(`README.md:45-50`). A matcher in the main bytecode needs backward jumps.

**The dispatch loop is placement-sensitive (verified).** `execute()` is
`always_inline` into `sjs_vm_run`'s loop because "a call per instruction
costs 5 to 10% of every evaluation ... when one more case tips it over"
(`src/vm.c:1089-1093`). `docs/extending.md` ("To tell a real slowdown from
code placement") says to keep new code out of `execute()`'s cases. Ten or
more regex opcodes there would tax every program.

**The value stack is the wrong store (verified).** A value is 16 bytes
(`include/staticjs.h:205-213`) and the stack has `max_stack` slots, 4,096
by default (`staticjs.h:153`). Captures and positions are `uint32_t`s; a
backtrack stack or thread list of values would hit "needs more than
max_stack" on ordinary inputs.

**Closures cannot loop (verified).** staticjs has no loop statement, and a
closure that is already running cannot be entered again
(`closure_running`, `vm.c:701-711`). The only way to visit every position
of an input is a loop built-in over its code units, with a callback per
unit. That would simulate an automaton in bytecode, with a set of states as
a value; *(estimate)* 10 to 50 times slower than C, and captures would
need large states.

**What a loop frame gives (verified).** A built-in that loops is a
`FRAME_LOOP` on the evaluator's own stack (`src/vm.h:9-26`). Its step may do
a unit of C work itself and return `LOOP_AGAIN` (`vm.h:171-173`), as the
shapes' C paths do (`src/builtins.c:1643-1651`, `1981`). `sjs_vm_run`
charges one unit per step and checks the budget between steps
(`vm.c:1620-1631`). A step may charge more (`vm->fuel_used += ...`, for
example `builtins.c:1737`). A fault in a loop frame reports the call's
position (`fault_position`, `vm.c:13-18`). `toSorted` already routes to a
step function of its own at the top of `sjs_loop_step`
(`builtins.c:1901-1903`), which is the model for a match step.

So the matcher is an interpreter of its own, with its own instruction set,
but not a second evaluator: the existing trampoline drives it, and fuel,
pausing, faults and limits are the evaluator's.

---

## 3. Architecture

```
source --lexer: L_REGEX--> token (body, flags)
       --parser--> SJS_EV_REGEXP event
       --compiler--> pattern parser (iterative) -> AST -> Pike program
                     -> SJS_REGEXP constant in program->constants

re.exec(s) --call()--> enter_loop --> FRAME_LOOP, builtin = REGEXP_EXEC
           sjs_loop_step --> regex_step: one input position per step,
                             LOOP_AGAIN until a match or the end
           LOOP_DONE --> the result (null, or a match array) on the stack
```

### 3.1 Where each piece lives

| Piece | File (proposed) | Notes |
|---|---|---|
| Lexing the literal | `src/lexer.c` | a new state, `L_REGEX` (section 6) |
| The event | `include/staticjs.h`, `src/shapes.rules` | `SJS_EV_REGEXP`, declared `leaf` in the rules' event language |
| Pattern parser and compiler | `src/regex.c` (new) | iterative, explicit stack; no C library |
| The value kind | `src/internal.h`, `src/value.c`, `src/writer.c` | `SJS_HEAP_REGEXP` |
| The match step | `src/regex.c`, called from `sjs_loop_step` | behind one early branch, as `sort_step` |
| Methods | `src/builtins.c` | IDs, `NAMES` columns, `sjs_builtin_loops` (docs/builtins.md, section 2) |

A new `src/regex.c` keeps the new code out of the hot functions of
`builtins.c` and `vm.c`, as `json.c` does for JSON (`src/README.md:32`).
`docs/builtins.md` section 10 ("A test on every step that tips the
inliner") applies to the branch in `sjs_loop_step`: check with `llvm-nm`
that the set of out-of-line helpers does not change, and keep the new step
`noinline`.

### 3.2 State

- **The compiled program** is immutable and belongs to the regex value,
  which belongs to the staticjs program's constants (`program->constants`
  holds values: `src/internal.h:872`).
- **A match's state** (two thread lists, their capture slots, the cursor)
  is one block from the context's allocator, made when the loop starts and
  freed when it ends. The loop frame points to it. `vm_frame` must stay at
  112 bytes or less (`vm.h` `_Static_assert` after the struct), and
  `sjs_loop_free` today frees `scratch` as an array of values
  (`builtins.c:1592-1598`), so the regex frame needs its own branch there,
  or a field of its own in the union.
- **Nothing is cached in the regex value.** Programs are shared between
  evaluators, which is why `GET_MEMBER`'s guesses live in the VM and not in
  the program (`vm.h`, the comment on `member_guess`). The lazy DFA
  (section 12.3) keeps its cache in the match's own state for the same
  reason (section 15). What the value holds besides the program is
  derived from it once, when it compiles, and never changes: the lead
  character, and for a program the lazy DFA takes, the class of each
  ASCII character (section 15).

---

## 4. The instruction set

A program is an array of instructions, each a 32-bit opcode word and up to
two 32-bit operands *(proposed encoding)*. Program counters are indices.

| Instruction | Operands | Meaning |
|---|---|---|
| `CHAR` | c | consume one character equal to c |
| `CHAR_FOLD` | c | consume one character whose canonical form is c (`i`) |
| `ANY` | | consume any character but a line terminator (`.` without `s`) |
| `ANY_ALL` | | consume any character (`.` with `s`) |
| `CLASS` | class index | consume one character in the class: sorted ranges in a side table; negation is in the table |
| `SPLIT` | x, y | fork: thread at x first, thread at y second (priority order) |
| `JMP` | x | go to x |
| `SAVE` | slot | record the current position in a capture slot |
| `CLEAR` | first, last | set capture slots first..last to unset (section 5.1) |
| `MARK` | register | record the current position in a loop register (section 5.2) |
| `CHECK_PROGRESS` | register | kill the thread if the position equals the register's (section 5.2) |
| `ASSERT` | kind | zero-width: `^`, `$` (with or without `m`), `\b`, `\B` |
| `MATCH` | | the thread matched |

- A greedy `e*` is `L: SPLIT body, out; body: e; JMP L; out:`. A lazy
  `e*?` swaps the operands of `SPLIT`. `e+`, `e?` and `{n,m}` are the usual
  expansions; `{n,m}` copies the body, so its size counts towards the
  program limit (section 9).
- Thread priority is the order of `SPLIT`'s operands. A classic Pike VM
  adds threads to the next list in priority order and drops a thread whose
  program counter is already on the list. For Perl-style semantics that
  finds the match a backtracking matcher finds, with the same captures
  *(inferred: Cox, "Regular Expression Matching: the Virtual Machine
  Approach", 2009)*. **With ECMAScript's empty check it does not**: two
  threads at one program counter can differ in their loop registers, and
  the later one can be the only one that matches. Section 5.3 gives the
  counterexample and the rule that replaces "one thread per program
  counter".
- An epsilon closure (following `SPLIT`, `JMP`, `SAVE`, `CLEAR`, `MARK`,
  `CHECK_PROGRESS`, `ASSERT` without consuming) uses an explicit stack of
  at most m entries, not recursion (`AGENTS.md`, "No recursion";
  `mise run check-recursion`).

---

## 5. The two ECMAScript rules

A Pike VM written for Perl or RE2 semantics gives other captures than
JavaScript in two places. Both rules are in the specification's
RepeatMatcher *(inferred from ECMA-262, 22.2.2.3.1; not checked here
against the text)*. The expected values below are what the analysis
expects JavaScript to give; *(inferred)*: they must be generated in Node
before they become spec cases (`docs/extending.md`, "To write a spec
case", step 2).

### 5.1 Captures are reset at each iteration

Each iteration of a quantified atom starts with the captures inside that
atom unset. So `/(?:(a)|b)+/.exec("ab")` gives `["ab", undefined]` in
JavaScript: the second iteration matched `b`, and the `(a)` of the first
iteration does not survive it. Perl and RE2 keep `"a"`.

**Implementation.** The compiler knows, for each quantifier, the range of
capture groups inside its body. At the top of the body it emits
`CLEAR first, last` for the slots of those groups. A thread that enters a
new iteration then carries unset slots for them, whatever an earlier
iteration recorded. Nested quantifiers each clear their own range.

### 5.2 An iteration that matches the empty string fails

An iteration that consumes nothing, once the quantifier's minimum count is
reached, fails rather than looping. So `/(a*)*/.exec("b")` gives
`["", undefined]` in JavaScript: the outer `*`'s first iteration would
match the empty string, so it fails, and the outer `*` matches zero times,
leaving group 1 unset. Some other engines give `["", ""]`.

**Implementation.** Each quantifier whose body can match the empty string
gets a loop register. Each iteration **above the minimum count** starts
with `MARK r` (the position where this iteration began) and ends with
`CHECK_PROGRESS r`, which kills a thread whose position equals `r`'s value.
That covers every kind of optional iteration, not only the jump back of
`*` and `+`: `?` and each optional copy of `{n,m}` have no jump back, and
the check still applies to them. Checked in Node 22.23.2 by the review:
`/(a*)?/.exec("b")` and `/(a*){0,3}/.exec("b")` give `["", undefined]`,
while `/(a*){1,2}/.exec("b")` gives `["", ""]` (its one required copy is
not checked). A register is per thread, as the capture slots are, so a
thread carries `2 × groups + loops` slots. A body that cannot match empty
(it consumes at least one character on every path; the compiler computes
this) needs no register.

Below the minimum count (`{2,}` before its second iteration) the check does
not apply. The expansion of `{n,m}` makes the first n copies separate code,
emitted without `MARK` and `CHECK_PROGRESS`.

### 5.3 Dropping threads, and the bound

A classic Pike VM drops a thread that reaches a program counter already on
the list. With loop registers that is wrong. The review built a JavaScript
Pike VM exactly as sections 4 to 5.2 first described it and compared it
with Node on random patterns: `/(?:a*?|aa)*/.exec("aa")` gives `["aa"]` in
Node and `["a"]` in that VM. At position 1, a thread whose register says
"this iteration began at 0" reaches the lazy loop's `SPLIT` first; the
later thread, of an iteration that began at 1, arrives at the same program
counter and is dropped, though it is the only one that goes on to consume
the second `a` before `MATCH`.

**The rule:** a thread is dropped only when a thread with the same program
counter **and the same set of loop registers equal to the current
position** is already on the list. Whether a register equals the current
position is the only thing `CHECK_PROGRESS` can observe, so two such
threads behave alike from here on, and the earlier has priority. With that
rule the review's prototype agreed with Node on 1,000,000 random cases.

**The bound** is then m × 2^d threads per position, where d is the depth
of nested loops whose bodies can match empty. d is a property of the
pattern, known when it compiles, and in real patterns it is 0 or 1. The
compiler computes 2^d and refuses a pattern past a limit (section 9.3), so
matching stays O(n × m × 2^d × s), linear in the input. Barrière and
Pit-Claudel, "Linear Matching of JavaScript Regular Expressions" (PLDI
2024), treat these rules for a linear engine and may give a construction
without the 2^d factor *(inferred: cited from memory; read it before
prototype 2)*.

`CLEAR` and `MARK` write slots of the current thread; `CHECK_PROGRESS`
compares one. None of them adds threads.

---

## 6. The lexer and the parser

### 6.1 How a `/` becomes a regex token (verified mechanism; the change is proposed)

The lexer is fed one byte at a time and hands each token to the parser's
handler as soon as the byte after it arrives (`src/lexer.c:1-5`).
`emit()` calls the handler, then sets `lexer->state = L_START`
(`lexer.c:144-157`). The handler runs before the lexer reads the next
byte, so the parser can decide, while it handles a `/` or `/=` token,
that the token begins a regular expression:

1. The parser, where it now refuses (`parser.c:1492-1495` in operand
   position; `scan_starts_regex` in the scans, `parser.c:853`, used at
   `1015` and `2548`), sets a field on the lexer: "a regex starts here",
   with `=` as the first byte of the body when the token was `/=`.
2. `emit()` checks the field after the handler returns and enters `L_REGEX`
   instead of `L_START`. That is not enough on its own: after
   `flush_punctuator` returns, `punctuator_byte` sets
   `lexer->state = L_START` itself when nothing is left held
   (`lexer.c:267`), which would overwrite `L_REGEX`. That line must leave
   the state alone when `emit` entered `L_REGEX`.
3. `L_REGEX` reads the body up to an unescaped `/` outside a class
   (`[...]`), then the flags (identifier characters), and emits a token of
   a new kind, `SJS_TOKEN_REGEXP` (`src/syntax.h:8-18`), whose text is the
   body and whose flags are a bit set.

`punctuator_byte` holds at most the two bytes of `/=` before it flushes,
because no other punctuator starts with `/` (`lexer.c:231-272`, and the
table at `lexer.c:45-55`); `//` and `/*` begin comments before any token
is emitted (`lexer.c:234-242`). So no byte of the body has been consumed
when the parser decides. *(Inferred from reading the code; not run.)*

- The body may contain any UTF-8. Outside strings, templates and comments
  the lexer accepts ASCII only today (the message at `lexer.c:760`), so
  `L_REGEX` takes non-ASCII bytes as a string does. UTF-8 is already
  checked across chunks for every byte (`lexer.c:811-838`).
- A line terminator in the body is a syntax error, as in JavaScript.
- The body is one token, bounded by `max_token_bytes`
  (`staticjs.h:131`, 64 KiB by default).
- A token can span any number of chunks; the spec suite already pushes
  every source a byte at a time and in 7-byte chunks
  (`tests/spec_test.c:4-6`), so lexing across chunks is tested by every
  regex spec case.

### 6.2 The lookahead scans

While the parser reads ahead to tell an arrow's parameter list from a
parenthesized expression, it only balances brackets, and refuses at a `/`
that begins a regex because "the lexer cannot read" its body
(`parser.c:18-25`). With `L_REGEX`, the scan sets the same lexer field and
then sees one `SJS_TOKEN_REGEXP` token, which it can skip as it skips a
string. The refusal at `2548-2549` then goes away; `max_lookahead` counts
the regex as one token.

The scans must decide about a `/` during the handler call that delivers
it, because by the next call the lexer has gone on reading. Tokens that
were already queued and are looked at again later (`decide_paren`,
`cover_initializer`) have been lexed already, so the decision cannot wait
until then *(inferred from those functions; prototype 1 checks it)*.

### 6.3 The event

A new `SJS_EV_REGEXP` (appended to `sjs_event_kind`, `staticjs.h:455`) with
`text` the body and `count` the flags. It is a leaf in the event stream.
`src/shapes.rules` must declare it, or the shapes generator fails
(`docs/extending.md`, "To add syntax", step 2); run `mise run shapes` and
commit the generated headers. A new event kind changes the public header,
so `SJS_VERSION_MINOR` goes up.

### 6.4 The pattern parser

The compiler hands the body and flags to `sjs_regex_compile` in
`src/regex.c`:

- **Iterative.** Groups nest; the parser keeps an explicit stack of open
  groups and alternatives, bounded by `max_nesting`, as the main parser is
  (`README.md:51-53`).
- **Two passes.** The first counts the capture groups and collects their
  names, because a group can be referred to before it is defined. The
  second builds the program.
- **Every construct is classified** into one of three outcomes (section
  8): compiled exactly, refused as `unsupported`, or refused as `syntax`.
  The classification is the riskiest part of the design, which is why it
  is prototype 1 (section 13).

---

## 7. Values

### 7.1 `SJS_REGEXP`

A new value kind in `sjs_kind` (`staticjs.h:183-196`), with heap type
`SJS_HEAP_REGEXP` (`internal.h:147-158`). Placed before `SJS_FUNCTION`,
beside the other objects, it renumbers the kinds after it; appended at the
end, it renumbers nothing. Either way the public enum changes, so
`SJS_VERSION_MINOR` goes up (`docs/extending.md`, "To
add a value kind", step 1, which also lists every switch on kind that must
handle it).

The heap block holds the source text, the flags, the group count, the
group names, and the program.

| Operation | Result |
|---|---|
| `typeof re` | `"object"` |
| `re === re2` | fault, as for any two objects (`README.md:399`) |
| `re.source`, `re.flags`, `re.global`, `re.ignoreCase`, `re.multiline`, `re.dotAll`, `re.unicode`, `re.sticky`, `re.hasIndices`, `re.unicodeSets` | their values: exact, from the flags and the source |
| `re.lastIndex` | `0` (section 7.3) |
| other names of `RegExp.prototype` or `Object.prototype` read as values | fault, as for the other kinds' prototype names (`README.md:431-437`) |
| any other property | `undefined` |
| assignment to a property | refused or a fault, as for a set or a function (`README.md:443-449`) |
| as a map key, set member or computed key | fault, as for any object |
| writer | `/source/flags` |
| `JSON.stringify` | `{}` (a regex has no own enumerable property) *(inferred; check in Node)* |
| `String(re)`, `` `${re}` ``, `"" + re` | fault, as `+` of an object faults (`README.md:399-401`) |

`re.source` escapes a `/` and line terminators as `RegExp.prototype.source`
does *(inferred: the exact escaping must come from Node)*.

**A smaller first step** *(the review's suggestion; not decided)*: allow a
regex only as the receiver of its methods and as the first argument of the
string methods, or in a `const` used only there, and refuse it as
`unsupported` anywhere else. It could then be a value tagged `internal`,
as a method on the stack is (`SJS_INTERNAL_METHOD`, `vm.c:790`), with no
change to the public `sjs_kind`, `typeof`, the writer or `JSON.stringify`.
`String(re)` and `"" + re` give `"/a/"` in Node, which a full value kind
could give later instead of faulting.

A regex literal compiles to a constant. In JavaScript each evaluation of
the literal makes a new object; no staticjs program can tell, because `===`
on objects faults and `lastIndex` never changes where a program can see it
(section 7.3).

### 7.2 Match results

`exec` and `match` without `g` give `null` or an array with more properties
than an array has: `index`, `input`, `groups` and, with `d`, `indices`. A
plain staticjs array cannot hold them: reading a name that is not an index
or `length` from an array falls through to the prototype check
(`vm.c:156-170`), which would give `undefined` for `m.index`. That is a
different value, which the rule forbids.

**Proposal.** An array with an extra block of named properties: a flag on
the array's heap header and a pointer to an object of the extra keys.
`sjs_property_get`'s array branch consults it before the prototype check.

- Elements, `length`, spread, `Array.isArray`, destructuring by index and
  every array method see the array, as in JavaScript.
- `m.index`, `m.input`, `m.groups`, `m.indices` read the extra block.
- `groups` is an object with a null prototype in JavaScript, so
  `m.groups.toString` is `undefined` there. A staticjs object faults on
  that name ("staticjs objects have no prototype", `vm.c:153`). A fault is
  allowed; a different value is not.
- `Object.keys(m)`, `Object.entries(m)`, `{...m}` and an object rest of
  `m` fault: JavaScript includes `index`, `input` and `groups`, in an order
  this design does not promise to match.
- `JSON.stringify(m)` writes the elements only, as JavaScript does.
- The writer writes a match result as an expression that gives an equal
  value *(proposed: `Object.assign([...], {index: ..., input: ...,
  groups: ...})`; check how `diff-node`'s `show()` should print it,
  `tests/diff-node.mjs:22-36`)*.
- The extra block must be dropped by any operation that makes a new array
  (`slice`, `map`, a spread), and a copy-on-write path that takes over a
  receiver (`docs/builtins.md` section 4) must keep or drop it as
  JavaScript does: `concat` and `with` make a new array without the
  properties *(inferred)*.

Adding a flag to arrays touches the hottest value kind. The alternative is
a value kind of its own (`SJS_MATCH`) that every array path accepts as an
array. Prototype 3 decides which, by `bench-compare` on the `eval.array.*`
rows.

### 7.3 `lastIndex`

In JavaScript `lastIndex` is a writable own property, read and written by
the matching methods:

- `exec` and `test` with `g` or `y` read it, and set it to the end of the
  match, or to 0 when nothing matched.
- `replace` and `match` with `y` and without `g` do the same.
- `replace`, `match` and `replaceAll` with `g` set it to 0 first and leave
  it 0.
- `search` saves it, matches from 0, and restores it.
- `split` and `matchAll` construct a copy and leave the original alone.

*(The review checked every row of this list in Node 22.23.2, and each is
right.)*

staticjs values are never changed once shared (`README.md:38-44`). So:

- `lastIndex` is 0 in every regex staticjs makes. No program can assign to
  it (section 7.1).
- A call that would write `lastIndex` **faults**: `exec` and `test` with
  `g` or `y`, and `replace` and `match` with `y` and without `g` ("a
  regular expression with g or y changes when it matches").
- Every other call leaves `lastIndex` as it found it, or sets it to 0, and
  needs no check.

An earlier draft let such a call run on a regex nothing else holds
(`sjs_value_unique`), as an iterator is read. That condition can never
hold: the program's constants keep a reference to every constant
(`internal.h:282`), and `SJS_OP_CONSTANT` takes another before it pushes
one (`vm.c:1117-1122`), so a regex literal is never unique, even when it is
used once. A count of 2 would not be a safe test either: a regex imported
from another module is in constants too, and its count changes when that
module's program is released *(inferred)*. A plain fault is simpler and
can be made less strict later. In JavaScript
`function f(s) { return /a/g.test(s); }` returns `true` on every call,
because each evaluation makes a new object, so a fault here does refuse
programs that JavaScript runs well. It is a needless fault, not a wrong
value.

---

## 8. What is accepted, refused and faulted

The rule: a program staticjs accepts gives JavaScript's value or a fault
(`README.md:10-14`). A refusal when compiled is `syntax` only where
JavaScript refuses the program too; JavaScript that staticjs leaves out is
`unsupported` (`AGENTS.md`, "The rule comes first"; `diff-node` checks
both, `tests/diff-node.mjs:1-6`).

### 8.1 Accepted, and matched exactly

| Construct | Notes |
|---|---|
| characters, escapes `\n \r \t \v \f \0 \xHH \uHHHH`, `\u{...}` with `u` | |
| `.` | line terminators are `\n`, `\r`, U+2028, U+2029; `s` makes it match them |
| `\d \D \w \W` | ASCII; with `u` and `i`, `\w` also matches U+017F and U+212A, and `\W` matches neither; `\b` and `\B` with `iu` follow `\w` *(the review checked `\W` in Node; the rest is inferred)* |
| `\s \S` | a fixed list: white space and line terminators, as `String.prototype.trim` uses *(inferred: the list must be checked against Node)* |
| classes `[...]`, ranges, negation | |
| `^ $` with and without `m`, `\b \B` | |
| `* + ? {n} {n,} {n,m}` and their lazy forms | `{n,m}` up to the program limit (section 9) |
| groups `(...)`, `(?:...)`, named `(?<name>...)` | |
| alternation | |
| flags `g i m s u y d` | `i` as in 8.2 |

**Positions.** Without `u` or `v`, a match steps over UTF-16 code units;
with `u`, over code points. staticjs strings are UTF-8 with an ASCII flag
and a UTF-16 length (`internal.h:172-184`). A step keeps a byte offset, a
flag for the second half of a pair, and a UTF-16 index, so `.` without `u`
can consume half of a character outside the BMP: `/^.{2}$/.test("😀")`
must be `true` *(inferred; Node decides)*. On an ASCII string a byte is a
code unit, and the step is a byte. Every index the methods report
(`m.index`, `search`, `indices`, `lastIndex`) is in UTF-16 code units.

### 8.2 Case-insensitive matching (`i`)

**Accepted** when every letter in the pattern, in characters and in class
ranges, is ASCII:

- Without `u`, Canonicalize maps a character to its uppercase, but never a
  non-ASCII character to an ASCII one. So ASCII letters match their ASCII
  case pairs only, and comparing non-ASCII characters exactly is right
  *(inferred from ECMA-262's Canonicalize)*.
- With `u`, simple case folding also folds U+212A (KELVIN SIGN) with `k`
  and U+017F (LATIN SMALL LETTER LONG S) with `s`. Those are the only
  non-ASCII characters whose fold is an ASCII letter *(inferred; check
  with a sweep of every code point in Node)*.

**Refused** as `unsupported`: a non-ASCII letter in a pattern with `i`,
until staticjs has case tables. It has none today: `toLowerCase` of a
string that is not ASCII faults (`README.md:450`).

### 8.3 Refused as `unsupported`

| Construct | Why |
|---|---|
| backreferences `\1`..`\9...`, `\k<name>` | not regular: no linear-time automaton matches them |
| lookahead `(?=...)`, `(?!...)`, lookbehind `(?<=...)`, `(?<!...)` | not in the first version; captureless lookaround can be added in linear time (section 12.2) |
| `\p{...}`, `\P{...}` | need Unicode property tables |
| the `v` flag | its set operations and strings in classes need the same tables |
| a non-ASCII letter with `i` | section 8.2 |
| `\digit` other than `\0` without `u` or `v` | in Annex B it is a backreference or a legacy octal escape, depending on the group count; refused either way |
| Annex B syntax Node accepts without `u`: a lone `]`, `{` or `}`, `\c` outside its standard form, an identity escape such as `\-`, a quantified lookahead, `\u{41}` (the letter u 41 times), `\p{L}` (the text `p{L}`), `[\d-z]`, `\08`, `\k` in a pattern with no named group | staticjs does not implement Annex B's meanings; they are valid JavaScript, so they are never `syntax` (the review checked each in Node) |
| `[]` and `[^]` | they match nothing and any character; valid, but rare. Accept them if they are cheap, else `unsupported` |
| `new RegExp(...)`, `RegExp(...)` | patterns made at run time; a later step (section 12.4) |

`RegExp` as a bare name would otherwise be refused as bound nowhere, and
`diff-node` requires that such a program throw in JavaScript too
(`diff-node.mjs:1-6`). `new RegExp("a")` does not throw, so the compiler
must refuse it as `unsupported` by name.

### 8.4 Refused as `syntax`

Only what Node 22 refuses when it parses: an unterminated literal or
class, an invalid flag or a repeated one, a quantifier with nothing to
repeat, `{n,m}` with n greater than m, a range out of order, an invalid
escape with `u`, a reference to a group name that does not exist with `u`
or with any named group, a duplicate group name. The review checked these
in Node as SyntaxErrors, and they belong in the spec: `/{1}/`,
`/x{1}{2}/`, `/^*/`, `/\b+/`, `/(?<=a)*/`.

Which constructs Node 22 refuses depends on its V8 version. Duplicate
named groups in different alternatives and inline modifiers (`(?i:...)`)
arrived in V8 after the one Node 22 ships, so Node 22 refuses them, and
staticjs must refuse them as `syntax` too *(inferred; `diff-node` is the
check)*. Wherever Node 22 accepts something this design does not
implement, the refusal is `unsupported`.

### 8.5 Faults when it runs

| Situation | Fault |
|---|---|
| a match, capture or `split` piece that begins or ends inside a surrogate pair | staticjs strings cannot hold a lone surrogate, as for `slice` (`README.md:450-457`) |
| `exec` or `test` with `g` or `y`; `replace` or `match` with `y` and without `g` | section 7.3 |
| a `split` or a `replace` with an empty match, without `u`, that would put text between the halves of a pair (`"😀".split(/(?:)/)`) | a lone surrogate, as `replaceAll` with an empty pattern faults today (`README.md:454-455`) |
| a non-string argument where JavaScript converts it (`re.test(1)`), or a split limit that is a string or an object (`s.split(re, "2")`) | as other string methods fault on a search argument that is not a string (`README.md:458-466`) |
| `replace` with a function that returns an object or a BigInt | as `+` of an object faults *(proposed)*. This is a new rule: `replaceAll` today faults on any function as the replacement (`builtins.c:2545`) |
| property reads of `m.groups` that JavaScript finds nowhere but staticjs's objects treat as prototype names | section 7.2 |
| exceeding a limit | a limit error (section 9) |

### 8.6 The methods

| Method | Kind | Notes |
|---|---|---|
| `re.test(s)` | loop | a boolean; no match result to build |
| `re.exec(s)` | loop | `null` or a match result |
| `s.match(re)` | loop | without `g` as `exec`; with `g` an array of the matched strings, or `null` |
| `s.matchAll(re)` | loop | requires `g` (JavaScript throws without it); gives an `SJS_ITERATOR` over match results, read once as iterators are |
| `s.search(re)` | loop | the index or -1 |
| `s.replace(re, x)`, `s.replaceAll(re, x)` | loop | `replaceAll` requires `g`; a string replacement with `$&`, `` $` ``, `$'`, `$$`, `$n`, `$<name>`, sharing `substitute` (`builtins.c:2536-2568`); a function replacement is a callback, called once per match |
| `s.split(re, limit)` | loop | captures spliced into the result, as JavaScript does |
| `s.replace(string, x)` | leaf | `replace` with a string is missing today, as `NAMES` gives it no ID (`builtins.c:149`); it comes with this work |

`match`, `matchAll`, `replace` and `search` are already in `NAMES` without
an ID (`builtins.c:137-153`); each gets one in the string group, with the
group's bound moved if it is last (`docs/builtins.md`, section 2). The
regex methods form a new receiver column.

A function replacement makes `replace` a loop that both steps the matcher
(`LOOP_AGAIN`) and calls back (`LOOP_CALL`). The loop frame's `phase`
field already tells a step that receives a callback's result from one that
does not (`builtins.c:1904-1959`).

**Two things in the loop machinery must change (verified).**

- **Leaf or loop is decided by ID, before the arguments are seen.**
  `call()` asks `sjs_builtin_loops(id)` (`vm.c:792`), and `split` and
  `replaceAll` are leaves today (`builtins.c:161`, `2539`). With a regex
  they must be loops. The options are: test the first argument's kind in
  `call()`, which is on every call's path and must be measured; let the
  leaf hand off to a loop frame; or make every call of these methods a
  loop, which slows the string forms. This document proposes the second,
  with the choice confirmed by `bench-compare` in prototype 3.
- **Where the callback is.** A loop calls back through the slot
  `LOOP_CALLBACK` (`vm.c:851`, `878`), and `sjs_loop_start` faults when
  that slot is not callable (`builtins.c:1523`, `1537-1541`), and the
  array loops read their receiver as an array *(the review's reading; not
  rechecked here)*. In `s.replace(re, fn)` the slot holds `re`,
  and `fn` is in `LOOP_SECOND`. The regex loops must branch off before
  those checks, and swap the two slots as `call()` does for `groupBy`
  (`vm.c:813-815`).

`split` with a regex tries each position in turn with a sticky match
(ECMA-262's `@@split`). Searching forward from the current position finds
the same matches, provided an empty match at the position the last piece
ended is skipped by moving one step on *(inferred; the random sweep must
cover empty matches in `split`)*.

---

## 9. Fuel and limits

### 9.1 Fuel

`docs/builtins.md` section 6: charge the work done, before or as it is
done, in 64 bits, never less than the work; an optimization only lowers
fuel.

| Work | Charge *(proposed)* |
|---|---|
| a loop step | the run loop's 1 unit (`vm.c:1623`) |
| matching | 1 per 8 instructions visited, the epsilon closure included, plus 1 per 16 capture slots copied when a thread forks |
| building a match result | 1 per element, plus the string bytes / 16, as string methods charge |
| a replacement | as `replaceAll` charges today |

Counting the instructions visited, rather than the threads run, matters:
an epsilon closure can visit up to m instructions at one position, and a
fork copies s slots. Making captures copy-on-write would lower the second
charge; it is an optimization for later. The figures are pinned in
`api_test`, as `test_fuel_pinned` and `test_shape_paths` pin others
(`docs/builtins.md`, section 6).

**How much a step does.** A step runs until it has visited a budget of
instructions (256 *(estimate)*), finishing the position it is at, and
charges what it did. One position per step would pay a whole iteration of
`sjs_vm_run` for each: the fuel check, the frame, the out-of-line
`sjs_loop_step`, its branches and the call into the regex step. For small
patterns that is about as much as the matching itself *(estimate)*, and
more once a prefilter or a DFA makes positions cheap. A bounded budget
keeps a paused evaluation pausing within a match.

**The leaf path.** Most guards are short patterns over short strings. When
m × n is below a bound known before the match starts, the match runs as a
leaf in one C call, its state in a fixed buffer in the VM, with no frame
and no allocation, charging the same fuel afterwards (leaves charge after
their work: `vm.c:840`). This is the default from the start, not a later
optimization. `spec_test` needs a switch that turns it off, so that every
case runs both ways and the outcomes and fuel agree, as with shapes.

Compiling a pattern happens in the compiler, which is bounded by the
source's size and is not fuelled; the limits (below) bound the work.

### 9.2 Why the bound holds

The automaton holds at most m × 2^d threads per position (section 5.3),
each visited a bounded number of times. With m instructions, s slots per
thread and n positions, the time is O(n × m × 2^d × s) and the memory
O(m × 2^d × s), whatever the input. d and s are fixed when the pattern
compiles. `/(a+)+$/` on `"a".repeat(n) + "b"` costs time linear in n.

### 9.3 Limits

`sjs_limits` (`staticjs.h:127-144`) gains *(proposed)*:

| Limit | Default *(estimate)* | Bounds |
|---|---|---|
| `max_regex_states` | 1,048,576 | m × 2^d × s: the threads and their slots one match can hold. Computed when the pattern compiles, in saturating arithmetic, **before** `{n,m}` is expanded, so `/a{100000}/` and `(a?)` repeated 10,000 times are limit errors when compiled, not gigabytes of thread lists |

`max_value_items` does not bound the matcher's own state: it counts the
elements of values. Without a limit on m × s, a pattern of many small
groups (`(a?)` repeated 10,000 times: m about 50,000, s about 20,000) would
need thread lists of gigabytes *(review's estimate)*. A field added to
`sjs_limits` changes the public header (`SJS_VERSION_MINOR`). A limit
reached is `SJS_ERROR_LIMIT`, as `frames_full` reports one
(`vm.c:713-718`).

---

## 10. Shapes

The matcher (`docs/matcher.md`) needs only the new event declared in its
event language (section 6.3). Two optional later uses:

- **A predicate shape**, `REGEX_TEST`, so that `filter`, `some`,
  `every`, `find` and their kin run the matcher without the callback's
  bytecode. It follows `docs/builtins.md` section 9: enter the callback
  where the bytecode would, check each precondition each step (the element
  is a string), fall back to the bytecode otherwise, and keep the stack
  room.
  - `x => /lit/.test(x)` and `x => /lit/.test(x.key)` fit the rule
    language today, once the regex event carries an operand
    (`operand: operand`) and there is an `SJS_OPERAND_REGEXP` kind. The
    operand encoding has room for it, and already stores a constant's
    index, as it does for text (`internal.h:841-851`).
  - The common guard, `x => re.test(x)` with `re` a `const` outside the
    callback, needs a name from outside as an operand. That is
    `docs/matcher.md` 11.9.3, designed and not implemented: `NAME` has no
    operand field in `src/shapes.rules`.
  - `docs/matcher.md` 4.3 says rules on `open N` events are refused, but
    `src/shapes.rules` has rules on `CALL_BEGIN` *(the review's reading;
    not rechecked here)*. Settle which applies to callback shapes before
    writing these rules.
- **Pure literals are not a shape.** A pattern with no metacharacters and
  no `i` or `y` flag can be run as a byte search (`find_bytes`, as
  `replaceAll` does): `test` as `includes`, `search` as `indexOf`, `split`
  and `replace` as their string forms, and `^literal` as `startsWith`.
  That is decided when the pattern compiles, inside the regex value, not
  by the matcher.
- **No rewrite** is needed: a regex changes no form's meaning.

---

## 11. Testing

### 11.1 Spec cases

A new `tests/spec/regexp.spec`, in the format at the top of
`tests/spec_test.c:25-43`:

- values for every construct of section 8.1 and every method of 8.6,
  generated in Node, never typed by hand (`docs/extending.md`, "To write a
  spec case", step 2);
- the two ECMAScript rules (section 5): `/(?:(a)|b)+/.exec("ab")`,
  `/(a*)*/.exec("b")`, `/(a*)+/.exec("b")`, `/(?:a|())*?/.exec("a")`;
  the empty check on `?` and optional copies, `/(a*)?/.exec("b")`,
  `/(a*){0,3}/.exec("b")`, `/(a*){1,2}/.exec("b")`; the minimum count,
  `/(a?){2,}/.exec("ab")` (`["a", ""]`); and the thread-dropping case of
  section 5.3, `/(?:a*?|aa)*/.exec("aa")` (`["aa"]`). These values were
  checked in Node 22.23.2 by the review; generate them again for the file;
- positions without `u` inside pairs: `.` over a character outside the BMP,
  `m.index` after one, a capture that would split one (a fault);
- `i` with `k`/`s` with and without `u`;
- `lastIndex`: `exec` and `test` with `g` or `y` (faults); `replace`,
  `match`, `search`, `split` with `g` (values);
- `split` edges: `"".split(/a*/)` gives `[]` but `"".split(/x/)` gives
  `[""]`; an empty match in a pair without `u` (a fault);
- `unsupported` cases for each row of 8.3, and `syntax` cases for each of
  8.4;
- deliberate faults where Node gives a value, as fault cases
  (`docs/builtins.md`, section 10).

`spec_test` runs each case with the source pushed whole, a byte at a time
and in chunks, on one unit of fuel per run in the byte mode, and once more
with shapes off (`tests/spec_test.c:1-23`). So every case also tests
lexing a regex across chunks and pausing within a match. `--full` fails
each allocation in turn, which covers the compiler's and the match's
allocations.

### 11.2 `diff-node`

`mise run diff-node` checks every value case, and the class of every
refusal, against Node 22 (`diff-node.mjs:1-6`). It does not compare fault
cases (`diff-node.mjs:319`). `show()` needs a branch for a regex
(`String(v)`) and one for a match result (`diff-node.mjs:22-36`).

### 11.3 The random sweep

A new test program, `tests/regex_test.c`, with `--print N`, as
`number_test` and `json_test` feed `diff-node` (`README.md:16-34`;
`diff-node.mjs:256-310`). It writes lines of a new letter:

```
R <pattern> <flags> <input> <outcome>
```

where the outcome is the match as `JSON.stringify` of
`[m.index, ...m, m.groups]`, or `null`, or a refusal class. Node evaluates
`new RegExp(pattern, flags).exec(input)` and compares. Each refusal is
checked as the spec's are: `syntax` must throw a SyntaxError in Node;
`unsupported` must not.

- **Patterns** come from a grammar over a small alphabet that reaches every
  edge: `a`, `b`, `é`, `😀`, `\n`, classes over them, every quantifier,
  nested groups, named groups, alternation, anchors, every flag
  combination.
- **Inputs** are short strings over the same alphabet.
- 100,000 lines per test run, and a million once before a commit, as for
  numbers (`docs/extending.md`, "To make number results bit-identical",
  step 4).
- The same generator, with refusals counted separately, measures how
  often real-looking patterns are refused.

### 11.4 A JavaScript prototype, before the C

Before the C matcher, a Pike VM written in JavaScript, following sections
4 and 5 instruction for instruction, is compared with Node's `RegExp` on
random patterns and inputs. Run inside Node, it compares millions of cases
in seconds. The review's prototype of the first draft found the
thread-dropping error of section 5.3 and the missing check on `?` this way,
where reading the specification had not.

It replaces the in-C backtracking oracle an earlier draft proposed: an
oracle written from the specification by the same people can share their
misreadings, and Node cannot. The C matcher is then checked against Node
through the random sweep (11.3). (`check-recursion` covers `src/` only, so
a recursive oracle in `tests/` would have been allowed, as the review
confirmed.)

### 11.5 `api_test`

- pinned fuel for `test`, `exec`, `replace` and `split` at a few sizes;
- a pause after every unit of fuel during a match, resumed to the same
  result;
- the program limit, and a match that fills `max_value_items`;
- out of memory during compile and during a match.

---

## 12. Benchmarks, and what comes later

### 12.1 Benchmark plan

New cases in `bench/bench.c`, named, sized and checked against Node as
`AGENTS.md` ("Benchmarks") requires:

| Case | Unit | Sizes | Measures |
|---|---|---|---|
| `compile.regex` | MB/s | a module of typical patterns | lexing and compiling patterns |
| `eval.regex.test` | element (character) | L | a guard-style `test` over strings of growing size |
| `eval.regex.exec` | element | L | captures and building a match result |
| `eval.regex.replace` | element | L | `replace` with `g` and `$1` |
| `eval.regex.split` | element | L | `split` on a class |
| `eval.regex.redos` | element | L | `/(a+)+$/` on `"a".repeat(n) + "b"`: linear here, which is the guarantee |
| `eval.regex.filter` | element | L | `filter(x => re.test(x.k))`, for the predicate shape later |

Before committing each step, `mise run bench-compare <commit before> --quick`,
then without `--quick`, and check the rows the change should not touch,
`eval.guard` and the array loops at least (`docs/extending.md`, "To
measure a change"). The new branch in `sjs_loop_step` is the most likely
cause of a regression there (`docs/builtins.md` section 10).

*(Estimate)*: before any prefilter or DFA, a Pike VM step over an ASCII
input should cost tens of nanoseconds per character for small patterns,
several times a backtracker's cost on easy patterns. The first
optimizations, in order of expected value:

1. a literal prefix: skip to the next place it occurs with `find_bytes`
   (`builtins.c`), as `replaceAll` does;
2. an anchored or one-pass program: a single thread, no slot copies;
3. a lazy DFA for `test` and `search` (12.3).

### 12.2 Lookaround

Lookahead and lookbehind without captures inside them can be matched in
linear time by matching each lookaround once over the whole input and
recording where it holds (Barrière and Pit-Claudel, PLDI 2024 *(inferred:
cited from memory)*). That is a later step with its own spec cases and its
own sweep; until then they are `unsupported`.

### 12.3 A lazy DFA

For `test` and `search`, a DFA built on demand from the program answers
"is there a match, and where does it end" without capture slots, in a
cache with a fixed budget, cleared when full. The cache lives in the
evaluator, not in the regex value (section 3.2). Add it only when the
`eval.regex.test` row asks for it.

Built for `test` (section 15 says how): its cache is in the match's own
state rather than the evaluator, and a full one hands the search back to
the automaton rather than being cleared.

### 12.4 `new RegExp`

A pattern made at run time compiles with the same `sjs_regex_compile`, in
a leaf charged in proportion to the pattern's length. A pattern that
JavaScript refuses is a fault (a SyntaxError in JavaScript), and one this
design does not implement is a fault too, as there is no compile step left
to refuse it in.

### 12.5 Unicode tables

`\p{...}`, the `v` flag, and `i` with non-ASCII letters need tables of
general categories, scripts and case folding: tens of kilobytes of
constant data *(estimate)*. They would also let `toLowerCase` and
`toUpperCase` accept any string. That is a project of its own.

---

## 13. Implementation plan

Small commits, each passing `mise run test`, scoped `feat(staticjs)` or
`perf(staticjs)` (`docs/extending.md`, "To commit"). The order follows the
risks: refusing in the wrong class is the error the rule punishes most,
so it comes before any matching.

### 1. Lexing, parsing and classification (no matching)

- `L_REGEX` in the lexer, the parser's operand-position `/` and the scans
  (section 6), `SJS_EV_REGEXP`, its rule in `src/shapes.rules`.
- `src/regex.c`'s pattern parser, classifying every construct (section 8)
  and compiling nothing yet.
- `SJS_REGEXP` with `source`, `flags` and the flag getters, and the
  writer.
- Spec cases for each refusal class; a pattern generator that prints
  `R <pattern> <flags> - <class>` lines for `diff-node`, with Annex B
  forms and Node 22's edges (duplicate names, modifiers, `v`) in it.

Done when `diff-node` agrees on the class of every generated pattern.

### 2. The Pike VM, for `test`, `search`, `split` and `replace`

A first version can leave out `exec` and `match` without `g`: the methods
above give strings, numbers and booleans, so the match result's
representation (section 7.2), its writer and `diff-node`'s `show()` can
wait. `match` with `g` gives an array of strings and can come here too.

- The instruction set of section 4, with `CLEAR`, `MARK` and
  `CHECK_PROGRESS` (section 5).
- The step as a loop frame with `LOOP_AGAIN`, fuel charged as section 9
  says, and pinned in `api_test`.
- Positions without `u` inside pairs; `i` (section 8.2).
- Before the C, the JavaScript prototype of section 11.4, compared with
  Node until it agrees on 1,000,000 cases.
- `tests/regex_test.c --print` against Node.

Done when 1,000,000 random cases agree with Node, and the cost per
character is measured, with the leaf path and without.

### 3. Match results, the other methods, and the benchmarks

- The match result's representation (section 7.2), chosen by
  `bench-compare` on the `eval.array.*` rows; `exec`, `match` without `g`,
  `matchAll`, `replace` with a function, and the faults of section 7.3.
- How `split` and `replaceAll` become loops with a regex (section 8.6),
  measured.
- The bench cases of section 12.1, with `bench-compare` showing no change
  past the noise on `eval.guard` and the loops, or showing that a change
  is placement (`docs/extending.md`, "To tell a real slowdown from code
  placement").
- `README.md`: the regex section, the built-ins table, "Where it faults",
  and "Refused when compiled"; `src/README.md`: `regex.c`;
  `docs/builtins.md`: the new loops.

---

## 14. What is verified and what is not

**Verified in the code** (every citation above with a line number): the
current refusals and their spec cases; the lexer's synchronous handler and
state reset; how `/` and `/=` are held; the forward-only jumps and the
last-read moves that depend on them; `execute()`'s forced inlining; the
value's size and `max_stack`; that a closure cannot be re-entered; the loop
frame, `LOOP_AGAIN`, its fuel and fault position, its size assertion and
how it frees `scratch`; that program constants are values; how strings are
stored; that a plain array gives `undefined` for `m.index`; the existing
`substitute` and the `NAMES` entries without IDs; what `spec_test` and
`diff-node` check.

**Checked in Node 22.23.2 by the adversarial review** (its prototypes are
not in the repository): the counterexample of section 5.3 and the rule
that fixes it, on 1,000,000 random cases; the empty check on `?` and
optional copies (5.2); every row of the `lastIndex` list (7.3);
`JSON.stringify(/a/)` is `"{}"`; `/^.{2}$/.test("😀")` is `true`; that
without `u` no non-ASCII character folds to an ASCII letter, and with `u`
only U+017F and U+212A do (every code point swept); that `\s` matches what
`trim` removes; that Node 22 refuses duplicate named groups and modifiers
and accepts `v`; the Annex B forms and SyntaxErrors listed in 8.3 and 8.4;
the examples of 5.1 and 5.2.

**Inferred, not verified:**

- that the lexer takes a regex body with no byte consumed past `/=`, given
  the change at `lexer.c:267` (read, not run); and that the scans can
  decide about a `/` within the handler call that delivers it;
- the statements about ECMA-262's text that the review did not test:
  `split`'s algorithm as a forward search, `source`'s escaping, `\b` with
  `iu`;
- that the m × 2^d bound of section 5.3 is the best available, and the
  papers cited from memory (Cox 2009; Barrière and Pit-Claudel, PLDI 2024);
- that comparing reference counts cannot make the `lastIndex` rule safe
  for imported regexes;
- that the array loops read their receiver as an array, and the
  `open N` rule question of section 10 (the review's readings);
- the expected values in sections 8 and 11 that the review did not run.

**Estimated:** every performance figure and default limit in sections 2,
9 and 12.

---

## 15. Where the implementation departs from this design

Each departure below was made because Node or the code said otherwise.

- **Classified by the parser, not the compiler (6.4).** The parser calls
  `sjs_regex_check` on each literal as it arrives. A syntax error in a
  pattern makes the whole program a SyntaxError in JavaScript, and the
  parser reads on after a refusal to find one (it sends no events once
  refusing, so the compiler would never see a later literal). An
  unsupported pattern is refused like any other construct staticjs leaves
  out, and read on. The compiler compiles only exact literals.
- **The decision about a `/` is made where the parser stands (6.1, 6.2).**
  `regex_starts` sets the lexer's `regex_next` while the `/` is the newest
  token queued, and the parser waits; the lexer's REGEXP token then takes
  the slash's place in the queue (`on_token`). Besides the operand
  position and the two scans, two lookaheads had to learn it: the first
  token of an expression statement (`statement_token` peeked past the
  slash) and a label's name (`labelled_statement`). A slash reached any
  other way is refused as unsupported ("cannot tell where this regular
  expression ends") rather than lexed wrongly; no spec case reaches it.
- **`\p{...}` with u or v (8.3, 8.4)** is unsupported only for a name Node
  22 accepts: `src/regex.c` keeps those names (filtered in Node 22.23.2 from
  `unicode-property-value-aliases-ecmascript` 3.0.0), and any other name is
  a syntax error, as in Node.
- **The v flag (8.3)** is unsupported, but its class syntax (nested
  classes, `&&`, `--`, `\q{...}`, the reserved punctuators, negated classes
  that may hold strings) is read, so that what Node refuses with v is a
  syntax error here too.
- **Group names (6.4)** of ASCII letters, digits, `$` and `_` are read
  exactly; a name with an escape or a character that is not ASCII is
  unsupported, as staticjs's own names are ASCII (the lexer refuses others
  the same way). Such a name also turns off the check that a `\k<name>`
  names a group, which is then refused as unsupported.
- **A quantifier's count** is clamped to 2^31 - 1 before `{n,m}` is
  checked for order, as V8 does: `/a{2147483648,2147483647}/` is valid in
  Node, `/a{4294967296,1}/` is not.
- **`(a?)` under a quantifier (9.3)** does not multiply the slots: its
  copies share one group number, so `/(a?){10000}/` costs instructions,
  not slots. Only a pattern that writes many groups has many slots.
- **The random sweep's tag (11.3)** is `X`, not `R`, which `number_test`
  already uses for BigInt rounding. Its lines for an exact pattern carry an
  input and the JSON of the methods' results on it (`search`, `replace`
  with and without g, `split`, `match` with g, `test`); a fault is checked
  to be one where Node makes a lone surrogate.
- **With u, V8 starts a search between the halves of a pair (8.1).** Node
  22 gives `"a😀".search(/\B/u)` as 2 and `"a😀".split(/\B/u)` cuts the
  pair: V8 tries every code unit as a search's start, even with u, where
  the halves are the characters around it for an assertion and nothing
  can be consumed (`"a😀".search(/\B./u)` is -1). The automaton does the
  same (`rx_mid_start` in `src/regex_match.c`): only an empty match can
  start there. `split` does so too, unlike ECMA-262's `@@split`, which
  steps by code point with u.
- **How a method becomes a loop (8.6, 9.1).** Every regular expression
  method is a leaf by its ID. One whose work (code units times threads,
  times code units again for g and `split`) is below 8,192, and whose
  state fits the evaluator's 8 KiB buffer, matches whole; any other returns
  `SJS_LEAF_LOOP`, and `call()` enters the loop of its operation on the same
  slots. Both charge one unit per step the loop takes (256 instructions
  visited, finishing the position) and the same work fuel, which
  `spec_test` checks on every case with a `/`, and `regex_test` on every
  random input. The loops are tested for in `sjs_loop_step` only where a
  loop ends, as a test at its top cost the predicate loops 5 to 13%.
- **`split` with a limit (8.5)** takes a number, a boolean, `null` or
  `undefined` (`ToUint32`, 2^32 - 1 for `undefined`: `sjs_split_limit` in
  `src/vm.h`), as JavaScript does, and stops once the result has that many
  elements, each capture counting as one; only a limit of another kind (a
  string, an object, a BigInt) faults. `split` with a string separator
  takes the same limits.
- **A replacement that is a primitive (8.6)** is its text, as `ToString`
  gives it (`"a".replace(/a/, 1)` is `"1"`); a BigInt or an object faults.
- **Match results (7.2)** are arrays with a flag (`SJS_ARRAY_EXTRA` in
  `src/internal.h`) and an object of their extra properties in the place
  of the field a set's array uses; no array path reads that field, so the
  flag costs only the checks named here. `sjs_property_get` reads the
  object after an index and `length`; `in` finds its keys. `Object.keys`,
  `Object.entries`, `Object.values`, an object spread or rest, and `sort`
  fault, with a message that names no built-in ("this array has properties
  besides its elements"): the first four would show the keys, and `sort`,
  which changes the array in place in JavaScript and keeps them, takes
  over or copies its receiver here. Paths that make a new array (`slice`,
  `map`, `concat`, an array spread, `Array.from`) copy the elements only,
  and a copy-on-write path never takes over such an array. `groups` holds
  the named groups in the order of the groups, as V8 does.
- **`matchAll` (8.6)** finds every match in its loop and gives an iterator
  over the array of their results, rather than matching as the iterator
  is read. The result is the same, as nothing can run between two reads
  that changes the string or the regex; the memory is all the results at
  once.
- **A replacement function (8.6)** always makes `replace` a loop: the leaf
  hands any call with a function to the loop before it matches, and the
  loop's first step swaps `LOOP_CALLBACK` and `LOOP_SECOND`. The function
  gets the match, each group (`undefined` for one that did not match), the
  index in code units, the input and, when any group has a name, the groups
  object. Its result is written as `ToString` gives it, without `$`
  patterns; an object, an array or a BigInt faults.
- **The literal-prefix skip (12.1, item 1) leaves fuel as it was.** Section
  9.1 lets an optimization lower fuel; here none does, so that fuel stays a
  property of the pattern and the input, not of the build. A program whose
  start is SAVEs then one ASCII character (with or without i) has, where no
  thread is alive, a start thread at each position that dies on that
  character. Over a run of ASCII bytes that cannot begin a match, the
  search passes the positions without following them, adds the
  instructions and slot copies following them would have counted, and
  stops where the budget of a step would have stopped it. It does not use
  `find_bytes`, and it compares one byte: a literal of several characters
  is skipped to its first.
- **Of 12.1's item 2, only the anchored start is built.** A program whose
  start is SAVEs then `^` without m has, at every position but the first,
  a start thread that dies at the `^`; only the start reaches those
  instructions, so the search counts that closure without following it,
  whatever other threads are alive, and with none alive passes runs of
  ASCII bytes as the literal-prefix skip does. A one-pass program run as
  a single thread, without slot copies, is not built: fuel counts the
  instructions and slot copies of the Pike VM itself, so a single-thread
  runner would still have to count every copy it no longer makes, and
  what it would save beyond the anchored start is the closure's stack and
  stamps for one thread, which no bench row isolates.
- **No predicate shape (10, 12.1) yet: the rules cannot express it.** Every
  form of it, `x => re.test(x)` and `x => /lit/.test(x)` alike, is a call:
  its pattern is a CALL_BEGIN bracket whose first child, the callee
  `re.test`, comes before the bracket opens. The generator refuses that
  (`mise/shapes.rb`: "takes the node(s) before it as its first children,
  which the matcher does not keep"; `docs/matcher.md` 4.3) for every rule
  but a rewrite's, whose anchored lane matches such brackets only because
  its rules bind nothing and hand nothing up, which a shape must (the
  element's name, the regex operand). This settles the question section
  10 left open: the rules on CALL_BEGIN in `src/shapes.rules` are a
  rewrite's (MUTATE_PATH), so the refusal applies to callback shapes. The
  shape needs `docs/matcher.md` 11.9.2 ([brackets]) first, then, for
  `re` read from outside, 11.9.3 ([names]), and an operand on the REGEXP
  event for a literal. Neither is built, so `eval.regex.filter` still
  calls its callback for each row.
- **A lazy DFA for `test` only (12.3), whose fuel is the automaton's.**
  `eval.regex.test` asked for it: the Pike VM spent about 550 instructions
  a character there. A `test` over an ASCII input that runs as a loop
  (not sticky, a program of at most 256 instructions and at most 32
  classes) keeps a cache of 8 KiB after its job, in the loop's block
  (`rx_dfa_of` in `src/regex_match.c`), set up the first time the search
  runs from it. The classes are sorted once, when the pattern compiles
  (`sjs_regex_dfa_classes`, which splits sets of 128 bits by each
  predicate's set), and the value keeps the class of each ASCII
  character, and its coarser class (below), packed in 16 bits as the
  rows read them, 256 bytes before its text (`rx_dfa_map`), or nothing when
  the program has more classes, whose tests then make no cache. Before,
  every test that ran as a loop allocated the cache and sorted the
  classes again, asking up to 259 predicates of each of 128 characters.
  Its result needs no slots, and a slot bears on what a thread does only
  through `CHECK` and the registers' mask, which compare a register with
  the position: a register equals the position only within the
  position's closure. So a list of threads after its position's start
  thread, as program counters, and whether a match is found, is a state;
  the step over a character with the next position's start thread is an
  edge, the same for every character class and class of the character
  after (each instruction that consumes, `\w`, the line terminators and
  the skip's leading character treat the characters of a class alike; at
  most 32 classes). Of the character after, only `\w`, the line
  terminators and the skip ask, so its class is a coarser one, of those
  predicates alone (`dfa_next`, at most 8, the end of the input one more):
  a state's row of edges is the classes by the coarser ones, which lets a
  pattern with more classes fit in the cache. An edge is
  learned by running the automaton itself once, and keeps what it visited
  and copied in the step and in the start, so fuel is the automaton's to
  the unit, whichever runs, and a step of the loop ends at the same
  place: where the budget runs out after an edge's step, the start is
  left for the next step (`pending`). An edge to a position the search
  skips hands the search back to the skip, and a full cache to the
  automaton. Positions, `^`, `$`, `\b`, the flags and empty iterations
  are the automaton's, as is everything but `test`. `spec_test` runs
  every case with a `/` as loops paused after each unit, with the cache
  and without (`sjs_vm_config`'s `no_regex_dfa`), and requires the same
  outcome, fuel and pauses; `regex_test` does so on its random patterns,
  and `api_test` on long inputs. On `eval.regex.test` it takes about 12
  times fewer instructions. Raising the classes from 16 to 32 with the
  coarser row took `eval.regex.test.many`'s second pattern
  (`a|b|...|q|0`: 25 classes, 4 coarser ones) from the automaton into the
  cache: about 5 times fewer instructions on the row.
