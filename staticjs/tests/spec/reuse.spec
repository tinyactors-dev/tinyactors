# Values updated in place when nothing else holds them: the last read of a
# local moves its value, reduce moves its accumulator, a literal that begins
# with a spread takes over a source nothing else holds, and concat, with and
# toSorted take over a receiver nothing else holds. Each case tries to see
# the change: through another name, a closure, a global, a module, or the
# receiver of a loop.

=== spreading the accumulator of reduce
["a", "b", "c"].reduce((acc, k, i) => ({ ...acc, [k]: i }), {})
--- value
{"a":0,"b":1,"c":2}

=== a key put again keeps its place
[["a", 1], ["b", 2], ["a", 3]].reduce((acc, [k, v]) => ({ ...acc, [k]: v }), {})
--- value
{"a":3,"b":2}

=== array indices come first, ascending, in an object taken over
["b", "2", "a", "1"].reduce((acc, k) => ({ ...acc, [k]: k }), {})
--- value
{"1":"1","2":"2","b":"b","a":"a"}

=== an index lower than those there goes before them
[2, 1, 0].reduce((acc, i) => ({ ...acc, [i]: i }), { a: 1 })
--- value
{"0":0,"1":1,"2":2,"a":1}

=== indices and names put into an accumulator
["x", "y"].reduce((acc, k, i) => ({ ...acc, [i]: k, [k]: i }), {})
--- value
{"0":"x","1":"y","x":0,"y":1}

=== a second spread after the accumulator
[{a: 1}, {b: 2, a: 3}].reduce((acc, o) => ({ ...acc, ...o, n: 0 }), {z: 0})
--- value
{"z":0,"a":3,"n":0,"b":2}

=== a parameter spread, then its keys put again
((o) => ({ ...o, a: 2, b: 3, a: 4 }))({ b: 1, a: 1 })
--- value
{"b":3,"a":4}

=== the accumulator read after it is spread
[1, 2].reduce((acc, x) => { const next = { ...acc, [x]: true }; return { ...next, before: Object.keys(acc).length }; }, {})
--- value
{"1":true,"2":true,"before":2}

=== the accumulator captured, and read after the put
["a", "b"].reduce((acc, x) => { const count = () => Object.keys(acc).length; return { ...acc, [x]: 1, n: count() }; }, {})
--- value
{"a":1,"n":2,"b":1}

=== the accumulator captured by a closure made after the spread
(() => { const r = [1, 2].reduce((acc, x) => ({ ...acc, [x]: () => Object.keys(acc).length }), {}); return [r[1](), r[2]()]; })()
--- value
[0,1]

=== a parameter captured before it is spread
((o) => { const peek = () => o.a; const next = { ...o, a: 2 }; return [peek(), next.a]; })({ a: 1 })
--- value
[1,2]

=== a closure made before the spread returns the value
(() => { const a = { x: 1 }; const f = () => a; const b = { ...a, y: 2 }; return [f(), b]; })()
--- value
[{"x":1},{"x":1,"y":2}]

=== a captured value spread inside the closure
((o) => { const g = (k) => ({ ...o, [k]: 1 }); return [g("a"), g("b"), o]; })({ z: 0 })
--- value
[{"z":0,"a":1},{"z":0,"b":1},{"z":0}]

=== the initial value kept in a const
(() => { const base = { a: 1 }; const r = [2, 3].reduce((acc, x) => ({ ...acc, [x]: x }), base); return [base, r]; })()
--- value
[{"a":1},{"2":2,"3":3,"a":1}]

=== the initial value's last read
(() => { const base = { a: 1 }; return [2].reduce((acc, x) => ({ ...acc, [x]: x }), base); })()
--- value
{"2":2,"a":1}

=== a value returned before its spread
((o) => [o, { ...o, b: 2 }])({ a: 1 })
--- value
[{"a":1},{"a":1,"b":2}]

=== a value returned after its spread
((o) => [{ ...o, b: 2 }, o])({ a: 1 })
--- value
[{"a":1,"b":2},{"a":1}]

=== the same value spread twice
((o) => ({ ...o, ...o, b: 1 }))({ a: 1 })
--- value
{"a":1,"b":1}

