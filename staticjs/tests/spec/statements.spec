# Statements in an arrow function's block body: const, if/else, blocks and
# return anywhere. Every case also runs in Node (mise run diff-node).

# Values

=== the proposal's example
[0, 20, 5, 1].map((f => x => { if (!x) return null; const y = f(x); if (y > 10) { return "big"; } else if (y > 5) return "mid"; else { const z = y * 2; return z; } })(x => x + 1))
--- value
[null,"big","mid",4]

=== if conditions use truthiness
["", "a", 0, -0, NaN, 1, null, undefined, [], {}, new Set(), x => x, false, true].map(v => { if (v) return 1; else return 0; })
--- value
[0,1,0,0,0,1,0,0,1,1,1,1,0,1]

=== an if without else falls through
(x => { if (x) return 1; return 2; })(0)
--- value
2

=== falling off the end gives undefined
(x => { if (x) return 1; })(0)
--- value
undefined

=== an empty body gives undefined
(() => {})()
--- value
undefined

=== a dangling else binds to the nearest if
[[true, true], [true, false], [false, true]].map(([a, b]) => { if (a) if (b) return 1; else return 2; return 3; })
--- value
[1,2,3]

=== else on the line after a return value
(x => { if (x) return 1
else return 2 })(0)
--- value
2

=== return alone on its line before else
[1, 0].map(x => { if (x) return
else return 2 })
--- value
[undefined,2]

=== return on its own line before a block
(() => { return
{} })()
--- value
undefined

=== empty statements
(x => { ;; if (x) ; else return 1; if (x) {} else {} ; return 2 })(1)
--- value
2

=== an else-if chain without a final else
[0, 1, 2].map(x => { if (x === 0) return "zero"; else if (x === 1) return "one"; })
--- value
["zero","one",undefined]

=== an else-if chain whose branches fall through
[0, 1, 2].map(x => { if (x === 0) { const a = 1; } else if (x === 1) { const b = 2; } else { const c = 3; } return x * 10; })
--- value
[0,10,20]

=== an if in an else-if chain's branch
[[1, 1], [1, 0], [0, 1], [0, 0]].map(([a, b]) => { if (a) { if (b) return "ab"; return "a"; } else if (b) return "b"; else return "-"; })
--- value
["ab","a","b","-"]

=== a 40-arm else-if chain
(x => { if (x === 0) return 0; else if (x === 1) return 1; else if (x === 2) return 2; else if (x === 3) return 3; else if (x === 4) return 4; else if (x === 5) return 5; else if (x === 6) return 6; else if (x === 7) return 7; else if (x === 8) return 8; else if (x === 9) return 9; else if (x === 10) return 10; else if (x === 11) return 11; else if (x === 12) return 12; else if (x === 13) return 13; else if (x === 14) return 14; else if (x === 15) return 15; else if (x === 16) return 16; else if (x === 17) return 17; else if (x === 18) return 18; else if (x === 19) return 19; else if (x === 20) return 20; else if (x === 21) return 21; else if (x === 22) return 22; else if (x === 23) return 23; else if (x === 24) return 24; else if (x === 25) return 25; else if (x === 26) return 26; else if (x === 27) return 27; else if (x === 28) return 28; else if (x === 29) return 29; else if (x === 30) return 30; else if (x === 31) return 31; else if (x === 32) return 32; else if (x === 33) return 33; else if (x === 34) return 34; else if (x === 35) return 35; else if (x === 36) return 36; else if (x === 37) return 37; else if (x === 38) return 38; else if (x === 39) return 39; else return -1; })(39)
--- value
39

=== nested blocks
(x => { { const y = x + 1; { const z = y * 2; return z; } } })(1)
--- value
4

=== a block shadows a parameter
(x => { { const x = 2; return x; } })(1)
--- value
2

=== a block shadows a name read before it
(a => { const r = a; { const a = 5; return [r, a]; } })(1)
--- value
[1,5]

=== a name read in one block, declared in a later sibling
(y => { { const a = y; } { const y = 3; return y; } })(1)
--- value
3

=== a const after an inner block that declared the same name
(x => { { const y = 1; } const y = 2; return y; })(0)
--- value
2

=== the same name in both branches
[true, false].map(c => { if (c) { const v = "yes"; return v; } else { const v = "no"; return v; } })
--- value
["yes","no"]

=== slots are reused after a block
(o => { { const {p, ...rest} = o; } { const [q] = [o.p + 1]; return q; } })({p: 4, r: 1})
--- value
5

=== a computed key's slot is reused after a block
(o => { { const {[`p`]: a, ...r} = o; } const {["r"]: b, ...s} = o; return [b, s]; })({p: 4, r: 1})
--- value
[1,{"p":4}]

