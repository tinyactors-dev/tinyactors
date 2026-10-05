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

=== numbers: an addition, not a subtraction
@shapes -
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
--- fault staticjs reads properties of objects, arrays, strings and sets

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
--- fault staticjs reads properties of objects, arrays, strings and sets

=== pluck: a boolean element faults
@shapes PLUCK(k)
[true].map(x => x.k)
--- fault staticjs reads properties of objects, arrays, strings and sets

=== pluck: a method name on an array faults
@shapes PLUCK(map)
[{map: 1}, [1]].map(x => x.map)
--- fault staticjs calls methods
