# Callback shapes (docs/matcher.md): which functions the matcher tags, and
# which it must not. `@shapes` lists the shape of each function of the
# program in function index order (the order of their ARROW_BEGIN events),
# the main program excluded; `-` is none. A shape only tags a function: every
# value here is what the bytecode gives, and diff-node checks it against Node.

# ---------------------------------------------------------------------------
# PLUCK: x => x.key

=== pluck a key
@shapes PLUCK(k)
[{k: 1}, {k: 2}].map(x => x.k)
--- value
[1,2]

=== pluck with the parameter in parentheses
@shapes PLUCK(k)
[{k: 1}].map((x) => x.k)
--- value
[1]

=== pluck with the body in parentheses
@shapes PLUCK(k)
[{k: 3}].map(x => (x.k))
--- value
[3]

=== pluck in a block body
@shapes PLUCK(k)
[{k: 1}].map(x => { return x.k; })
--- value
[1]

=== pluck in a block body without a semicolon
@shapes PLUCK(name)
[{name: "a"}].map(row => {
  return row.name
})
--- value
["a"]

=== an empty statement sends no event, so the body is still a pluck
@shapes PLUCK(k)
[{k: 7}].map(x => { ; return x.k; ; })
--- value
[7]

=== pluck a key that is also a method name
@shapes PLUCK(length)
["ab", [1, 2, 3]].map(s => s.length)
--- value
[2,3]

=== pluck a key missing from an element gives undefined
@shapes PLUCK(k)
[{k: 1}, {j: 2}].map(x => x.k)
--- value
[1,undefined]

=== a pluck of null faults as the bytecode does
@shapes PLUCK(k)
[{k: 1}, null].map(x => x.k)
--- fault cannot read a property of null

=== a pluck that is never called is tagged all the same
@shapes PLUCK(k)
[].map(x => x.k)
--- value
[]

=== a key of 64 bytes is captured
@shapes PLUCK(k234567890123456789012345678901234567890123456789012345678901234)
[{k234567890123456789012345678901234567890123456789012345678901234: 1}].map(x => x.k234567890123456789012345678901234567890123456789012345678901234)
--- value
[1]

=== a key longer than 64 bytes is not captured, and the function is not tagged
@shapes -
[{k23456789012345678901234567890123456789012345678901234567890123445: 1}].map(x => x.k23456789012345678901234567890123456789012345678901234567890123445)
--- value
[1]

# ---------------------------------------------------------------------------
# COMPARE_BY_KEY: (a, b) => a.key - b.key, ascending; b.key - a.key, descending

=== sort by a key, ascending
@shapes COMPARE_BY_KEY(score, false)
[{score: 2}, {score: 3}, {score: 1}].toSorted((a, b) => a.score - b.score)
--- value
[{"score":1},{"score":2},{"score":3}]

=== sort by a key, descending
@shapes COMPARE_BY_KEY(score, true)
[{score: 2}, {score: 3}, {score: 1}].toSorted((a, b) => b.score - a.score)
--- value
[{"score":3},{"score":2},{"score":1}]

=== sort by a key in a block body, ascending
@shapes COMPARE_BY_KEY(k, false)
[{k: 2}, {k: 1}].toSorted((a, b) => { return a.k - b.k; })
--- value
[{"k":1},{"k":2}]

=== sort by a key in a block body, descending
@shapes COMPARE_BY_KEY(k, true)
[{k: 1}, {k: 2}].toSorted((a, b) => { return b.k - a.k; })
--- value
[{"k":2},{"k":1}]

=== the parameters' positions decide the direction, not their names
@shapes COMPARE_BY_KEY(k, true)
[{k: 1}, {k: 2}].toSorted((b, a) => a.k - b.k)
--- value
[{"k":2},{"k":1}]

=== a comparator on a key holding a string faults as the bytecode does
@shapes COMPARE_BY_KEY(k, false)
[{k: "b"}, {k: "a"}].toSorted((a, b) => a.k - b.k)
--- fault arithmetic in staticjs is on numbers

# ---------------------------------------------------------------------------
# COMPARE_NUMERIC: (a, b) => a - b, ascending; b - a, descending

=== sort numbers, ascending
@shapes COMPARE_NUMERIC(false)
[3, 1, 2].toSorted((a, b) => a - b)
--- value
[1,2,3]

=== sort numbers, descending
@shapes COMPARE_NUMERIC(true)
[3, 1, 2].toSorted((a, b) => b - a)
--- value
[3,2,1]

=== sort numbers in a block body, ascending
@shapes COMPARE_NUMERIC(false)
[2, 1].toSorted((a, b) => { return a - b; })
--- value
[1,2]

=== sort numbers in a block body, descending
@shapes COMPARE_NUMERIC(true)
[1, 2].toSorted((a, b) => { return b - a; })
--- value
[2,1]

=== sort numbers: the parameters' positions decide the direction, not their names
@shapes COMPARE_NUMERIC(true)
[1, 3, 2].toSorted((b, a) => a - b)
--- value
[3,2,1]

=== sort numbers with the body in parentheses
@shapes COMPARE_NUMERIC(false)
[2, 1].toSorted((a, b) => (a - b))
--- value
[1,2]

=== sort numbers: a comparator that is never called is tagged all the same
@shapes COMPARE_NUMERIC(false)
[].toSorted((p, q) => p - q)
--- value
[]

# ---------------------------------------------------------------------------
# KEY_COMPARE: x => x.key OP c, and c OP x.key recorded with OP mirrored;
# ELEMENT_COMPARE: x => x OP c, and c OP x. OP is ===, !==, <, <=, > or >=,
# and c a literal: a number, a string, true, false or null.

=== a key equal to a number
@shapes KEY_COMPARE(k, STRICT_EQUAL, 2)
[{k: 1}, {k: 2}, {k: 3}].filter(x => x.k === 2)
--- value
[{"k":2}]

=== a number equal to a key keeps ===
@shapes KEY_COMPARE(k, STRICT_EQUAL, 2)
[{k: 1}, {k: 2}, {k: 3}].filter(x => 2 === x.k)
--- value
[{"k":2}]

=== a key not equal to a number, both ways
@shapes KEY_COMPARE(k, STRICT_NOT_EQUAL, 2) KEY_COMPARE(k, STRICT_NOT_EQUAL, 2)
[[{k: 1}, {k: 2}, {k: 3}].filter(x => x.k !== 2), [{k: 1}, {k: 2}, {k: 3}].filter(x => 2 !== x.k)]
--- value
[[{"k":1},{"k":3}],[{"k":1},{"k":3}]]

=== a key less than a number, and a number greater than a key
@shapes KEY_COMPARE(k, LESS, 2) KEY_COMPARE(k, LESS, 2)
[[{k: 1}, {k: 2}, {k: 3}].filter(x => x.k < 2), [{k: 1}, {k: 2}, {k: 3}].filter(x => 2 > x.k)]
--- value
[[{"k":1}],[{"k":1}]]

=== a key at most a number, and a number at least a key
@shapes KEY_COMPARE(k, LESS_EQUAL, 2) KEY_COMPARE(k, LESS_EQUAL, 2)
[[{k: 1}, {k: 2}, {k: 3}].filter(x => x.k <= 2), [{k: 1}, {k: 2}, {k: 3}].filter(x => 2 >= x.k)]
--- value
[[{"k":1},{"k":2}],[{"k":1},{"k":2}]]

=== a key greater than a number, and a number less than a key
@shapes KEY_COMPARE(k, GREATER, 2) KEY_COMPARE(k, GREATER, 2)
[[{k: 1}, {k: 2}, {k: 3}].filter(x => x.k > 2), [{k: 1}, {k: 2}, {k: 3}].filter(x => 2 < x.k)]
--- value
[[{"k":3}],[{"k":3}]]

=== a key at least a number, and a number at most a key
@shapes KEY_COMPARE(k, GREATER_EQUAL, 2) KEY_COMPARE(k, GREATER_EQUAL, 2)
[[{k: 1}, {k: 2}, {k: 3}].filter(x => x.k >= 2), [{k: 1}, {k: 2}, {k: 3}].filter(x => 2 <= x.k)]
--- value
[[{"k":2},{"k":3}],[{"k":2},{"k":3}]]

=== an element equal to a number, both ways
@shapes ELEMENT_COMPARE(STRICT_EQUAL, 2) ELEMENT_COMPARE(STRICT_EQUAL, 2)
[[1, 2, 3].filter(x => x === 2), [1, 2, 3].filter(x => 2 === x)]
--- value
[[2],[2]]

=== an element not equal to a number, both ways
@shapes ELEMENT_COMPARE(STRICT_NOT_EQUAL, 2) ELEMENT_COMPARE(STRICT_NOT_EQUAL, 2)
[[1, 2, 3].filter(x => x !== 2), [1, 2, 3].filter(x => 2 !== x)]
--- value
[[1,3],[1,3]]

