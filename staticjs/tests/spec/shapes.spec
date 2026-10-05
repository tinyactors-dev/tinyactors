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