=== a value spread and put into its own copy
((o) => ({ ...o, self: o }))({ a: 1 })
--- value
{"a":1,"self":{"a":1}}

=== the accumulator put into the next one
[1, 2].reduce((acc) => ({ ...acc, prev: acc }), {})
--- value
{"prev":{"prev":{}}}

=== the accumulator through another const
[1, 2].reduce((acc, x) => { const same = acc; return { ...same, [x]: x }; }, {})
--- value
{"1":1,"2":2}

=== the accumulator in one branch of a conditional
[1, 2, 3].reduce((acc, x) => x % 2 === 0 ? acc : { ...acc, [x]: x }, {})
--- value
{"1":1,"3":3}

=== the accumulator spread in the first branch, returned in the second
[1, 2, 3].reduce((acc, x) => x % 2 === 0 ? { ...acc, [x]: x } : acc, {})
--- value
{"2":2}

=== the accumulator spread in two returns
[1, 2, 3].reduce((acc, x) => { if (x === 2) { return { ...acc, two: true }; } return { ...acc, [x]: x }; }, {})
--- value
{"1":1,"3":3,"two":true}

=== the accumulator in block consts of both branches
[1, 2].reduce((acc, x) => { if (x > 0) { const a = { ...acc, [x]: x }; return a; } else { const b = acc; return b; } }, {})
--- value
{"1":1,"2":2}

=== a block's const, and a const after the block in its slot
((o) => { { const a = { ...o, b: 2 }; if (a.b > 5) { return a; } } const c = { z: 1 }; return [o, c]; })({ a: 1 })
--- value
[{"a":1},{"z":1}]

=== a destructured element spread
[[1, 2]].reduce((acc, [k, v]) => ({ ...acc, [k]: v }), {})
--- value
{"1":2}

=== an element of a rest parameter spread
((...args) => ({ ...args[0], b: 2 }))({ a: 1 })
--- value
{"a":1,"b":2}

=== an accumulator with a default value
[1, 2].reduce((acc = {}, x) => ({ ...acc, [x]: x }), undefined)
--- value
{"1":1,"2":2}

=== a default value that reads an earlier parameter
((o, p = { ...o, b: 2 }) => [o, p])({ a: 1 })
--- value
[{"a":1},{"a":1,"b":2}]

=== an object rest element of the accumulator
[1, 2].reduce(({ ...acc }, x) => ({ ...acc, [x]: x }), { a: 0 })
--- value
{"1":1,"2":2,"a":0}

=== reduce without an initial value: the first element stays in the array
[{ a: 1 }, { b: 2 }, { c: 3 }].reduce((acc, o) => ({ ...acc, ...o }))
--- value
{"a":1,"b":2,"c":3}

=== reduce without an initial value, the array read after
((xs) => [xs.reduce((acc, o) => ({ ...acc, ...o })), xs[0]])([{ a: 1 }, { b: 2 }])
--- value
[{"a":1,"b":2},{"a":1}]

=== a nested accumulator, as water-filling has it
[["a", 1], ["b", 2]].reduce((acc, [k, v]) => ({ n: acc.n + 1, out: { ...acc.out, [k]: v } }), { n: 0, out: {} })
--- value
{"n":2,"out":{"a":1,"b":2}}

=== a property of a value still held
(() => { const s = { out: { a: 1 } }; const t = { ...s.out, b: 2 }; return [s, t]; })()
--- value
[{"out":{"a":1}},{"a":1,"b":2}]

=== a spread of a value a callback returns
[{ a: 1 }].map((o) => ({ ...o })).map((o) => ({ ...o, b: 2 }))
--- value
[{"a":1,"b":2}]

=== spreading the accumulator of reduce into an array
[1, 2, 3].reduce((acc, x) => [...acc, x * 2], [])
--- value
[2,4,6]

=== an array spread, then read again
(() => { const xs = [1]; const ys = [...xs, 2]; return [xs, ys]; })()
--- value
[[1],[1,2]]

=== an array spread twice
((xs) => [...xs, ...xs])([1])
--- value
[1,1]