=== an element less than a number, and a number greater than an element
@shapes ELEMENT_COMPARE(LESS, 2) ELEMENT_COMPARE(LESS, 2)
[[1, 2, 3].filter(x => x < 2), [1, 2, 3].filter(x => 2 > x)]
--- value
[[1],[1]]

=== an element at most a number, and a number at least an element
@shapes ELEMENT_COMPARE(LESS_EQUAL, 2) ELEMENT_COMPARE(LESS_EQUAL, 2)
[[1, 2, 3].filter(x => x <= 2), [1, 2, 3].filter(x => 2 >= x)]
--- value
[[1,2],[1,2]]

=== an element greater than a number, and a number less than an element
@shapes ELEMENT_COMPARE(GREATER, 2) ELEMENT_COMPARE(GREATER, 2)
[[1, 2, 3].filter(x => x > 2), [1, 2, 3].filter(x => 2 < x)]
--- value
[[3],[3]]

=== an element at least a number, and a number at most an element
@shapes ELEMENT_COMPARE(GREATER_EQUAL, 2) ELEMENT_COMPARE(GREATER_EQUAL, 2)
[[1, 2, 3].filter(x => x >= 2), [1, 2, 3].filter(x => 2 <= x)]
--- value
[[2,3],[2,3]]

=== predicates in a block body
@shapes KEY_COMPARE(k, LESS, 2) ELEMENT_COMPARE(GREATER, 1)
[[{k: 1}, {k: 2}].filter(x => { return x.k < 2; }), [1, 2].filter(x => { return 1 < x; })]
--- value
[[{"k":1}],[2]]

=== predicates with the body in parentheses
@shapes KEY_COMPARE(k, STRICT_EQUAL, 1) ELEMENT_COMPARE(STRICT_EQUAL, 1)
[[{k: 1}].filter(x => (x.k === 1)), [1].filter((x) => (1 === x))]
--- value
[[{"k":1}],[1]]

=== operands: a fraction, an exponent, a hexadecimal number
@shapes ELEMENT_COMPARE(LESS, 1.5) ELEMENT_COMPARE(LESS, 1e+21) KEY_COMPARE(k, STRICT_EQUAL, 16)
[[1, 1.5, 2].filter(x => x < 1.5), [1, Infinity].filter(x => x < 1e21), [{k: 16}, {k: 10}].filter(x => x.k === 0x10)]
--- value
[[1],[1],[{"k":16}]]

=== operands: strings, an empty one and one that is not ASCII
@shapes KEY_COMPARE(s, STRICT_EQUAL, "open") ELEMENT_COMPARE(STRICT_NOT_EQUAL, "") ELEMENT_COMPARE(STRICT_EQUAL, "été")
[[{s: "open"}, {s: "closed"}].filter(x => x.s === "open"), ["", "a"].filter(x => x !== ""), ["ete", "été"].findIndex(x => "été" === x)]
--- value
[[{"s":"open"}],["a"],1]

=== operands: a string of 64 bytes is captured
@shapes ELEMENT_COMPARE(STRICT_EQUAL, "k234567890123456789012345678901234567890123456789012345678901234")
["k234567890123456789012345678901234567890123456789012345678901234", "k"].findIndex(x => x === "k234567890123456789012345678901234567890123456789012345678901234")
--- value
0

=== operands: true, false and null
@shapes ELEMENT_COMPARE(STRICT_EQUAL, true) KEY_COMPARE(k, STRICT_EQUAL, false) ELEMENT_COMPARE(STRICT_NOT_EQUAL, null) KEY_COMPARE(k, STRICT_EQUAL, null)
[[true, false, 1, "true"].filter(x => x === true), [{k: false}, {k: 0}, {k: ""}].filter(x => false === x.k), [null, undefined, 0].filter(x => x !== null), [{k: null}, {}].filter(x => x.k === null)]
--- value
[[true],[{"k":false}],[undefined,0],[{"k":null}]]

=== strings compared with strings, both ways
@shapes ELEMENT_COMPARE(LESS, "b") KEY_COMPARE(s, GREATER_EQUAL, "b")
[["a", "b", "c"].filter(x => x < "b"), [{s: "a"}, {s: "b"}, {s: "c"}].filter(x => "b" <= x.s)]
--- value
[["a"],[{"s":"b"},{"s":"c"}]]

=== each method with a predicate
@shapes KEY_COMPARE(k, GREATER, 1) KEY_COMPARE(k, GREATER, 1) KEY_COMPARE(k, GREATER, 1) KEY_COMPARE(k, GREATER, 1) KEY_COMPARE(k, GREATER, 1) KEY_COMPARE(k, GREATER, 1)
[[{k: 1}, {k: 2}, {k: 3}].filter(x => x.k > 1), [{k: 1}, {k: 2}, {k: 3}].find(x => x.k > 1), [{k: 1}, {k: 2}, {k: 3}].findIndex(x => x.k > 1), [{k: 1}, {k: 2}, {k: 3}].findLast(x => x.k > 1), [{k: 1}, {k: 2}, {k: 3}].some(x => x.k > 1), [{k: 1}, {k: 2}, {k: 3}].every(x => x.k > 1)]
--- value
[[{"k":2},{"k":3}],{"k":2},1,{"k":3},true,false]

=== a template without a substitution is a string literal: the parser sends a STRING
@shapes KEY_COMPARE(k, STRICT_EQUAL, "a") ELEMENT_COMPARE(STRICT_EQUAL, "b")
[[{k: "a"}, {k: "b"}].filter(x => x.k === `a`), ["a", "b"].filter(x => `b` === x)]
--- value
[[{"k":"a"}],["b"]]

=== a predicate that is never called is tagged all the same
@shapes ELEMENT_COMPARE(STRICT_EQUAL, 1)
[].some(x => x === 1)
--- value
false

# ---------------------------------------------------------------------------
# SUM: (a, x) => a + x; SUM_KEY: (a, x) => a + x.key. The flag says the
# element is the left operand: x + a, x.key + a.

=== sum the elements
@shapes SUM(false)
[1, 2, 3].reduce((a, x) => a + x, 0)
--- value
6

=== sum the elements, the element first
@shapes SUM(true)
[1, 2, 3].reduce((a, x) => x + a, 0)
--- value
6

=== sum the elements in a block body, both orders
@shapes SUM(false) SUM(true)
[[1, 2].reduce((a, x) => { return a + x; }, 0), [1, 2].reduce((a, x) => { return x + a; }, 0)]
--- value
[3,3]

=== sum the elements with the body in parentheses
@shapes SUM(false)
[1, 2].reduce((acc, n) => (acc + n), 0)
--- value
3

=== sum: the parameters' positions decide the order, not their names
@shapes SUM(true) SUM(false)
[["a", "b"].reduce((x, a) => a + x, ""), ["a", "b"].reduce((x, a) => x + a, "")]
--- value
["ba","ab"]

=== sum a key
@shapes SUM_KEY(k, false)
[{k: 1}, {k: 2}].reduce((a, x) => a + x.k, 0)
--- value
3

=== sum a key, the key first
@shapes SUM_KEY(k, true)
[{k: 1}, {k: 2}].reduce((a, x) => x.k + a, 0)
--- value
3

=== sum a key in a block body, both orders
@shapes SUM_KEY(kwh, false) SUM_KEY(kwh, true)
[[{kwh: 1.5}].reduce((t, r) => { return t + r.kwh; }, 1), [{kwh: 1.5}].reduce((t, r) => { return r.kwh + t; }, 1)]
--- value
[2.5,2.5]

=== sum a key with the body in parentheses
@shapes SUM_KEY(k, false)
[{k: 1}, {k: 2}].reduce((a, x) => (a + x.k), 0)
--- value
3

=== sum a key that is also a method name
@shapes SUM_KEY(length, false)
[[1, 2], "abc"].reduce((a, x) => a + x.length, 0)
--- value
5

=== a sum that is never called is tagged all the same
@shapes SUM(false) SUM_KEY(k, true)
[[5].reduce((a, x) => a + x), [].reduce((a, x) => x.k + a, 0)]
--- value
[5,0]

# ---------------------------------------------------------------------------
# Functions inside functions, and functions in defaults and patterns

=== an outer function untagged, the function inside it tagged
@shapes - PLUCK(k)
[[{k: 1}], [{k: 2}]].map(xs => xs.map(x => x.k))
--- value
[[1],[2]]

=== a tagged function before an untagged one
@shapes PLUCK(k) -
[{k: 1}].map(x => x.k).map(y => y + 1)
--- value
[2]

=== a tagged function in a parameter's default
@shapes - PLUCK(k)
((f = x => x.k) => [{k: 4}].map(f))()
--- value
[4]

=== a tagged function in a default of a held const pattern
@shapes PLUCK(k) -
@module shapes_held
const {f = x => x.k} = {};
export const r = [{k: 5}].map(f).map(v => v * 2);
--- value
{"r":[10]}