=== a closure captures a block's const
(x => { if (x > 0) { const k = x * 10; const f = y => y + k; return f(1); } return 0; })(2)
--- value
21

=== a closure leaves its block, and the next block reuses the slot
[true, false].map(c => (x => { if (x) { const a = 7; return () => a; } { const b = 9; return () => b; } })(c)())
--- value
[7,9]

=== captures through two functions from blocks
(x => { { const a = x; return (y => { { const b = y; return () => a + b; } })(2)(); } })(1)
--- value
3

=== a closure in a block captures a const of the body and one of the block
(x => { const a = x + 1; { const b = a * 2; const f = () => [a, b]; return f(); } })(1)
--- value
[2,4]

=== code after return is accepted and never runs
(x => { return 1; const y = 2; })(0)
--- value
1

=== code after an if whose branches both return
(x => { if (x) return 1; else return 2; return 3; })(0)
--- value
2

=== an if in a block whose branches both return
(x => { { if (x) return 1; else return 2; } })(0)
--- value
2

=== conditions with && ?: ?? and ?.
(o => { if (o?.a ?? false) return "a"; if (o && o.b ? true : false) return "b"; return "none"; })({b: 1})
--- value
"b"

=== a condition that is an arrow function
(() => { if (x => x) return "functions are truthy"; })()
--- value
"functions are truthy"

=== a condition that calls an arrow function with a block body
(x => { if ((y => { if (y) return false; return true; })(x)) return "no"; return "yes"; })(1)
--- value
"yes"

=== statements in an arrow in a const pattern's default
(o => { const {f = x => { if (x) return "t"; else return "f"; }} = o; return [f(1), f(0)]; })({})
--- value
["t","f"]

=== statements in an arrow in a parameter's default
((g = x => { if (x) { return 1; } return 2; }) => [g(0), g(1)])()
--- value
[2,1]

=== line breaks end consts and returns before if
(x => {
  if (x) return 1
  const y = 2
  if (y) return y
})(0)
--- value
2

=== a block may shadow a built-in namespace
(x => { { const Math = 1; return Math; } })(0)
--- value
1

=== a module whose functions use if
@module pick
export const pick = x => { if (x) return "a"; else return "b"; };
export const v = pick(0);
--- value
{"pick":<function>,"v":"b"}

=== falling off the end after a nested if
[0, 1, 2].map(x => { if (x) { if (x > 1) return 2; } })
--- value
[undefined,undefined,2]

=== falling off the end after an if in an else
[0, 1, 2].map(x => { if (x === 1) return 1; else { if (x === 2) return 2; } })
--- value
[undefined,1,2]

=== falling off the end after an if in a block
[0, 1, 2].map(x => { { if (x) return 1; } })
--- value
[undefined,1,1]

=== falling off the end after code after a return
[0, 1].map(x => { if (x) { return 1; const y = 2; } })
--- value
[undefined,1]

=== a closure made before a block that shadows its name
(a => { const g = () => a; { const a = 2; return [g(), a]; } })(1)
--- value
[1,2]

=== a block's const is gone after it, and the global is seen again
@global g = 5
(x => { { const g = 1; } return g; })(0)
--- value
5

=== a name that starts with if
(x => { const iffy = 1; if (iffy) return iffy; })(0)
--- value
1

=== keywords without spaces around them
(x => { if(x)return 1;else{return 2} })(0)
--- value
2

=== a property named if, called and divided in a parameter's default
((o = {if: x => x}, a = () => o.if(4) / 2) => a())()
--- value
2

=== a property named if divided twice, read ahead
((o = {if: x => x}, a = () => { return o.if(4) /2/ 1 }) => a())()
--- value
2

=== a property named while, read ahead
((o = {while: x => x}, a = () => { return o.while(4) /2/ 1 }) => a())()
--- value
2

=== a property named catch, read ahead
((p = {catch: x => x}, a = () => p.catch(8) / 2) => a())()
--- value
4

=== a property named if after ?., read ahead
((o = {if: x => x}, a = () => { return o?.if(4) /2/ 1 }) => a())()
--- value
2

# Errors: syntax

=== a const as the body of an if
(x => { if (x) const y = 1; return 0; })
--- error syntax lexical declaration

=== a const as the body of an else
(x => { if (x) return 1; else const y = 2; })
--- error syntax lexical declaration

=== let as the body of an if
(x => { if (x) let y = 1; })
--- error syntax lexical declaration