=== an array accumulator read after its spread
[[1], [2]].reduce((acc, xs) => [...acc, ...xs, acc.length], [])
--- value
[1,0,2,2]

=== a set spread into an array is copied
((s) => [...s, 3])(new Set([1, 2]))
--- value
[1,2,3]

=== a spread after an element
((xs) => [0, ...xs])([1, 2])
--- value
[0,1,2]

=== concat onto the accumulator
[1, 2, 3].reduce((acc, x) => acc.concat([x], x), [])
--- value
[1,1,2,2,3,3]

=== concat of an array still held
(() => { const a = [1]; const b = a.concat(2); return [a, b]; })()
--- value
[[1],[1,2]]

=== concat of an array with itself
((a) => a.concat(a))([1, 2])
--- value
[1,2,1,2]

=== with on the accumulator
[0, 1, 2].reduce((acc, x) => acc.with(x, x * 10), [0, 0, 0])
--- value
[0,10,20]

=== with on an array still held
(() => { const a = [1, 2]; const b = a.with(0, 9); return [a, b]; })()
--- value
[[1,2],[9,2]]

=== with of an array into itself
((a) => a.with(0, a))([1, 2])
--- value
[[1,2],2]

=== with a negative index on the accumulator
[5].reduce((acc, x) => acc.with(-1, x), [1, 2])
--- value
[1,5]

=== toSorted of an array nothing else holds
[3, 1, 2].map(x => x).toSorted((a, b) => a - b)
--- value
[1,2,3]

=== toSorted of an array still held
(() => { const a = [3, 1, 2]; const b = a.toSorted((x, y) => x - y); return [a, b]; })()
--- value
[[3,1,2],[1,2,3]]

=== toSorted whose comparator holds the array
((a) => a.toSorted((x, y) => (a[0] === 3 ? x - y : 0)))([3, 1, 2])
--- value
[1,2,3]

=== toSorted of an empty array nothing else holds
[].map(x => x).toSorted((a, b) => a - b)
--- value
[]

=== toSorted with undefined last, nothing else holding the array
[3, undefined, 1].map(x => x).toSorted((a, b) => a - b)
--- value
[1,3,undefined]

=== a fault in with after the accumulator moved
[1].reduce((acc) => acc.with(5, 0), [1, 2])
--- fault with() needs an index inside the array

=== a fault after the accumulator was taken over
[1, 2].reduce((acc, x) => ({ ...acc, [x]: x, bad: [x][1].z }), {})
--- fault cannot read a property of undefined

=== a fault in the comparator of toSorted on an array nothing else holds
[3, 1, 2].map(x => x).toSorted((a, b) => [a][1].c)
--- fault cannot read a property of undefined

=== globals are never changed
@global g = {a: 1}
@global xs = [1, 2]
[{ ...g, b: 2 }, [...xs, 3], xs.concat(4), xs.with(0, 9), xs.toSorted((a, b) => b - a), g, xs]
--- value
[{"a":1,"b":2},[1,2,3],[1,2,4],[9,2],[2,1],{"a":1},[1,2]]

=== a global as the initial value of reduce
@global init = {a: 1}
[[2].reduce((acc, x) => ({ ...acc, [x]: x }), init), init]
--- value
[{"2":2,"a":1},{"a":1}]

=== a module's exports are read at its end
@module reuse
export const o = { a: 1 };
export const xs = [1, 2];
export const p = { ...o, b: 2 };
export const ys = [...xs, 3];
--- value
{"o":{"a":1},"p":{"a":1,"b":2},"xs":[1,2],"ys":[1,2,3]}

=== a module's const spread, and exported under two names
@module reuse_twice
const o = { a: 1 };
export const p = { ...o, b: 2 };
export { o, o as q };
--- value
{"o":{"a":1},"p":{"a":1,"b":2},"q":{"a":1}}

=== imported values are never changed
@module reuse_spread
import { o, xs } from "reuse";
export const all = [{ ...o, c: 3 }, [...xs, 4], xs.concat(5), xs.with(0, 9), xs.toSorted((a, b) => b - a), o, xs];
--- value
{"all":[{"a":1,"c":3},[1,2,4],[1,2,5],[9,2],[2,1],{"a":1},[1,2]]}