=== an untagged comparator around a tagged pluck
@shapes - PLUCK(k) PLUCK(k)
[{k: 2}, {k: 1}].toSorted((a, b) => [a].map(x => x.k)[0] - [b].map(y => y.k)[0])
--- value
[{"k":1},{"k":2}]

# ---------------------------------------------------------------------------
# Near misses: none of these is tagged.

=== a captured name is not the parameter
@shapes - -
(y => [{k: 1}].map(x => y.k))({k: 9})
--- value
[9]

=== a parameter with a default
@shapes -
[{k: 1}].map((x = 1) => x.k)
--- value
[1]

=== a parameter that is a pattern
@shapes -
[{k: 1}].map(({k}) => k)
--- value
[1]

=== a rest parameter
@shapes -
[{k: 1}].map((...x) => x.length)
--- value
[3]

=== a second parameter
@shapes -
[{k: 1}].map((x, i) => x.k)
--- value
[1]

=== an optional chain
@shapes -
[{k: 1}, null].map(x => x?.k)
--- value
[1,undefined]

=== a computed key
@shapes -
[{k: 1}].map(x => x["k"])
--- value
[1]

=== a key of a key
@shapes -
[{k: {j: 1}}].map(x => x.k.j)
--- value
[1]

=== a negated key
@shapes -
[{k: 1}].map(x => -x.k)
--- value
[-1]

=== different keys on the two sides
@shapes -
[{k: 2, j: 0}, {k: 1, j: 5}].toSorted((a, b) => a.k - b.j)
--- value
[{"k":2,"j":0},{"k":1,"j":5}]

=== the first parameter on both sides
@shapes -
[{k: 2}, {k: 1}].toSorted((a, b) => a.k - a.k)
--- value
[{"k":2},{"k":1}]

=== the second parameter on both sides
@shapes -
[{k: 2}, {k: 1}].toSorted((a, b) => b.k - b.k)
--- value
[{"k":2},{"k":1}]

=== an addition, not a subtraction
@shapes -
[{k: 0, i: 1}, {k: 0, i: 2}].toSorted((a, b) => a.k + b.k)
--- value
[{"k":0,"i":1},{"k":0,"i":2}]

=== a third parameter
@shapes -
[{k: 2}, {k: 1}].toSorted((a, b, c) => a.k - b.k)
--- value
[{"k":1},{"k":2}]

=== a comparator reading a captured name
@shapes - -
(c => [{k: 0, i: 1}, {k: 0, i: 2}].toSorted((a, b) => a.k - c.k))({k: 0})
--- value
[{"k":0,"i":1},{"k":0,"i":2}]

=== a const before the return
@shapes -
[{k: 1}].map(x => { const y = x; return y.k; })
--- value
[1]

=== a block that shadows the parameter
@shapes -
[{k: 1}].map(x => { { const x = {k: 5}; return x.k; } })
--- value
[5]

=== an if around the return
@shapes -
[{k: 1}].map(x => { if (x) return x.k; })
--- value
[1]

=== a statement after the return
@shapes -
[{k: 1}].map(x => { return x.k; return 2; })
--- value
[1]

=== a function returning a function
@shapes - - -
[{k: 1}].map(x => y => x.k).map(f => f())
--- value
[1]

=== numbers: the first parameter on both sides
@shapes -
[3, 1, 2].toSorted((a, b) => a - a)
--- value
[3,1,2]

=== numbers: the second parameter on both sides
@shapes -
[3, 1, 2].toSorted((a, b) => b - b)
--- value
[3,1,2]

=== numbers: an addition is SUM, not a comparator, and toSorted runs its bytecode
@shapes SUM(false)
[0, -0, 0].toSorted((a, b) => a + b)
--- value
[0,-0,0]

=== numbers: a subtraction of a subtraction
@shapes -
[3, 1, 2].toSorted((a, b) => a - b - 0)
--- value
[1,2,3]

=== numbers: a subtraction from a negation
@shapes -
[1, -1].toSorted((a, b) => -a - b)
--- value
[1,-1]

=== numbers: a key on one side only
@shapes -
[{k: 1}, {k: 2}].toSorted((a, b) => a.k - b)
--- fault arithmetic in staticjs is on numbers

=== numbers: a third parameter
@shapes -
[3, 1, 2].toSorted((a, b, c) => a - b)
--- value
[1,2,3]

=== numbers: a rest parameter
@shapes -
[3, 1, 2].toSorted((a, ...b) => a - b)
--- fault arithmetic in staticjs is on numbers

=== numbers: a rest parameter, never called
@shapes -
[1].toSorted((a, ...b) => a - b)
--- value
[1]

=== numbers: a parameter with a default
@shapes -
[3, 1, 2].toSorted((a = 0, b) => a - b)
--- value
[1,2,3]

=== numbers: a captured name on one side
@shapes - -
(c => [0, -0, 0].toSorted((a, b) => a - c))(0)
--- value
[0,-0,0]

=== numbers: one parameter
@shapes -
[3, 1, 2].map(a => a - 1)
--- value
[2,0,1]

=== numbers: a const before the return
@shapes -
[3, 1, 2].toSorted((a, b) => { const d = a - b; return d; })
--- value
[1,2,3]

=== numbers: a comparison, not a subtraction
@shapes -
[3, 1, 2].toSorted((a, b) => a > b ? 1 : -1)
--- value
[1,2,3]

=== predicates: a captured name, not a literal
@global y = 1
@shapes -
[{k: 1}, {k: 2}].filter(x => x.k === y)
--- value
[{"k":1}]

=== predicates: undefined, NaN and Infinity are names, not literals
@shapes - - -
[[{k: 1}, {}].filter(x => x.k === undefined), [NaN, 1].filter(x => x === NaN), [1, Infinity].filter(x => x < Infinity)]
--- value
[[{}],[],[1]]

=== predicates: a negative number is a negation of a literal
@shapes - -
[[{k: -1}, {k: 1}].filter(x => x.k === -1), [-1, 1].filter(x => -1 < x)]
--- value
[[{"k":-1}],[1]]

=== predicates: a sum of literals
@shapes -
[{k: "ab"}, {k: "a"}].filter(x => x.k === "a" + "b")
--- value
[{"k":"ab"}]

=== predicates: a template with a substitution is not a literal
@shapes - -
[[{k: "a1"}, {k: "b"}].filter(x => x.k === `a${1}`), ["a", "1b"].filter(x => `${1}b` === x)]
--- value
[[{"k":"a1"}],["1b"]]

=== predicates: a string of 65 bytes is not captured
@shapes -
["k2345678901234567890123456789012345678901234567890123456789012345", "k"].findIndex(x => x === "k2345678901234567890123456789012345678901234567890123456789012345")
--- value
0

=== predicates: arithmetic, not a comparison
@shapes - - - -
[[{k: 1}].map(x => x.k + 1), [{k: 1}].map(x => x.k - 1), [1].map(x => x * 2), [1].map(x => 2 % x)]
--- value
[[2],[0],[2],[0]]

=== predicates: in is not a comparison
@shapes -
[{k: 1}, {}].filter(x => "k" in x)
--- value
[{"k":1}]

=== predicates: a key of a key
@shapes -
[{k: {j: 1}}, {k: {j: 2}}].filter(x => x.k.j === 1)
--- value
[{"k":{"j":1}}]

=== predicates: an optional chain
@shapes -
[{k: 1}, null].filter(x => x?.k === 1)
--- value
[{"k":1}]

=== predicates: a computed key
@shapes -
[{k: 1}, {k: 2}].filter(x => x["k"] === 1)
--- value
[{"k":1}]

=== predicates: a second parameter
@shapes -
[{k: 1}, {k: 2}].filter((x, i) => x.k === 1)
--- value
[{"k":1}]

=== predicates: a parameter with a default, a rest parameter
@shapes - -
[[1, 2].filter((x = 0) => x === 1), [1, 2].filter((...x) => x.length === 3)]
--- value
[[1],[1,2]]

=== predicates: two literals
@shapes -
[1, 2].filter(x => 1 === 2)
--- value
[]

=== predicates: two keys, and the element on both sides
@shapes - -
[[{k: 1, j: 1}, {k: 1, j: 2}].filter(x => x.k === x.j), [1, NaN].filter(x => x === x)]
--- value
[[{"k":1,"j":1}],[1]]

=== predicates: a negation of a comparison
@shapes -
[{k: 1}, {k: 2}].filter(x => !(x.k === 1))
--- value
[{"k":2}]

=== predicates: a comparison compared with true
@shapes -
[{k: 1}, {k: 2}].filter(x => (x.k === 1) === true)
--- value
[{"k":1}]

=== predicates: a comparison in a conjunction
@shapes -
[{k: 1}, {k: 2}].filter(x => x.k === 1 && true)
--- value
[{"k":1}]