=== let and [ on the next line as the body of an if
(x => { if (x) let
[a] = x; })
--- error syntax lexical declaration

=== a class as the body of an if
(x => { if (x) class C {} })
--- error syntax declaration

=== an async function as the body of an if
(x => { if (x) async function f() {} })
--- error syntax declaration

=== a generator as the body of an if
(x => { if (x) function* g() {} })
--- error syntax declaration

=== a labelled function as the body of an if
(x => { if (x) l: function f() {} })
--- error syntax declaration

=== a function as the body of an if in a module
@module e
export default x => { if (x) function f() {} }
--- error syntax declaration

=== a labelled function in a module
@module e
export default x => { l: function f() {} }
--- error syntax declaration

=== a labelled const
(x => { l: const y = 1; })
--- error syntax lexical declaration

=== if without parentheses
(x => { if x return 1; })
--- error syntax expected ( after if

=== if without its closing parenthesis
(x => { if (x return 1; })
--- error syntax expected )

=== if with an empty condition
(x => { if () return 1; })
--- error syntax expected an expression

=== if with no statement
(x => { if (x) })
--- error syntax expected a statement

=== else with no statement
(x => { if (x) return 1; else })
--- error syntax expected a statement

=== else without if
(x => { else return 1; })
--- error syntax expected a statement

=== else after a return value on the same line
(x => { if (x) return 1 else return 2 })
--- error syntax expected an operator

=== continue in an if
(x => { if (x) continue; })
--- error syntax expected a statement

=== break in a block
(x => { { break; } })
--- error syntax expected a statement

=== a name declared twice in a block
(x => { { const a = 1; const a = 2; } })
--- error syntax declared twice

=== a block that does not end
(x => { if (x) { return 1; }
--- error syntax ends early

=== if in an expression
if (1) 2
--- error syntax expected an expression

=== a syntax error after an expression statement
(x => { a b; })
--- error syntax expected an operator

=== a syntax error after an unsupported statement keeps reading
(x => { x; return 1 2; })
--- error syntax expected an operator

=== return in an if at a module's top level
@module e
if (a) return 1;
--- error syntax only in a function

=== export in a block at a module's top level
@module e
if (a) { export const b = 1; }
--- error syntax export

=== import in a block
(x => { { import y from "m"; } })
--- error syntax import declares only at the top level

=== a class with no name as a statement
(x => { class {} })
--- error syntax expected a statement

=== an empty statement between an if's block and else
(x => { if (x) {}; else return 1; })
--- error syntax expected a statement

=== two semicolons between a return and else
(x => { if (x) return 1;; else return 2; })
--- error syntax expected a statement

=== else after else
(x => { if (x) return 1; else return 2; else return 3; })
--- error syntax expected a statement

=== a block between an if and else
(x => { if (x) return 1; { } else return 2; })
--- error syntax expected a statement

=== else if without its condition
(x => { if (x) return 1; else if })
--- error syntax expected ( after if

=== let as the body of an if in a module
@module e
export default x => { if (x) let; return 0; }
--- error syntax lexical declaration

=== a label inside one of the same name
(() => { l: l: return 1; })()
--- error syntax label

=== a label inside one of the same name, in an if
(() => { if (1) l: l: return 1; })()
--- error syntax label

=== a label in a block inside one of the same name
(() => { l: { l: return 1 } })()
--- error syntax label

=== a label in blocks inside one of the same name
(() => { a: { b: { a: return 1 } } })()
--- error syntax label

=== a label inside one of the same name in a module
@module e
export const f = () => { if (1) l: l: return 1 };
--- error syntax label

=== a label inside one of the same name, around an if
(() => { l: if (1) l: return 1; })
--- error syntax label

=== break to a label around the function
(() => { l: { const f = () => { break l; }; } })
--- error syntax break

=== break to a label that has ended
(() => { l: { } break l; })
--- error syntax break

=== break to a label that is not open
(() => { l: { m: { break n; } } })()
--- error syntax break

=== break to a label with something after it
(() => { l: { break l 1; } })
--- error syntax expected ; after break

=== continue to a label
(() => { l: { continue l; } })
--- error syntax expected a statement

=== a label with no statement
(() => { l: })
--- error syntax expected a statement

=== a syntax error in a label's statement
(() => { l: return 1 2; })
--- error syntax expected an operator

# Errors: unsupported

=== while
(x => { while (x) {} return 1; })
--- error unsupported no loops

=== for
(x => { for (;;) {} })
--- error unsupported no loops

=== do while
(x => { do {} while (x) })
--- error unsupported no loops

=== switch
(x => { switch (x) {} })
--- error unsupported no switch

=== throw
(x => { throw x; })
--- error unsupported no exceptions

=== try
(x => { try {} catch {} })
--- error unsupported no exceptions

=== var
(x => { var y = 1; })
--- error unsupported declares with const

=== a function declaration in a block
(x => { function f() {} return 1; })
--- error unsupported function or class declarations

=== a class declaration in a block
(x => { class C {} return 1; })
--- error unsupported function or class declarations

=== a label
(x => { a: return 1; })
--- error unsupported no labels

=== a label as the body of an if
(x => { if (x) l: return 1; })
--- error unsupported no labels

=== a labelled function in a script's block
(x => { l: function f() {} })
--- error unsupported no labels

=== the same label on two statements
(() => { l: return 1; l: return 2; })()
--- error unsupported no labels

=== the same label in a function inside it
(() => { l: { const f = () => { l: return 1; }; } return 1; })()
--- error unsupported no labels

=== break to the label around it
(() => { l: { if (1) break l; } return 1; })()
--- error unsupported no labels

=== break to the label around it, through a block
(() => { l: { m: { break l; } } return 1; })()
--- error unsupported no labels

=== break as a label's statement
(() => { l: break l; })()
--- error unsupported no labels

=== break to a label, and a line break
(() => { l: { break l
1 } })
--- error unsupported no labels

=== two labels
(() => { l: m: return 1; })()
--- error unsupported no labels

=== an expression statement
(x => { x; return 1; })
--- error unsupported expression statement

=== an expression statement as the body of an if
(x => { if (x) x; return 1; })
--- error unsupported expression statement

=== let and a name on the next line as the body of an if
(x => { if (x) let
y = 1; return 0; })
--- error unsupported does not have this

=== let alone as the body of an if
(x => { if (x) let; return 0; })
--- error unsupported does not have this

=== a name that starts with else
(x => { if (x) return 1; elsewhere; })
--- error unsupported expression statement

=== a name that starts with if
(x => { iffy; })
--- error unsupported expression statement

=== a function as the body of an if in a script
(x => { if (x) function f() {} })
--- error unsupported does not have this

=== let as an expression statement before a block
(x => { if (x) let
{} })
--- error unsupported does not have this

=== a comma in a condition
(x => { if (x, 1) return 1; })
--- error unsupported comma

=== a regular expression after an if's condition, read ahead
(f = () => { if (x) /'/.test(y) }) => f
--- error unsupported expression statement

=== a regular expression after a block statement, read ahead
(f = () => { {} /'/.test(y) }) => f
--- error unsupported expression statement

=== a division after an object literal, read ahead
((f = () => ({}) / 2) => 1)()
--- value
1

=== a name read in a block and declared after it
(x => { { const y = z; } const z = 1; return 0; })
--- error unsupported used before

=== a name read in a block, then declared in it
(a => { const b = 1; { const r = b; const b = 2; return r; } })
--- error unsupported used before

=== a name read in a branch and declared after it
(x => { if (x) return z; const z = 1; return z; })
--- error unsupported used before

=== a name read by a closure in a block and declared after it
(x => { { const f = () => w; } const w = 1; })
--- error unsupported used before

=== a name read by a closure in a block, declared later in it, with an outer binding
(a => { { const f = () => a; const a = 2; return f(); } })(1)
--- error unsupported used before

=== a name read through two functions in a block, declared later in it
(a => { { const f = () => () => a; const a = 2; return f()(); } })(1)
--- error unsupported used before

=== an if at a module's top level
@module e
if (true) {}
--- error unsupported module holds

=== a block at a module's top level
@module e
{ const a = 1; }
--- error unsupported module holds

# Errors: names

=== a name bound nowhere in a branch that is taken
(x => { if (x) return nope; return 1; })(1)
--- error name not bound

=== a block's const is gone after it
(x => { { const a = 1; } return a; })(0)
--- error name not bound

# Faults

=== a fault in a branch that is taken
(x => { if (x) return [] === []; return 1; })(1)
--- fault identity

=== a branch not taken does not fault
(x => { if (x) return [] === []; return 1; })(0)
--- value
1

=== a fault in a condition
(x => { if ([] === []) return 1; return 2; })(0)
--- fault identity

=== a fault in a later test of an else-if chain
(x => { if (x) return 1; else if ([] === []) return 2; return 3; })(0)
--- fault identity

=== a fault in a condition's branch
(o => { if (o.a) return o.a + {}; return 0; })({a: "x"})
--- fault adds two numbers, or a string and a primitive

=== a function cannot reach itself through a parameter, even with a base case
(f => f(f, 3))((self, n) => { if (n === 0) return 0; return self(self, n - 1); })
--- fault already running

=== a regular expression after a loop's head, read ahead
(f = () => { while (x) /'/.test(y) }) => f
--- error unsupported loops

=== a regular expression after a try statement, read ahead
(f = () => { try {} finally {} /'/.test(y) }) => f
--- error unsupported exceptions