=== imports read again after a module spread them
@module reuse_again
import { o, xs } from "reuse";
import * as ns from "reuse";
export const again = [o, xs, { ...ns, z: 1 }];
--- value
{"again":[{"a":1},[1,2],{"o":{"a":1},"p":{"a":1,"b":2},"xs":[1,2],"ys":[1,2,3],"z":1}]}

# More holders and shapes: each case below gives the same result with or
# without reuse, and would differ if a value something else holds changed.

=== a closure called twice over a captured const
(() => { const c = { x: 1 }; const g = () => ({ ...c, y: 2 }); return [g(), g(), c]; })()
--- value
[{"x":1,"y":2},{"x":1,"y":2},{"x":1}]

=== a function called twice on one const
(() => { const f = (o) => ({ ...o, a: 1 }); const x = { b: 1 }; return [f(x), f(x), x]; })()
--- value
[{"b":1,"a":1},{"b":1,"a":1},{"b":1}]

=== the same object under two keys
(() => { const v = { a: 1 }; const o = { p: v, q: v }; return [{ ...o.p, b: 2 }, o]; })()
--- value
[{"a":1,"b":2},{"p":{"a":1},"q":{"a":1}}]

=== chained parameter defaults
((a = { x: 1 }, b = { ...a, y: 2 }, c = [...Object.keys(b), "z"]) => [a, b, c])()
--- value
[{"x":1},{"x":1,"y":2},["x","y","z"]]

=== a closure two levels deep over a parameter
((acc) => { const g = () => () => acc; return [{ ...acc, k: 1 }, g()()]; })({ x: 1 })
--- value
[{"x":1,"k":1},{"x":1}]

=== a destructured source spread afterwards
((o) => { const { a, ...rest } = o; return [{ ...o, z: 1 }, rest, a]; })({ a: 1, b: 2 })
--- value
[{"a":1,"b":2,"z":1},{"b":2},1]

=== an array destructured, then spread
((xs) => { const [h, ...t] = xs; return [[...xs, 1], h, t]; })([7, 8])
--- value
[[7,8,1],7,[8]]

=== -0 put again over a key the source has
((o) => ({ ...o, a: -0 }))({ a: 1, b: 2 })
--- value
{"a":-0,"b":2}

=== the key -0 is the key "0"
((o) => ({ ...o, [-0]: 9 }))({ a: 2, 0: 1 })
--- value
{"0":9,"a":2}

=== keys that look like indices and are not
["01", "-1", "4294967295", "4294967294", "1", "b"].reduce((acc, k) => ({ ...acc, [k]: 0 }), { 2: 0, a: 0 })
--- value
{"1":0,"2":0,"4294967294":0,"a":0,"01":0,"-1":0,"4294967295":0,"b":0}

=== an array spread first into an object literal
((xs) => ({ ...xs, a: 1, 0: "z" }))(["x", "y"])
--- value
{"0":"z","1":"y","a":1}

=== a string spread first into an object literal
((s) => ({ ...s, a: 1 }))("hi")
--- fault does not spread a string into an object

=== __proto__ put after a spread taken over
((o) => ({ ...o, ["__proto__"]: 1 }))({ a: 1 })
--- fault no prototype

=== __proto__ put into the accumulator
["a", "__proto__"].reduce((acc, k) => ({ ...acc, [k]: 1 }), {})
--- fault no prototype

=== the receiver of map spread in its callback
[1, 2].map((x) => x).map((x, i, a) => [...a, x].length)
--- value
[3,3]

=== the receiver of filter concatenated in its callback
[1, 2].map((x) => x).filter((x, i, a) => a.concat(x).length === 3)
--- value
[1,2]

=== the receiver of flatMap spread in its callback
[[1]].map((x) => x).flatMap((x, i, a) => [...a, x])
--- value
[[1],[1]]

=== the receiver of reduce changed with with() in its callback
[1, 2].map((x) => x).reduce((acc, x, i, a) => [...acc, a.with(i, 0)], [])
--- value
[[0,2],[1,0]]