=== predicates: a parameter that is a pattern
@shapes -
[{k: 1}, {k: 2}].filter(({k}) => k === 1)
--- value
[{"k":1}]

=== predicates: a const before the return
@shapes -
[{k: 1}, {k: 2}].filter(x => { const y = 1; return x.k === y; })
--- value
[{"k":1}]

=== predicates: a key compared with a literal of a captured object
@shapes - -
(y => [{k: 1}, {k: 2}].filter(x => y.k === 1))({k: 1})
--- value
[{"k":1},{"k":2}]

=== predicates: equality that JavaScript has and staticjs leaves out
[1, 2].filter(x => x == 1)
--- error unsupported

=== sums: the accumulator on both sides
@shapes -
[1, 2].reduce((a, x) => a + a, 1)
--- value
4

=== sums: the element on both sides
@shapes -
[1, 2].reduce((a, x) => x + x, 0)
--- value
4

=== sums: a subtraction is COMPARE_NUMERIC, and reduce runs its bytecode
@shapes COMPARE_NUMERIC(false)
[1, 2, 3].reduce((a, x) => a - x, 10)
--- value
4

=== sums: a sum of a sum
@shapes - -
[[1, 2].reduce((a, x) => a + x + 0, 0), [1, 2].reduce((a, x) => a + (x + 1), 0)]
--- value
[3,5]

=== sums: a product, not a sum
@shapes -
[2, 3].reduce((a, x) => a * x, 1)
--- value
6

=== sums: a third parameter
@shapes -
[1, 2].reduce((a, x, i) => a + x, 0)
--- value
3

=== sums: one parameter
@shapes -
[1, 2].reduce(a => a + 1, 0)
--- value
2

=== sums: a parameter with a default
@shapes -
[1, 2].reduce((a = 0, x) => a + x, 0)
--- value
3

=== sums: a rest parameter
@shapes -
[1].reduce((a, ...x) => a + x, 0)
--- fault staticjs adds two numbers, or a string and a primitive

=== sums: a rest parameter, never called
@shapes -
[].reduce((a, ...x) => a + x, 0)
--- value
0

=== sums: a parameter that is a pattern
@shapes -
[{k: 1}, {k: 2}].reduce((a, {k}) => a + k, 0)
--- value
3

=== sums: a captured name, not the element
@shapes - -
(y => [1, 2].reduce((a, x) => a + y, 0))(5)
--- value
10

=== sums: a key of the accumulator
@shapes -
[1].reduce((a, x) => a.k + x, {k: 1})
--- value
2

=== sums: keys on both sides
@shapes -
[{k: 2}].reduce((a, x) => a.k + x.k, {k: 1})
--- value
3

=== sums: a key of a key
@shapes -
[{k: {j: 1}}].reduce((a, x) => a + x.k.j, 0)
--- value
1

=== sums: a computed key and an index
@shapes - -
[[{k: 1}].reduce((a, x) => a + x["k"], 0), [[1], [2]].reduce((a, x) => a + x[0], 0)]
--- value
[1,3]

=== sums: an optional chain
@shapes -
[{k: 1}].reduce((a, x) => a + x?.k, 0)
--- value
1

=== sums: a key read from the wrong parameter's name
@shapes - -
(b => [{k: 1}].reduce((a, x) => a + b.k, 0))({k: 2})
--- value
2

=== sums: a const before the return
@shapes -
[1, 2].reduce((a, x) => { const s = a + x; return s; }, 0)
--- value
3

=== sums: a negated element
@shapes -
[1, 2].reduce((a, x) => a + -x, 0)
--- value
-3

# ---------------------------------------------------------------------------
# The fast paths (docs/matcher.md, section 7): toSorted runs a COMPARE_BY_KEY
# comparator and map a PLUCK callback in C while their preconditions hold,
# and calls the bytecode otherwise. Every case runs with and without shapes
# (spec_test) and must give the same value, or the same fault at the same
# place, with no more fuel. The bytes and chunks runs pause on every unit and
# every 13, inside the loops.

# toSorted: values

=== sort by a key: equal keys keep their order, ascending
@shapes COMPARE_BY_KEY(k, false) PLUCK(i)
[{k: 1, i: 0}, {k: 0, i: 1}, {k: 1, i: 2}, {k: 0, i: 3}, {k: 1, i: 4}].toSorted((a, b) => a.k - b.k).map(r => r.i)
--- value
[1,3,0,2,4]

=== sort by a key: equal keys keep their order, descending
@shapes COMPARE_BY_KEY(k, true) PLUCK(i)
[{k: 1, i: 0}, {k: 0, i: 1}, {k: 1, i: 2}, {k: 0, i: 3}, {k: 1, i: 4}].toSorted((a, b) => b.k - a.k).map(r => r.i)
--- value
[0,2,4,1,3]

=== sort by a key: 0 and -0 are equal keys, and keep their order
@shapes COMPARE_BY_KEY(k, false) PLUCK(i)
[{k: 0, i: 0}, {k: -0, i: 1}, {k: 0, i: 2}, {k: -0, i: 3}].toSorted((a, b) => a.k - b.k).map(r => r.i)
--- value
[0,1,2,3]

=== sort by a key: -0 and 0, descending
@shapes COMPARE_BY_KEY(k, true) PLUCK(i)
[{k: -0, i: 0}, {k: 0, i: 1}, {k: -1, i: 2}, {k: -0, i: 3}].toSorted((a, b) => b.k - a.k).map(r => r.i)
--- value
[0,1,3,2]

=== sort by a key: -0 survives as the key's value
@shapes COMPARE_BY_KEY(k, false) PLUCK(k)
[{k: 1}, {k: -0}].toSorted((a, b) => a.k - b.k).map(r => r.k)
--- value
[-0,1]

=== sort by a key: Infinity, -Infinity, and two Infinity keys, whose difference is NaN
@shapes COMPARE_BY_KEY(k, false) PLUCK(i)
[{k: Infinity, i: 0}, {k: 1, i: 1}, {k: -Infinity, i: 2}, {k: Infinity, i: 3}, {k: -Infinity, i: 4}].toSorted((a, b) => a.k - b.k).map(r => r.i)
--- value
[2,4,1,0,3]

=== sort by a key: a single NaN key
@shapes COMPARE_BY_KEY(k, false) PLUCK(k)
[{k: NaN}, {k: 2}, {k: 1}].toSorted((a, b) => a.k - b.k).map(r => r.k)
--- value
[NaN,1,2]

=== sort by a key: fractions and large numbers
@shapes COMPARE_BY_KEY(k, false) PLUCK(k)
[{k: 0.3}, {k: 1e21}, {k: 0.1 + 0.2}, {k: -0.5}, {k: 0.7 - 0.4}].toSorted((a, b) => a.k - b.k).map(r => r.k)
--- value
[-0.5,0.29999999999999993,0.3,0.30000000000000004,1e+21]

=== sort by a key: objects with the key at different places, and with many keys
@shapes COMPARE_BY_KEY(k, false) PLUCK(i)
[{a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, k: 3, i: 0}, {k: 1, i: 1}, {i: 2, k: 2}, {x: 0, i: 3, k: 0}].toSorted((a, b) => a.k - b.k).map(r => r.i)
--- value
[3,1,2,0]

=== sort by a key: the elements are objects the result shares
@shapes - COMPARE_BY_KEY(k, false) -
(xs => [xs.toSorted((a, b) => a.k - b.k), xs].map(ys => ys[0].v))([{k: 2, v: [2]}, {k: 1, v: [1]}])
--- value
[[1],[2]]

=== sort by a key: an empty array, and one element, never compare
@shapes COMPARE_BY_KEY(k, false) COMPARE_BY_KEY(k, false)
[[].toSorted((a, b) => a.k - b.k), [{k: "a"}].toSorted((a, b) => a.k - b.k)]
--- value
[[],[{"k":"a"}]]

=== sort by a key: undefined elements go last and are never compared
@shapes COMPARE_BY_KEY(k, false)
[undefined, {k: 2}, undefined, {k: 1}].toSorted((a, b) => a.k - b.k)
--- value
[{"k":1},{"k":2},undefined,undefined]

=== sort by a key named toString that the objects hold
@shapes COMPARE_BY_KEY(toString, false) PLUCK(toString)
[{toString: 2}, {toString: 1}].toSorted((a, b) => a.toString - b.toString).map(r => r.toString)
--- value
[1,2]

=== sort arrays and strings by length: the bytecode reads it
@shapes COMPARE_BY_KEY(length, false)
[[1, 2, 3], "ab", [], "x"].toSorted((a, b) => a.length - b.length)
--- value
[[],"x","ab",[1,2,3]]

=== sort sets by size: the bytecode reads it
@shapes COMPARE_BY_KEY(size, true)
[new Set([1]), new Set([1, 2, 3]), new Set()].toSorted((a, b) => b.size - a.size)
--- value
[new Set([1,2,3]),new Set([1]),new Set([])]

=== sort mixed elements: objects in C, arrays and strings in bytecode
@shapes COMPARE_BY_KEY(length, false)
[[1, 2, 3], {length: 2}, "a", {length: 0}, [1, 2, 3, 4]].toSorted((a, b) => a.length - b.length)
--- value
[{"length":0},"a",{"length":2},[1,2,3],[1,2,3,4]]

=== sort a large array by a key
@shapes - COMPARE_BY_KEY(k, true) PLUCK(k)
[3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4, 3].map((n, i) => ({k: n, i})).toSorted((a, b) => b.k - a.k).map(r => r.k)
--- value
[9,9,9,8,8,7,6,6,6,5,5,5,4,4,4,3,3,3,3,3,2,2,2,1,1]

# toSorted: faults, each the bytecode's own

=== sort by a key: a missing key faults
@shapes COMPARE_BY_KEY(k, false)
[{k: 1}, {j: 2}].toSorted((a, b) => a.k - b.k)
--- fault arithmetic in staticjs is on numbers

=== sort by a key: an undefined key faults
@shapes COMPARE_BY_KEY(k, false)
[{k: 1}, {k: undefined}].toSorted((a, b) => a.k - b.k)
--- fault arithmetic in staticjs is on numbers

=== sort by a key: a string key faults
@shapes COMPARE_BY_KEY(k, true)
[{k: 1}, {k: "2"}].toSorted((a, b) => b.k - a.k)
--- fault arithmetic in staticjs is on numbers

=== sort by a key: a null or boolean key faults
@shapes COMPARE_BY_KEY(k, false)
[{k: null}, {k: true}].toSorted((a, b) => a.k - b.k)
--- fault arithmetic in staticjs is on numbers

=== sort by a key found only on Object.prototype faults
@shapes COMPARE_BY_KEY(toString, false)
[{k: 1}, {k: 2}].toSorted((a, b) => a.toString - b.toString)
--- fault staticjs objects have no prototype

=== sort by a key: a null element faults
@shapes COMPARE_BY_KEY(k, false)
[{k: 1}, null].toSorted((a, b) => a.k - b.k)
--- fault cannot read a property of null

=== sort by a key: number elements fault
@shapes COMPARE_BY_KEY(k, false)
[2, 1].toSorted((a, b) => a.k - b.k)
--- fault staticjs reads properties of objects, arrays, strings, sets, maps and iterators

=== sort by a key: the one bad pair faults where the bytecode does, after fast pairs
@shapes COMPARE_BY_KEY(k, false)
[{k: 4}, {k: 3}, {k: 2}, {k: 1}, {k: 6}, {k: 5}, {k: 8}, {j: 7}].toSorted((a, b) =>
  a.k -
  b.k)
--- fault arithmetic in staticjs is on numbers

=== sort by a key: a key of an array that is not length reads as undefined, and faults
@shapes COMPARE_BY_KEY(k, false)
[[1], [2]].toSorted((a, b) => a.k - b.k)
--- fault arithmetic in staticjs is on numbers

# toSorted with COMPARE_NUMERIC (docs/matcher.md, 11.6): pairs of numbers in
# C, anything else in bytecode, which faults on it.

=== sort numbers: equal numbers, and a larger array both ways
@shapes COMPARE_NUMERIC(false) COMPARE_NUMERIC(true)
[[3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4, 3].toSorted((a, b) => a - b),
 [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4, 3].toSorted((a, b) => b - a)]
--- value
[[1,1,2,2,2,3,3,3,3,3,4,4,4,5,5,5,6,6,6,7,8,8,9,9,9],[9,9,9,8,8,7,6,6,6,5,5,5,4,4,4,3,3,3,3,3,2,2,2,1,1]]

=== sort numbers: 0 and -0 are equal, and keep their order, ascending
@shapes COMPARE_NUMERIC(false)
[0, -0, 0, -0, -1, 1].toSorted((a, b) => a - b)
--- value
[-1,0,-0,0,-0,1]

=== sort numbers: -0 and 0 are equal, and keep their order, descending
@shapes COMPARE_NUMERIC(true)
[-0, 0, -1, -0, 1, 0].toSorted((a, b) => b - a)
--- value
[1,-0,0,-0,0,-1]

=== sort numbers: a single NaN
@shapes COMPARE_NUMERIC(false)
[NaN, 2, 1].toSorted((a, b) => a - b)
--- value
[NaN,1,2]

=== sort numbers: a NaN difference keeps the pair's order
@shapes COMPARE_NUMERIC(false)
[2, NaN].toSorted((a, b) => a - b)
--- value
[2,NaN]

=== sort numbers: Infinity, -Infinity, and two Infinities, whose difference is NaN
@shapes COMPARE_NUMERIC(false) COMPARE_NUMERIC(true)
[[Infinity, 1, -Infinity, Infinity, -Infinity].toSorted((a, b) => a - b),
 [Infinity, 1, -Infinity, Infinity, -Infinity].toSorted((a, b) => b - a)]
--- value
[[-Infinity,-Infinity,1,Infinity,Infinity],[Infinity,Infinity,1,-Infinity,-Infinity]]

=== sort numbers: fractions, large and small numbers
@shapes COMPARE_NUMERIC(false)
[0.3, 1e21, 0.1 + 0.2, -0.5, 0.7 - 0.4, 1 / 1e22 / 1e22, -1e22 * 1e22, 1e22 * 1e22].toSorted((a, b) => a - b)
--- value
[-1e+44,-0.5,1.0000000000000001e-44,0.29999999999999993,0.3,0.30000000000000004,1e+21,1e+44]

=== sort numbers: a difference that overflows to Infinity
@shapes - COMPARE_NUMERIC(false)
(h => [h, -h, h * 1.7, -h * 1.7].toSorted((a, b) => a - b))(1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22 * 1e22)
--- value
[-1.7000000000000003e+308,-1.0000000000000002e+308,1.0000000000000002e+308,1.7000000000000003e+308]

=== sort numbers: undefined elements go last and are never compared
@shapes COMPARE_NUMERIC(false)
[undefined, 2, undefined, 1].toSorted((a, b) => a - b)
--- value
[1,2,undefined,undefined]

=== sort numbers: undefined among strings is never compared, so nothing faults
@shapes COMPARE_NUMERIC(true)
[undefined, "a"].toSorted((a, b) => b - a)
--- value
["a",undefined]

=== sort numbers: an empty array, and one element of any kind, never compare
@shapes COMPARE_NUMERIC(false) COMPARE_NUMERIC(false)
[[].toSorted((a, b) => a - b), ["a"].toSorted((a, b) => a - b)]
--- value
[[],["a"]]

=== sort numbers: the receiver nothing else holds takes the order
@shapes - COMPARE_NUMERIC(true)
[1, 2, 3].map(x => x).toSorted((a, b) => b - a)
--- value
[3,2,1]

=== sort numbers: the array is not changed
@shapes - COMPARE_NUMERIC(false)
(xs => [xs.toSorted((a, b) => a - b), xs])([2, 3, 1])
--- value
[[1,2,3],[2,3,1]]

=== sort numbers: a tagged comparator in reduce runs its bytecode
@shapes COMPARE_NUMERIC(false)
[1, 2, 3].reduce((a, b) => a - b)
--- value
-4

=== sort numbers: a tagged comparator in map runs its bytecode
@shapes COMPARE_NUMERIC(false)
[5, 7].map((a, b) => a - b)
--- value
[5,6]

=== sort numbers: the comparator called directly
@shapes COMPARE_NUMERIC(true)
((a, b) => b - a)(1, 3)
--- value
2

# toSorted with COMPARE_NUMERIC: faults, each the bytecode's own

=== sort numbers: two strings fault
@shapes COMPARE_NUMERIC(false)
["b", "a"].toSorted((a, b) => a - b)
--- fault arithmetic in staticjs is on numbers

=== sort numbers: a string among numbers faults, after pairs in C
@shapes COMPARE_NUMERIC(false)
[4, 3, 2, 1, 6, 5, 8, "7"].toSorted((a, b) => a - b)
--- fault arithmetic in staticjs is on numbers

=== sort numbers: a string first faults, descending
@shapes COMPARE_NUMERIC(true)
["1", 2, 3].toSorted((a, b) => b - a)
--- fault arithmetic in staticjs is on numbers

=== sort numbers: null faults
@shapes COMPARE_NUMERIC(false)
[1, null].toSorted((a, b) => a - b)
--- fault arithmetic in staticjs is on numbers

=== sort numbers: a boolean faults
@shapes COMPARE_NUMERIC(false)
[true, 1].toSorted((a, b) => a - b)
--- fault arithmetic in staticjs is on numbers

=== sort numbers: objects and arrays fault
@shapes COMPARE_NUMERIC(false) COMPARE_NUMERIC(false)
[[[2], [1]].toSorted((a, b) => a - b), [{}, 1].toSorted((a, b) => a - b)]
--- fault arithmetic in staticjs is on numbers