=== the receiver of toSorted read by its comparator
[3, 1, 2].map((x) => x).toSorted((p, q) => p - q).map((x, i, a) => a.concat(x).length)
--- value
[4,4,4]

=== elements of an array given to map's callback
(() => { const objs = [{ a: 1 }, { a: 2 }]; return [objs.map((o) => ({ ...o, b: 1 })), objs]; })()
--- value
[[{"a":1,"b":1},{"a":2,"b":1}],[{"a":1},{"a":2}]]

=== the element find gives
(() => { const objs = [{ a: 1 }]; const e = objs.find((o) => true); return [{ ...e, b: 1 }, objs]; })()
--- value
[{"a":1,"b":1},[{"a":1}]]

=== an accumulator put into itself by a spread
[1, 2].reduce((acc) => [...acc, acc], [])
--- value
[[],[[]]]

=== an accumulator concatenated into itself
[1, 2].reduce((acc) => acc.concat([acc]), [])
--- value
[[],[[]]]

=== the initial value of reduce is its receiver
(() => { const xs = [1, 2]; const r = xs.reduce((acc, x) => acc.concat([x]), xs); return [r, xs]; })()
--- value
[[1,2,1,2],[1,2]]

=== reduce gives back its receiver
(() => { const xs = [1, 2]; const r = xs.reduce((acc, x, i, a) => a, 0); return [r.concat([3]), r, xs]; })()
--- value
[[1,2,3],[1,2],[1,2]]

=== nested reduces thread one accumulator
[[1, 2], [3]].reduce((acc, xs) => xs.reduce((a, x) => [...a, x], acc), [])
--- value
[1,2,3]

=== closures over each accumulator
[1, 2, 3].reduce((acc, x) => [...acc, () => acc], []).map((f, i) => f().length)
--- value
[0,1,2]

# Reading a member: the value outlives an object only the stack held, and a
# key made at run time finds the same property as the literal's.
=== a member of an object nothing else holds
({ a: [1, 2], b: { c: "x" } }).b.c
--- value
"x"

=== an array member outlives its object
({ a: [1, 2] }).a
--- value
[1,2]

=== a key made at run time is the same key
(k => ({ [k + "b"]: 1, ab2: 2 }).ab)("a")
--- value
1

=== a member the object lacks is undefined
(o => o.b)({ a: 1 })
--- value
undefined

=== one member read finds its key at a different place in each object
[{ a: 1, b: 2 }, { b: 3 }, { c: 1, d: 0, b: 4 }, {}, { a: 5 }, { b: 6, a: 7 }, { ba: 8, b: 9 }].map(o => o.b)
--- value
[2,3,4,undefined,undefined,6,9]

=== a key made at run time is found where a literal's key was
[{ x: 1, ab: 2 }, { ["a" + "b"]: 3, x: 4 }, { x: 5, [`a${"b"}`]: 6 }].map(o => o.ab)
--- value
[2,3,6]

=== a large object read where a small one had its key
[{ k: 1 }, Object.fromEntries([..."abcdefghijklmnop"].map((c, i) => [c, i]).concat([["k", 99]]))].map(o => o.k)
--- value
[1,99]

=== a missing member still faults on prototype names
(o => o.constructor)({ a: 1 })
--- fault no prototype

=== a callback that spreads its parameter leaves the receiver's elements alone
(xs => [xs.map(a => [...a, 0]), xs])([[1], [2]])
--- value
[[[1,0],[2,0]],[[1],[2]]]

=== a callback that spreads its object parameter leaves the receiver's elements alone
(xs => [xs.map(o => ({...o, n: 0})), xs])([{a: 1}, {b: 2}])
--- value
[[{"a":1,"n":0},{"b":2,"n":0}],[{"a":1},{"b":2}]]

=== a callback that moves a local leaves the next element's alone
[[1], [2]].map(a => { const b = a; return [...b, 9]; })
--- value
[[1,9],[2,9]]

=== reduce's initial value, held elsewhere, is not updated in place
(init => [[1, 2].reduce((acc, x) => [...acc, x], init), init])([])
--- value
[[1,2],[]]