=== sort numbers: functions fault
@shapes - - COMPARE_NUMERIC(false)
[() => 1, () => 2].toSorted((a, b) => a - b)
--- fault arithmetic in staticjs is on numbers

# map: values

=== pluck: present and missing keys
@shapes PLUCK(k)
[{k: 1}, {}, {j: 1, k: 2}, {k: undefined}].map(x => x.k)
--- value
[1,undefined,2,undefined]

=== pluck: the values keep -0, NaN and Infinity
@shapes PLUCK(k) -
[{k: -0}, {k: NaN}, {k: -Infinity}, {k: 0}].map(x => x.k).map(v => 1 / v)
--- value
[-Infinity,NaN,-0,Infinity]

=== pluck: values that are objects, arrays and functions
@shapes - PLUCK(k) -
[{k: {a: 1}}, {k: [1, 2]}, {k: () => 3}, {k: null}].map(x => x.k).map(v => typeof v === "function" ? v() : v)
--- value
[{"a":1},[1,2],3,null]

=== pluck: a value the result shares with its object
@shapes - PLUCK(v) -
(rows => [rows.map(r => r.v), rows].map(xs => xs[0]))([{v: [1]}])
--- value
[[1],{"v":[1]}]

=== pluck: an own key named toString
@shapes PLUCK(toString)
[{toString: 1}, {toString: "a"}].map(x => x.toString)
--- value
[1,"a"]

=== pluck: objects with many keys, and the key at different places
@shapes PLUCK(k)
[{a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, k: 10}, {k: 1}, {a: 0, k: 2}, {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9}].map(x => x.k)
--- value
[10,1,2,undefined]

=== pluck: strings, arrays and sets by length or size, in bytecode
@shapes PLUCK(length) PLUCK(size)
[["ab", [1, 2, 3], {length: 7}, "été", {}].map(s => s.length), [new Set([1, 2]), {size: 5}, {}].map(s => s.size)]
--- value
[[2,3,7,3,undefined],[2,5,undefined]]

=== pluck: a key that is an array index is not a pluck
@shapes -
[[5, 6], [7]].map(x => x[0])
--- value
[5,7]

=== pluck: an empty array
@shapes PLUCK(k)
[].map(x => x.k).length
--- value
0

=== pluck: a large array
@shapes - PLUCK(k)
[3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4, 3].map(n => ({k: n * 2})).map(x => x.k)
--- value
[6,2,8,2,10,18,4,12,10,6,10,16,18,14,18,6,4,6,16,8,12,4,12,8,6]

# map: faults, each the bytecode's own

=== pluck: a key found only on Object.prototype faults
@shapes PLUCK(toString)
[{toString: 1}, {k: 2}].map(x => x.toString)
--- fault staticjs objects have no prototype

=== pluck: constructor, on Object.prototype, faults
@shapes PLUCK(constructor)
[{}].map(x => x.constructor)
--- fault staticjs objects have no prototype

=== pluck: an undefined element faults
@shapes PLUCK(k)
[{k: 1}, undefined].map(x => x.k)
--- fault cannot read a property of undefined

=== pluck: a number element faults
@shapes PLUCK(k)
[{k: 1}, 2].map(x => x.k)
--- fault staticjs reads properties of objects, arrays, strings, sets, maps and iterators

=== pluck: a boolean element faults
@shapes PLUCK(k)
[true].map(x => x.k)
--- fault staticjs reads properties of objects, arrays, strings, sets, maps and iterators

=== pluck: a method name on an array faults
@shapes PLUCK(map)
[{map: 1}, [1]].map(x => x.map)
--- fault staticjs calls methods

# The predicate loops with KEY_COMPARE and ELEMENT_COMPARE (docs/matcher.md,
# 11.7): filter, find, findIndex, findLast, findLastIndex, some and every decide an
# element in C while its preconditions hold, and call the bytecode for it
# otherwise, which gives the answer or the fault.

# predicates: NaN, -0 and Infinity

=== predicates: a NaN key against each operator
@shapes KEY_COMPARE(k, STRICT_EQUAL, 1) KEY_COMPARE(k, STRICT_NOT_EQUAL, 0) KEY_COMPARE(k, LESS, 1) KEY_COMPARE(k, GREATER_EQUAL, 0) KEY_COMPARE(k, GREATER, 1)
[[{k: NaN}].some(x => x.k === 1), [{k: NaN}].some(x => x.k !== 0), [{k: NaN}].some(x => x.k < 1), [{k: NaN}].some(x => x.k >= 0), [{k: NaN}].some(x => 1 < x.k)]
--- value
[false,true,false,false,false]

=== predicates: NaN elements are never greater
@shapes ELEMENT_COMPARE(GREATER, 0) ELEMENT_COMPARE(LESS_EQUAL, 0)
[[NaN, 1, NaN, 2].filter(x => x > 0), [NaN, 0].filter(x => 0 >= x)]
--- value
[[1,2],[0]]

=== predicates: -0 equals 0
@shapes KEY_COMPARE(k, STRICT_EQUAL, 0) ELEMENT_COMPARE(STRICT_EQUAL, 0)
[[{k: -0}, {k: 1}].filter(x => x.k === 0), [-0].filter(x => 0 === x)]
--- value
[[{"k":-0}],[-0]]

=== predicates: find gives -0 itself
@shapes ELEMENT_COMPARE(GREATER_EQUAL, 0)
[-1, -0, 0].find(x => x >= 0)
--- value
-0

=== predicates: -0 is not less than 0, and is at most 0
@shapes ELEMENT_COMPARE(LESS, 0) ELEMENT_COMPARE(LESS_EQUAL, 0) ELEMENT_COMPARE(LESS, 0)
[[-0].some(x => x < 0), [-0].some(x => x <= 0), [-0].some(x => 0 > x)]
--- value
[false,true,false]

=== predicates: Infinity against numbers; a negative bound is a negation, not a literal
@shapes ELEMENT_COMPARE(GREATER, 1000000) -
[[-Infinity, 1, Infinity].filter(x => x > 1e6), [Infinity, -Infinity].findIndex(x => x < -1e21)]
--- value
[[Infinity],1]

# predicates: undefined and missing keys

=== predicates: a missing key reads as undefined
@shapes KEY_COMPARE(k, STRICT_EQUAL, 1) KEY_COMPARE(k, STRICT_NOT_EQUAL, null)
[[{}, {k: 1}].filter(x => x.k === 1), [{}, {k: null}].filter(x => x.k !== null)]
--- value
[[{"k":1}],[{}]]

=== predicates: a missing key against a number faults
@shapes KEY_COMPARE(k, LESS, 1)
[{k: 0}, {}].some(x => x.k < 1)
--- value
true

=== predicates: a missing key against a number faults when it is reached
@shapes KEY_COMPARE(k, LESS, 1)
[{k: 2}, {}].some(x => x.k < 1)
--- fault staticjs compares two numbers or two strings

=== predicates: undefined elements
@shapes ELEMENT_COMPARE(STRICT_EQUAL, 1) ELEMENT_COMPARE(STRICT_NOT_EQUAL, 1)
[[undefined, 1].filter(x => x === 1), [undefined, 1].filter(x => x !== 1)]
--- value
[[1],[undefined]]

=== predicates: undefined against a number faults
@shapes ELEMENT_COMPARE(GREATER, 0)
[1, undefined].filter(x => x > 0)
--- fault staticjs compares two numbers or two strings

=== predicates: a key on Object.prototype faults
@shapes KEY_COMPARE(toString, STRICT_EQUAL, 1)
[{toString: 1}, {}].filter(x => x.toString === 1)
--- fault staticjs objects have no prototype

=== predicates: an own key named as one on Object.prototype
@shapes KEY_COMPARE(toString, STRICT_EQUAL, 1)
[{toString: 1}, {toString: 2}].filter(x => x.toString === 1)
--- value
[{"toString":1}]

# predicates: mixed types, and strings against numbers

=== predicates: strict equality of mixed types never faults
@shapes ELEMENT_COMPARE(STRICT_EQUAL, 1) ELEMENT_COMPARE(STRICT_EQUAL, "1") ELEMENT_COMPARE(STRICT_NOT_EQUAL, null)
[[1, "1", true, null, {}, [1], undefined].filter(x => x === 1), [1, "1", true, null, {}, [1]].filter(x => x === "1"), [1, "1", null, {}, [1], new Set([1])].filter(x => null !== x).length]
--- value
[[1],["1"],5]

=== predicates: a key holding an object or an array is equal to no literal
@shapes KEY_COMPARE(k, STRICT_EQUAL, 1) KEY_COMPARE(k, STRICT_NOT_EQUAL, "a")
[[{k: {}}, {k: [1]}, {k: 1}].filter(x => x.k === 1), [{k: {}}, {k: [1]}, {k: "a"}].filter(x => x.k !== "a")]
--- value
[[{"k":1}],[{"k":{}},{"k":[1]}]]

=== predicates: a key holding an array is not ordered
@shapes KEY_COMPARE(k, LESS, 1)
[{k: 0}, {k: [1]}].some(x => x.k < 1)
--- value
true

=== predicates: a key holding an array faults when it is compared
@shapes KEY_COMPARE(k, LESS, 1)
[{k: 2}, {k: [1]}].some(x => x.k < 1)
--- fault staticjs compares two numbers or two strings

=== predicates: strings order by code units, not as numbers
@shapes ELEMENT_COMPARE(LESS, "9") ELEMENT_COMPARE(LESS, "9")
[["10", "9", "8"].filter(x => x < "9"), ["10", "9", "80"].filter(x => "9" > x)]
--- value
[["10","8"],["10","80"]]

=== predicates: a string against a number faults where JavaScript would compare
@shapes ELEMENT_COMPARE(LESS, 2)
[1, "a", 3].filter(x => x < 2)
--- fault staticjs compares two numbers or two strings

=== predicates: a number against a string faults
@shapes KEY_COMPARE(k, GREATER_EQUAL, "a")
[{k: "b"}, {k: 1}].filter(x => "a" <= x.k)
--- fault staticjs compares two numbers or two strings

=== predicates: strings order by UTF-16 code units, not by code points, both ways
@shapes ELEMENT_COMPARE(GREATER, "😀") ELEMENT_COMPARE(GREATER, "😀") KEY_COMPARE(s, LESS, "😀")
[["\u{1F600}", "\uFFFF"].findIndex(x => x > "\u{1F600}"), ["\u{1F600}", "\uFFFF"].findIndex(x => "\u{1F600}" < x), [{s: "\uFFFF"}, {s: "a"}].findIndex(x => x.s < "\u{1F600}")]
--- value
[1,1,1]

=== predicates: a boolean operand of an order runs the bytecode, which faults
@shapes ELEMENT_COMPARE(LESS, true)
[1].filter(x => x < true)
--- fault staticjs compares two numbers or two strings

=== predicates: a null operand of an order runs the bytecode, which faults
@shapes KEY_COMPARE(k, GREATER_EQUAL, null)
[{k: 1}].some(x => null <= x.k)
--- fault staticjs compares two numbers or two strings

=== predicates: an order on an empty array never compares
@shapes ELEMENT_COMPARE(LESS, false)
[].every(x => x < false)
--- value
true

# predicates: elements that are not objects, for a key

=== predicates: a null element faults
@shapes KEY_COMPARE(k, STRICT_EQUAL, 1)
[{k: 2}, null].filter(x => x.k === 1)
--- fault cannot read a property of null

=== predicates: a number element faults
@shapes KEY_COMPARE(k, STRICT_EQUAL, 1)
[{k: 2}, 3].find(x => x.k === 1)
--- fault staticjs reads properties of objects, arrays, strings, sets, maps and iterators

=== predicates: arrays and strings by length, sets by size, in bytecode
@shapes KEY_COMPARE(length, STRICT_EQUAL, 3) KEY_COMPARE(size, GREATER, 0)
[[[1, 2], "abc", {length: 3}, [1, 2, 3]].filter(x => x.length === 3), [new Set([1]), {size: 0}, new Set([]), {size: 2}].filter(x => x.size > 0)]
--- value
[["abc",{"length":3},[1,2,3]],[new Set([1]),{"size":2}]]

=== predicates: objects with an index
@shapes KEY_COMPARE(k, STRICT_NOT_EQUAL, 1)
[{a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, k: 10}, {k: 1}, {a: 0, k: 2}, {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9}, {k: 1, a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9}].filter(x => x.k !== 1).length
--- value
3

# predicates: empty arrays

=== predicates: each method on an empty array
@shapes KEY_COMPARE(k, STRICT_EQUAL, 1) KEY_COMPARE(k, STRICT_EQUAL, 1) ELEMENT_COMPARE(STRICT_EQUAL, 1) ELEMENT_COMPARE(GREATER, 1) KEY_COMPARE(k, LESS, 1) ELEMENT_COMPARE(GREATER_EQUAL, 1)
[[].filter(x => x.k === 1), [].find(x => x.k === 1), [].findIndex(x => x === 1), [].findLast(x => x > 1), [].some(x => x.k < 1), [].every(x => x >= 1)]
--- value
[[],undefined,-1,undefined,false,true]

# predicates: where they stop, and where they fault

=== predicates: find stops at its element, before a null
@shapes KEY_COMPARE(k, STRICT_EQUAL, 2)
[{k: 1}, {k: 2}, null].find(x => x.k === 2)
--- value
{"k":2}

=== predicates: findIndex stops at its element, before a null
@shapes KEY_COMPARE(k, STRICT_EQUAL, 2)
[{k: 1}, {k: 2}, null].findIndex(x => x.k === 2)
--- value
1

=== predicates: findLast stops at its element, from the end
@shapes KEY_COMPARE(k, STRICT_EQUAL, 2)
[null, {k: 2, i: 0}, {k: 2, i: 1}, {k: 3}].findLast(x => x.k === 2)
--- value
{"k":2,"i":1}

=== predicates: findLast gives undefined when nothing matches
@shapes ELEMENT_COMPARE(GREATER, 5)
[1, 2, 3].findLast(x => x > 5)
--- value
undefined

=== predicates: some stops before a faulting element
@shapes ELEMENT_COMPARE(GREATER, 1)
[1, 2, "a"].some(x => x > 1)
--- value
true

=== predicates: every stops at its first false
@shapes KEY_COMPARE(k, GREATER, 0)
[{k: 1}, {k: 0}, null].every(x => x.k > 0)
--- value
false

=== predicates: every is true when each element passes
@shapes ELEMENT_COMPARE(STRICT_NOT_EQUAL, "x")
["a", 1, null, undefined, {}].every(x => x !== "x")
--- value
true

=== predicates: some faults at a mixed comparison before a null
@shapes KEY_COMPARE(k, GREATER, 5)
[{k: 1}, {k: "a"}, null].some(x => x.k > 5)
--- fault staticjs compares two numbers or two strings

=== predicates: some faults at a null before a mixed comparison
@shapes KEY_COMPARE(k, GREATER, 5)
[{k: 1}, null, {k: "a"}].some(x => x.k > 5)
--- fault cannot read a property of null

=== predicates: findLast faults at a mixed comparison first, from the end
@shapes KEY_COMPARE(k, GREATER, 5)
[null, {k: "a"}, {k: 1}].findLast(x => x.k > 5)
--- fault staticjs compares two numbers or two strings

=== predicates: findLast faults at a null first, from the end
@shapes KEY_COMPARE(k, GREATER, 5)
[{k: "a"}, null, {k: 1}].findLast(x => x.k > 5)
--- fault cannot read a property of null

=== predicates: findLastIndex stops at its element, from the end
@shapes KEY_COMPARE(k, STRICT_EQUAL, 2)
[null, {k: 2, i: 0}, {k: 2, i: 1}, {k: 3}].findLastIndex(x => x.k === 2)
--- value
2

=== predicates: findLastIndex, mirrored and in a block, gives the index
@shapes KEY_COMPARE(k, LESS, 3) KEY_COMPARE(k, GREATER, 1)
[[{k: 1}, {k: 2}, {k: 5}].findLastIndex(x => 3 > x.k), [{k: 1}, {k: 2}, {k: 5}].findLastIndex(x => { return x.k > 1; })]
--- value
[1,2]

=== predicates: findLastIndex of an element comparison, plain and mirrored
@shapes ELEMENT_COMPARE(GREATER, 1) ELEMENT_COMPARE(LESS, 3)
[[1, 2, 3, 0].findLastIndex(x => x > 1), [1, 2, 3, 0].findLastIndex(x => 3 > x)]
--- value
[2,3]

=== predicates: findLastIndex gives -1 when nothing matches, or on an empty array
@shapes ELEMENT_COMPARE(GREATER, 5) ELEMENT_COMPARE(GREATER, 5) KEY_COMPARE(k, STRICT_EQUAL, 1)
[[1, 2, 3].findLastIndex(x => x > 5), [].findLastIndex(x => x > 5), [].findLastIndex(x => x.k === 1)]
--- value
[-1,-1,-1]

=== predicates: findLastIndex stops before a faulting element below it
@shapes KEY_COMPARE(k, STRICT_EQUAL, 2)
[null, {k: 2}].findLastIndex(x => x.k === 2)
--- value
1

=== predicates: findLastIndex faults at a null first, from the end
@shapes KEY_COMPARE(k, GREATER, 5)
[{k: "a"}, null, {k: 1}].findLastIndex(x => x.k > 5)
--- fault cannot read a property of null

=== predicates: findLastIndex faults at a mixed comparison first, from the end
@shapes KEY_COMPARE(k, GREATER, 5)
[null, {k: "a"}, {k: 1}].findLastIndex(x => x.k > 5)
--- fault staticjs compares two numbers or two strings

=== predicates: findLastIndex with a boolean operand of an order faults
@shapes ELEMENT_COMPARE(LESS, true)
[1].findLastIndex(x => x < true)
--- fault staticjs compares two numbers or two strings

=== predicates: findLastIndex with a key found only on Object.prototype faults
@shapes KEY_COMPARE(toString, STRICT_EQUAL, 1)
[{toString: 1}, {}].findLastIndex(x => x.toString === 1)
--- fault staticjs objects have no prototype

=== predicates: filter faults at the element, after keeping some
@shapes ELEMENT_COMPARE(GREATER_EQUAL, 2)
[3, 1, 2, null, 4].filter(x => x >= 2)
--- fault staticjs compares two numbers or two strings

=== predicates: a large array
@shapes - KEY_COMPARE(k, GREATER, 8) PLUCK(k)
[3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4, 3].map(n => ({k: n * 2})).filter(x => x.k > 8).map(x => x.k)
--- value
[10,18,12,10,10,16,18,14,18,16,12,12]

# predicates: the shape is a tag, and other methods run its bytecode

=== predicates: map runs a tagged predicate as bytecode
@shapes KEY_COMPARE(k, GREATER, 1) ELEMENT_COMPARE(STRICT_EQUAL, 1)
[[{k: 1}, {k: 2}].map(x => x.k > 1), [1, 2].map(x => x === 1)]
--- value
[[false,true],[true,false]]

=== predicates: a tagged predicate called directly
@shapes ELEMENT_COMPARE(LESS, 3)
(x => x < 3)(2)
--- value
true

# reduce with SUM and SUM_KEY (docs/matcher.md, 11.8): a number accumulator
# and a number element (or key) are added in C, anything else in bytecode,
# which concatenates or faults.

# sums: values

=== sums: with and without an initial value
@shapes SUM(false) SUM(false)
[[1, 2, 3].reduce((a, x) => a + x, 10), [1, 2, 3].reduce((a, x) => a + x)]
--- value
[16,6]

=== sums: one element and no initial value is never added
@shapes SUM(false) SUM(false) SUM_KEY(k, false)
[[7].reduce((a, x) => a + x), [null].reduce((a, x) => a + x), ["s"].reduce((a, x) => a + x.k)]
--- value
[7,null,"s"]

=== sums: an empty array with an initial value is the initial value
@shapes SUM(false) SUM_KEY(k, false)
[[].reduce((a, x) => a + x, 0), [].reduce((a, x) => a + x.k, "start")]
--- value
[0,"start"]

=== sums: an empty array without an initial value faults
@shapes SUM(false)
[].reduce((a, x) => a + x)
--- fault reduce of an empty array needs an initial value

=== sums: rounding follows the order of the additions
@shapes SUM(false) SUM(true) SUM(false) SUM(false)
[[0.1, 0.2, 0.3].reduce((a, x) => a + x, 0), [0.1, 0.2, 0.3].reduce((a, x) => x + a, 0), [1e16, 1, 1].reduce((a, x) => a + x), [1, 1, 1e16].reduce((a, x) => a + x)]
--- value
[0.6000000000000001,0.6000000000000001,10000000000000000,10000000000000002]

=== sums: -0 survives only -0
@shapes SUM(false) SUM(false) SUM(true) SUM(false) SUM(false)
[[-0].reduce((a, x) => a + x, -0), [-0].reduce((a, x) => a + x, 0), [-0, -0].reduce((a, x) => x + a), [-0].reduce((a, x) => a + x), [0, -0].reduce((a, x) => a + x)]
--- value
[-0,0,-0,-0,0]

=== sums: NaN, and Infinity with -Infinity
@shapes SUM(false) SUM(false) SUM(true)
[[1, NaN, 2].reduce((a, x) => a + x, 0), [Infinity, -Infinity].reduce((a, x) => a + x, 0), [Number.MAX_VALUE, Number.MAX_VALUE].reduce((a, x) => x + a)]
--- value
[NaN,NaN,Infinity]

=== sums: an initial string concatenates, in the bytecode's order
@shapes SUM(false) SUM(true) SUM(false)
[[1, 2].reduce((a, x) => a + x, ""), ["a", "b"].reduce((a, x) => x + a, ""), [1, 2].reduce((a, x) => a + x, "s")]
--- value
["12","ba","s12"]

=== sums: a string element turns the sum into a string
@shapes SUM(false) SUM(true) SUM(false)
[[1, 2, "3", 4].reduce((a, x) => a + x), [1, 2, "3", 4].reduce((a, x) => x + a), [1, "x", true].reduce((a, x) => a + x)]
--- value
["334","433","1xtrue"]

=== sums: a large array
@shapes - SUM(false) - SUM_KEY(k, true)
(xs => [xs.reduce((a, x) => a + x, 0), xs.map(n => ({k: n / 4})).reduce((a, x) => x.k + a, 0.5)])([3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4, 3])
--- value
[118,30]

=== sums: a key's values, the key at different places and with many keys
@shapes SUM_KEY(k, false)
[{k: 1}, {j: 0, k: 2}, {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, k: 4}, {k: 8, j: 0}].reduce((a, x) => a + x.k, 0)
--- value
15

=== sums: a key holding strings concatenates
@shapes SUM_KEY(k, false) SUM_KEY(k, false)
[[{k: "a"}, {k: "b"}].reduce((a, x) => a + x.k, ""), [{k: 1}, {k: "b"}, {k: 2}].reduce((a, x) => a + x.k, 0)]
--- value
["ab","1b2"]

=== sums: an own key named toString
@shapes SUM_KEY(toString, false)
[{toString: 2}, {toString: 3}].reduce((a, x) => a + x.toString, 1)
--- value
6

=== sums: arrays and strings by length, sets by size, in bytecode
@shapes SUM_KEY(length, false) SUM_KEY(size, true)
[[[1, 2], "abc", {length: 4}].reduce((a, x) => a + x.length, 0), [new Set([1]), {size: 2}].reduce((a, x) => x.size + a, 0)]
--- value
[9,3]

=== sums: a tagged sum in map and toSorted runs its bytecode
@shapes SUM(false) SUM(true)
[[1, 2].map((a, x) => a + x), [0, -0].toSorted((a, x) => x + a)]
--- value
[[1,3],[0,-0]]

=== sums: a tagged sum called directly
@shapes SUM(true) SUM_KEY(k, false)
[((a, x) => x + a)("a", "b"), ((a, x) => a + x.k)(1, {k: 2})]
--- value
["ba",3]

# sums: faults, each the bytecode's own

=== sums: a boolean element faults
@shapes SUM(false)
[1, 2, true].reduce((a, x) => a + x)
--- fault staticjs adds two numbers, or a string and a primitive

=== sums: null and undefined elements fault
@shapes SUM(false) SUM(true)
[[1, null].reduce((a, x) => a + x), [1, undefined].reduce((a, x) => x + a)]
--- fault staticjs adds two numbers, or a string and a primitive

=== sums: an array or an object element faults
@shapes SUM(false)
[1, [2]].reduce((a, x) => a + x)
--- fault staticjs adds two numbers, or a string and a primitive

=== sums: a string with an object faults
@shapes SUM(false)
["a", {}].reduce((a, x) => a + x)
--- fault staticjs adds two numbers, or a string and a primitive

=== sums: an initial value that is not a number or a string faults
@shapes SUM(false) SUM(false)
[[1].reduce((a, x) => a + x, undefined), [1].reduce((a, x) => a + x, null)]
--- fault staticjs adds two numbers, or a string and a primitive

=== sums: a missing key faults
@shapes SUM_KEY(k, false)
[{k: 1}, {}].reduce((a, x) => a + x.k, 0)
--- fault staticjs adds two numbers, or a string and a primitive

=== sums: a key found only on Object.prototype faults
@shapes SUM_KEY(toString, false)
[{toString: 1}, {}].reduce((a, x) => a + x.toString, 0)
--- fault staticjs objects have no prototype

=== sums: a null element faults for a key
@shapes SUM_KEY(k, false)
[{k: 1}, null].reduce((a, x) => a + x.k, 0)
--- fault cannot read a property of null

=== sums: a number element faults for a key
@shapes SUM_KEY(k, true)
[{k: 1}, 2].reduce((a, x) => x.k + a, 0)
--- fault staticjs reads properties of objects, arrays, strings, sets, maps and iterators

=== sums: the first bad key faults, before a null
@shapes SUM_KEY(k, false)
[{k: 1}, {k: true}, null].reduce((a, x) => a + x.k, 0)
--- fault staticjs adds two numbers, or a string and a primitive

=== sums: the first null faults, before a bad key
@shapes SUM_KEY(k, false)
[{k: 1}, null, {k: true}].reduce((a, x) => a + x.k, 0)
--- fault cannot read a property of null
