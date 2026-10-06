# The built-in functions and methods.

=== map and filter
[1, 2, 3, 4].filter(x => x % 2 === 0).map(x => x * x)
--- value
[4,16]

=== map passes the index and the array
["a", "b"].map((x, i, xs) => x + i + xs.length)
--- value
["a02","b12"]

=== reduce with an initial value
[1, 2, 3].reduce((sum, x) => sum + x, 10)
--- value
16

=== reduce without an initial value starts at the first element
[[1], [2], [3]].reduce((acc, x) => [...acc, ...x])
--- value
[1,2,3]

=== reduce of an empty array needs an initial value
[].reduce((a, b) => a + b)
--- fault initial value

=== reduce of an empty array with one
[].reduce((a, b) => a + b, "start")
--- value
"start"

=== some and every
[[1, 2].some(x => x > 1), [1, 2].every(x => x > 1), [].some(x => true), [].every(x => false)]
--- value
[true,false,false,true]

=== find, findIndex and findLast
[[5, 12, 8].find(x => x > 6), [5, 12, 8].findIndex(x => x > 6), [5, 12, 8].findLast(x => x > 6), [1].find(x => x > 6), [1].findIndex(x => x > 6)]
--- value
[12,1,8,undefined,-1]

=== findLastIndex walks from the end
[[5, 12, 8].findLastIndex(x => x > 6), [5, 12, 8].findLastIndex(x => x > 20), [].findLastIndex(x => true), [7, 7].findLastIndex(x => x === 7)]
--- value
[2,-1,-1,1]

=== findLastIndex gives the callback the element, index and array
[10, 20, 30].findLastIndex((x, i, xs) => x === 20 && i === 1 && xs.length === 3)
--- value
1

=== findLastIndex visits from the end, so the last match wins
["a", "b", "c", "b"].findLastIndex((x, i) => x === "b" && i < 3)
--- value
1

=== findLastIndex faults on a callback that is not a function
[1].findLastIndex(1)
--- fault not a function

=== findLastIndex faults without a callback, even on an empty array
[].findLastIndex()
--- fault not a function

=== findLastIndex takes the truthiness of the answer
[[0, 1, 2].findLastIndex(x => x), [0, 1, 2].findLastIndex(x => ''), [NaN, 0, -0].findLastIndex(x => x === 0), [NaN].findLastIndex(x => x !== x)]
--- value
[2,-1,2,0]

=== flatMap flattens one level
[1, 2].flatMap(x => [x, [x * 10]])
--- value
[1,[10],2,[20]]

=== flatMap keeps values that are not arrays
[1, 2].flatMap(x => x)
--- value
[1,2]

=== flat
[[1, 2], 3, [[4]]].flat()
--- value
[1,2,3,[4]]

=== toSorted with a comparator
[3, 1, 10, 2].toSorted((a, b) => a - b)
--- value
[1,2,3,10]

=== toSorted is stable
[{k: 1, v: "a"}, {k: 0, v: "b"}, {k: 1, v: "c"}, {k: 0, v: "d"}].toSorted((x, y) => x.k - y.k).map(e => e.v)
--- value
["b","d","a","c"]

=== toSorted puts undefined last without asking the comparator
[3, undefined, 1].toSorted((a, b) => a - b)
--- value
[1,3,undefined]

=== toSorted of many elements
[9, 3, 7, 1, 8, 2, 6, 4, 5, 0, 11, 10].toSorted((a, b) => b - a)
--- value
[11,10,9,8,7,6,5,4,3,2,1,0]

=== sorting strings with a comparator
["pear", "apple", "fig"].toSorted((a, b) => a < b ? -1 : a > b ? 1 : 0)
--- value
["apple","fig","pear"]

=== toSorted needs a comparator in staticjs
[2, 1].toSorted()
--- fault compare function

=== toReversed, slice, at, with, concat
[[1, 2, 3].toReversed(), [1, 2, 3, 4].slice(1, -1), [1, 2, 3].slice(-2), [1, 2, 3].at(-1), [1, 2, 3].with(0, 9), [1].concat([2, 3], 4)]
--- value
[[3,2,1],[2,3],[2,3],3,[9,2,3],[1,2,3,4]]

=== includes and indexOf
[[1, NaN].includes(NaN), [1, NaN].indexOf(NaN), ["a", "b"].indexOf("b"), [1].includes(2)]
--- value
[true,-1,1,false]

=== lastIndexOf finds the last strict match
[[1, 2, 1].lastIndexOf(1), [1, NaN].lastIndexOf(NaN), [0].lastIndexOf(-0), ["a", "b", "a"].lastIndexOf("a"), [1].lastIndexOf(2), [1, "1"].lastIndexOf("1"), [null, undefined].lastIndexOf(undefined)]
--- value
[2,-1,0,2,-1,1,1]

=== lastIndexOf on an empty array
[[].lastIndexOf(1), [].lastIndexOf(1, 5), [].lastIndexOf(1, -5)]
--- value
[-1,-1,-1]

=== lastIndexOf with a fromIndex
[[1, 2, 1, 2].lastIndexOf(1, 1), [1, 2, 1, 2].lastIndexOf(2, -2), [1, 2, 1].lastIndexOf(1, -3), [1, 2, 1].lastIndexOf(2, -4), [1, 2, 1].lastIndexOf(1, 99), [1, 2, 1].lastIndexOf(1, 1.9), [1, 2, 1].lastIndexOf(1, -Infinity), [1, 2, 1].lastIndexOf(1, Infinity), [1, 2, 1].lastIndexOf(1, NaN), [1, 2].lastIndexOf(1, -0)]
--- value
[0,1,0,-1,2,0,-1,2,0,0]

=== lastIndexOf takes the fromIndex a number would give
[[1, 2, 1].lastIndexOf(1, undefined), [1, 2, 1].lastIndexOf(1, null), [1, 2, 1].lastIndexOf(2, true), [1, 2, 1].lastIndexOf(1, 1e15), [1, 2, 1].lastIndexOf(1, -1e15), [1, 2, 1].lastIndexOf(1, 0), ["é", "ü", "é"].lastIndexOf("é", -2)]
--- value
[0,0,1,2,-1,0,0]

=== lastIndexOf faults on an object
[[1].lastIndexOf({})]
--- fault identity

=== lastIndexOf faults on an array
[[1].lastIndexOf([1])]
--- fault identity

=== lastIndexOf faults on a set
[[1].lastIndexOf(new Set())]
--- fault identity

=== lastIndexOf faults on an object even when the array is empty
[].lastIndexOf({})
--- fault identity

=== lastIndexOf faults on a fromIndex that would be coerced
[1].lastIndexOf(1, "0")
--- fault number

=== lastIndexOf faults on an object even when fromIndex skips every element
[1, 2].lastIndexOf({}, -9)
--- fault identity

=== lastIndexOf on a string still faults
"abc".lastIndexOf("c")
--- fault does not have this method

=== join
[[1, null, "a", undefined, true].join(), [1, 2].join(" - "), [].join()]
--- value
["1,,a,,true","1 - 2",""]

=== the original array does not change
(xs => { const sorted = xs.toSorted((a, b) => a - b); return [xs, sorted]; })([2, 1])
--- value
[[2,1],[1,2]]

=== a mutating method of a literal
[1, 2].push(3)
--- fault only on a variable

=== reading a method without calling it
[1].map
--- fault calls methods

=== reading a string method staticjs does not have
"x".normalize
--- fault calls methods

=== typeof a string method staticjs does not have
typeof "x".localeCompare
--- fault calls methods

=== reading an array method staticjs does not have
[1].forEach
--- fault calls methods

=== reading a set method staticjs does not have
new Set([1]).isDisjointFrom
--- fault calls methods

=== in finds an array method staticjs does not have
"forEach" in []
--- fault no prototype

=== in finds a mutating array method
"push" in [1]
--- fault no prototype

=== a pattern default is not taken for a string method
(({at = 1}) => at)("x")
--- fault calls methods

=== a computed pattern key that names a string method
((k) => (({[k]: m = 1}) => m)("x"))("at")
--- fault calls methods

=== a const pattern over an array method
(xs => { const {forEach = 0} = xs; return forEach; })([])
--- fault calls methods

=== a pattern reading a set method
(({entries}) => typeof entries)(new Set())
--- fault calls methods

=== string methods
["hello".startsWith("he"), "hello".endsWith("lo"), "hello".includes("ell"), "héllo".indexOf("l"), "x".indexOf("y")]
--- value
[true,true,true,2,-1]

=== slicing strings by UTF-16 index
["héllo".slice(1, 3), "abc".slice(-2), "😀x".slice(2)]
--- value
["él","bc","x"]

=== split
["a,b,,c".split(","), "abc".split(""), "abc".split(), "a--b".split("--")]
--- value
[["a","b","","c"],["a","b","c"],["abc"],["a","b"]]

=== case and trim
["Hello".toUpperCase(), "Hello".toLowerCase(), "  padded \n".trim()]
--- value
["HELLO","hello","padded"]

=== set methods
(a => [a.has(1), a.has(9), a.size, a.union(new Set([4, 1])), a.intersection(new Set([3, 2, 7])), a.difference(new Set([2])), a.symmetricDifference(new Set([3, 4])), a.isSubsetOf(new Set([1, 2, 3, 4]))])(new Set([1, 2, 3]))
--- value
[true,false,3,new Set([1,2,3,4]),new Set([2,3]),new Set([1,3]),new Set([1,2,4]),true]

=== intersection follows the smaller set
new Set([1, 2, 3, 4]).intersection(new Set([4, 3]))
--- value
new Set([4,3])

=== a large set finds its elements by SameValueZero
(s => [s.size, s.has(NaN), s.has(-0), s.has(0), s.has("7"), s.has(7), s.has(true), s.has(false), s.has(null), s.has(undefined), s.has(99), s.has("a"), s.has("b")])(new Set([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, NaN, 0, -0, NaN, "7", true, null, undefined, "a", "a"]))
--- value
[17,true,true,true,true,true,true,false,true,true,false,true,false]

=== set methods on large sets
(a => [a.intersection(new Set([19, 3, 40, 0, 5, 6, 7, 8, 9, 10])), a.difference(new Set([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18])), a.isSubsetOf(a.union(new Set(["x"]))), a.union(new Set([-0, 25, 3])).size, a.symmetricDifference(new Set([0, 1, 2, 3, 4, 5, 6, 7, 8, 30]))])(new Set([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19]))
--- value
[new Set([19,3,0,5,6,7,8,9,10]),new Set([19]),true,21,new Set([9,10,11,12,13,14,15,16,17,18,19,30])]

=== a large object finds its keys, and keeps index keys first
(o => [o.k1, o.k3, o.k10, o.k11, o[2], o[5], o[9], "k12" in o, "k11" in o, o.k99, Object.keys(o)])({k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7, k8: 8, k9: 9, k10: 10, 5: "a", 2: "b", k3: "again", 9: "c", k11: 11})
--- value
[1,"again",10,11,"b","a","c",false,true,undefined,["2","5","9","k1","k2","k3","k4","k5","k6","k7","k8","k9","k10","k11"]]

=== a large object from entries, spread and added to
(o => ({...o, k2: "x", k20: 20}))(Object.fromEntries([["k1", 1], ["k2", 2], ["k3", 3], ["k4", 4], ["k5", 5], ["k6", 6], ["k7", 7], ["k8", 8], ["k9", 9], ["k10", 10], ["k1", "dup"]]))
--- value
{"k1":"dup","k2":"x","k3":3,"k4":4,"k5":5,"k6":6,"k7":7,"k8":8,"k9":9,"k10":10,"k20":20}

=== a large object built by spreading in reduce
(o => [o.a, o.m, o.t, o.z, Object.keys(o).length])(["a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "a"].reduce((acc, k, i) => ({...acc, [k]: i}), {}))
--- value
[20,12,19,undefined,20]

=== Math
[Math.max(1, 5, 3), Math.min(), Math.max(), Math.abs(-2), Math.floor(-1.5), Math.ceil(-1.5), Math.round(2.5), Math.round(-2.5), Math.round(-0.4), Math.trunc(-1.7), Math.sign(-3), Math.max(1, NaN)]
--- value
[5,Infinity,-Infinity,2,-2,-1,3,-2,-0,-1,-1,NaN]

=== Math.min and max order zeros
[Math.min(0, -0), Math.max(-0, 0)]
--- value
[-0,0]

=== Math constants
[Math.PI, Math.E, Math.SQRT2]
--- value
[3.141592653589793,2.718281828459045,1.4142135623730951]

=== spreading into Math.max
Math.max(...[3, 9, 4])
--- value
9

=== Object
[Object.keys({b: 1, a: 2}), Object.values({b: 1, a: 2}), Object.entries({x: [1]}), Object.fromEntries([["a", 1], ["b", 2]]), Object.keys(["x", "y"])]
--- value
[["b","a"],[1,2],[["x",[1]]],{"a":1,"b":2},["0","1"]]

=== Number
[Number.isInteger(5), Number.isInteger(5.5), Number.isInteger("5"), Number.isSafeInteger(2 ** 0 * 9007199254740992), Number.isFinite(Infinity), Number.isNaN(NaN), Number.MAX_SAFE_INTEGER]
--- fault ** for BigInts only

=== Number without **
[Number.isInteger(5), Number.isInteger(5.5), Number.isInteger("5"), Number.isSafeInteger(9007199254740992), Number.isFinite(Infinity), Number.isNaN(NaN), Number.MAX_SAFE_INTEGER]
--- value
[true,false,false,false,false,true,9007199254740991]

=== Array.of
[Array.of(), Array.of(7), Array.of(1, "a", null, true), Array.of(3).length, Array.of(1, 2).concat(Array.of(3)), Array.of(undefined)[0] === undefined, Array.of(NaN).includes(NaN), 1 / Array.of(-0)[0]]
--- value
[[],[7],[1,"a",null,true],1,[1,2,3],true,true,-Infinity]

=== Array.of holds its arguments, not copies
((o, a) => { const r = Array.of(o, a, [1], {k: 2}); return [r[0].x, r[1][0], r[2], r[3].k, r.length]; })({x: 5}, [9])
--- value
[5,9,[1],2,4]

=== Array.of with a spread and a function argument
[Array.of(...[1, 2, 3]), Array.of(...[]).length, Array.of(...Array.of(4, 5), 6), Array.of(x => x).length]
--- value
[[1,2,3],0,[4,5,6],1]

=== Array.of chains into the methods that reuse a fresh array
[Array.of(3, 1, 2).toSorted((a, b) => a - b), Array.of(1, 2, 3).toSpliced(1, 1, 9), Array.of(1, 2).with(0, 5), Array.of([1, 2], [3]).map(x => x.length)]
--- value
[[1,2,3],[1,9,3],[5,2],[2,1]]

=== Array.of as a callback gets the element, index and array
[1, 2].map(Array.of)
--- value
[[1,0,[1,2]],[2,1,[1,2]]]

=== Array.of call faults
Array.of.call(null, 1)
--- fault staticjs has methods on arrays, strings, sets, maps, iterators and BigInts only

=== Array.of typeof
typeof Array.of
--- value
"function"

=== Array.of is not a constructor call
new Array.of(1)
--- error unsupported constructs only Set

=== Array.from an array, a set, a string and a length object
[Array.from([1, 2, 3]), Array.from(new Set([3, 1, 3, 2])), Array.from("abc"), Array.from({length: 3}).length, Array.from({length: 0})]
--- value
[[1,2,3],[3,1,2],["a","b","c"],3,[]]

=== Array.from empty input
[Array.from([]), Array.from(new Set()), Array.from(""), Array.from([], x => x), Array.from("", x => x), Array.from(new Set(), x => x), Array.from({length: 0}, x => x)]
--- value
[[],[],[],[],[],[],[]]

=== Array.from a string keeps surrogate pairs together
[Array.from("a\u{1F600}b").length, Array.from("a\u{1F600}b")[1] === "\u{1F600}", Array.from("é中").length, Array.from("\u{1F600}", c => c.length)]
--- value
[3,true,2,[2]]

=== Array.from gives an array that holds the same elements
((a, o) => { const b = Array.from([o, a]); return [b[0].x, b[1][0], b.length, Array.from(a) !== undefined]; })([4], {x: 5})
--- value
[5,4,2,true]

=== Array.from keeps NaN, -0 and undefined
[Array.from([NaN])[0] !== Array.from([NaN])[0], 1 / Array.from([-0])[0], Array.from([undefined, null]).length, Array.from({length: 2})[1] === undefined]
--- value
[true,-Infinity,2,true]

=== Array.from with a mapper
[Array.from([1, 2, 3], x => x * 2), Array.from(new Set([1, 2]), x => x + 1), Array.from("ab", c => c + c), Array.from({length: 3}, (_, i) => i * i), Array.from([5, 6], (x, i) => [x, i])]
--- value
[[2,4,6],[2,3],["aa","bb"],[0,1,4],[[5,0],[6,1]]]

=== Array.from calls the mapper with the element and its index only
Array.from([7, 8], (...args) => args.length)
--- value
[2,2]

=== Array.from with a mapper that is a built-in
[Array.from([1, 2], Array.of), Array.from([[1], [2, 3]], Array.isArray), Array.from([4, 9], Math.max)]
--- value
[[[1,0],[2,1]],[true,true],[4,9]]

=== Array.from with an undefined mapper is Array.from without one
[Array.from([1, 2], undefined), Array.from("ab", undefined)]
--- value
[[1,2],["a","b"]]

=== Array.from for an array-like gives undefined to the mapper
Array.from({length: 2}, x => x === undefined)
--- value
[true,true]

=== Array.from inside a mapper and a loop
[Array.from([1, 2], x => Array.from({length: x}, (_, i) => i)), [1, 2].map(x => Array.from("ab", c => c + x)), Array.from([3, 1, 2]).toSorted((a, b) => a - b)]
--- value
[[[0],[0,1]],[["a1","b1"],["a2","b2"]],[1,2,3]]

=== Array.from with a mapper that returns objects and arrays
Array.from([1, 2], x => ({k: x}))
--- value
[{"k":1},{"k":2}]

=== Array.from with a PLUCK mapper
[Array.from([{a: 1}, {a: 2}, {}], x => x.a).length, Array.from([{a: 1}, {a: 2}], x => x.a), Array.from([{a: [1]}], x => x.a)]
--- value
[3,[1,2],[[1]]]

=== Array.from with a PLUCK mapper in C
@shapes PLUCK(k)
Array.from([{k: 1}, {k: "a"}, {n: 2}], x => x.k)
--- value
[1,"a",undefined]

=== Array.from with a PLUCK mapper on a string reads undefined
@shapes PLUCK(k)
Array.from("ab", x => x.k)
--- value
[undefined,undefined]

=== Array.from with a PLUCK mapper on a length object faults
@shapes PLUCK(k)
Array.from({length: 2}, x => x.k)
--- fault

=== Array.from with a PLUCK mapper on a number element faults
@shapes PLUCK(k)
Array.from([{k: 1}, 5], x => x.k)
--- fault reads properties

=== Array.from with a PLUCK mapper that reads a prototype name
@shapes PLUCK(toString)
Array.from([{a: 1}], x => x.toString)
--- fault no prototype

=== Array.from with a third mapper parameter reads undefined
@shapes -
Array.from([1, 2], (x, i, a) => a === undefined)
--- value
[true,true]

=== Array.from ignores a third argument
Array.from([1, 2], x => x + 1, 5)
--- value
[2,3]

=== Array.from with a spread and chained
[Array.from(...[[1, 2]]), Array.from([1, 2]).concat(Array.from("c")), Array.from(Array.from([1])).length]
--- value
[[1,2],[1,2,"c"],1]

=== Array.from faults on a number
Array.from(5)
--- fault Array.from takes

=== Array.from faults on null and undefined
Array.from(null)
--- fault Array.from takes

=== Array.from faults with no arguments
Array.from()
--- fault Array.from takes

=== Array.from faults on an object that is not a length
Array.from({a: 1})
--- fault Array.from takes

=== Array.from faults on an empty object
Array.from({})
--- fault Array.from takes

=== Array.from faults on an object with a length and another key
Array.from({length: 1, a: 1})
--- fault Array.from takes

=== Array.from faults on a negative length
Array.from({length: -1})
--- fault non-negative integer

=== Array.from faults on a fractional length
Array.from({length: 1.5})
--- fault non-negative integer

=== Array.from faults on a NaN length
Array.from({length: NaN})
--- fault non-negative integer

=== Array.from faults on an infinite length
Array.from({length: Infinity})
--- fault non-negative integer

=== Array.from faults on a length that is a string
Array.from({length: "2"})
--- fault non-negative integer

=== Array.from faults on a length past the limit
Array.from({length: 1e9})
--- fault limit

=== Array.from faults on a mapper that is not a function
Array.from([1], 1)
--- fault Array.from maps with a function

=== Array.from faults on a null mapper
Array.from([1], null)
--- fault Array.from maps with a function

=== Array.from faults on a mapper that is not a function, for an empty array
Array.from([], "x")
--- fault Array.from maps with a function

=== Array.from faults when the mapper faults
Array.from([1], x => x.a.b)
--- fault

=== Array.from faults on a function as items
Array.from(x => x)
--- fault Array.from takes

=== Array.from typeof
typeof Array.from
--- value
"function"

=== Array.from is not a constructor call
new Array.from([1])
--- error unsupported constructs only Set

=== String and Boolean
[String(1.5), String(null), String(), Boolean(""), Boolean("x"), Boolean()]
--- value
["1.5","null","",false,true,false]

=== Array.isArray
[Array.isArray([]), Array.isArray({}), Array.isArray("x")]
--- value
[true,false,false]

=== a namespace on its own is refused
Math
--- error unsupported Math.name

# A key built at run time finds the same methods and prototype names as a
# key written in the source.
=== a method named by a key built at run time
((h, s) => [new Set([1, 2])[h](2), [1, 2, 3][s](1), "abc"[s](1)])("h" + "as", "sl" + "ice")
--- value
[true,[2,3],"bc"]

=== a changing method named by a key built at run time
(p => [1][p](2))("pu" + "sh")
--- fault only on a variable

=== a method of another kind named by a key built at run time
(h => [1][h](1))("h" + "as")
--- fault does not have

=== a prototype name built at run time, read from an object
(t => ({})[t])("to" + "String")
--- fault no prototype

=== a prototype name built at run time, tested with in on an array
(m => m in [1])("ma" + "p")
--- fault no prototype

=== a string method built at run time, read as a value
(t => "x"[t])("tr" + "im")
--- fault calls methods

=== a name that only sorts near prototype names is an own property
(o => [o.toStrin, o["valueOf" + "f"], o.__proto, "concat" in {}, "a" in [1]])({ toStrin: 1, valueOff: 2, __proto: 3 })
--- value
[1,2,3,false,false]

# Object.groupBy: a loop; keys are the callback's results through ToPropertyKey,
# each holding its items in order. Node's result has a null prototype, and
# staticjs objects have none.
=== Object.groupBy groups by the callback's result
Object.groupBy([1, 2, 3, 4, 5], x => x % 2 ? "odd" : "even")
--- value
{"odd":[1,3,5],"even":[2,4]}

=== Object.groupBy on an empty array
Object.groupBy([], x => x)
--- value
{}

=== Object.groupBy keeps array-index keys first, in ascending order
Object.groupBy(["b", "10", "a", "2", "01"], x => x)
--- value
{"2":["2"],"10":["10"],"b":["b"],"a":["a"],"01":["01"]}

=== Object.groupBy turns primitive keys into property keys
Object.groupBy([0, -0, 1, NaN, null, undefined, true, false, "1", 1.5], x => x === 0 ? -0 : x)
--- value
{"0":[0,-0],"1":[1,"1"],"NaN":[NaN],"null":[null],"undefined":[undefined],"true":[true],"false":[false],"1.5":[1.5]}

=== Object.groupBy passes the element and its index
Object.groupBy(["a", "b", "c", "d"], (x, i) => i % 2 ? "odd" : "even")
--- value
{"even":["a","c"],"odd":["b","d"]}

=== Object.groupBy passes no third argument
Object.groupBy([1, 2], (x, i, ...rest) => rest.length ? "extra" : "two")
--- value
{"two":[1,2]}

=== Object.groupBy with an object key callback in C
@shapes PLUCK(k)
Object.groupBy([{k: "a", n: 1}, {k: "b", n: 2}, {k: "a", n: 3}, {n: 4}], x => x.k)
--- value
{"a":[{"k":"a","n":1},{"k":"a","n":3}],"b":[{"k":"b","n":2}],"undefined":[{"n":4}]}

=== Object.groupBy with a number key callback in C
@shapes PLUCK(k)
Object.groupBy([{k: 2}, {k: 1}, {k: 2}, {k: 10}], x => x.k)
--- value
{"1":[{"k":1}],"2":[{"k":2},{"k":2}],"10":[{"k":10}]}

=== Object.groupBy with a key callback on a number element
@shapes PLUCK(k)
Object.groupBy([{k: 1}, 5], x => x.k)
--- fault reads properties

=== Object.groupBy with a key callback on an element with no key to read
@shapes PLUCK(k)
Object.groupBy([{k: 1}, null], x => x.k)
--- fault

=== Object.groupBy with a key callback that reads a prototype name
@shapes PLUCK(toString)
Object.groupBy([{a: 1}], x => x.toString)
--- fault no prototype

=== Object.groupBy with a key callback that finds an object
Object.groupBy([{k: {}}], x => x.k)
--- fault primitive

=== Object.groupBy with a key callback that finds an array
Object.groupBy([[1]], x => x)
--- fault primitive

=== Object.groupBy faults on a callback result that is an object
Object.groupBy([1], x => ({}))
--- fault primitive

=== Object.groupBy faults on a function result
Object.groupBy([1], x => y => y)
--- fault primitive

=== Object.groupBy faults on the key __proto__
Object.groupBy(["__proto__"], x => x)
--- fault no prototype

=== Object.groupBy with many keys
(g => [Object.keys(g).length, g.k4])(Object.groupBy([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15], x => "k" + String(x % 11)))
--- value
[11,[4,15]]

=== Object.groupBy leaves its items alone
(xs => [Object.groupBy(xs, x => x > 1), xs])([1, 2, 3])
--- value
[{"false":[1],"true":[2,3]},[1,2,3]]

=== Object.groupBy results are objects with own properties
(g => [g.odd, g.even, Object.keys(g), "odd" in g, Object.entries(g)])(Object.groupBy([1, 2, 3], x => x % 2 ? "odd" : "even"))
--- value
[[1,3],[2],["odd","even"],true,[["odd",[1,3]],["even",[2]]]]

=== Object.groupBy results can be named after a prototype name
Object.groupBy([1, 2], x => "toString").toString
--- value
[1,2]

=== Object.groupBy results have no prototype to read
Object.groupBy([1, 2], x => "a").toString
--- fault no prototype

=== Object.groupBy results have no prototype to test
"constructor" in Object.groupBy([1], x => "a")
--- fault no prototype

=== Object.groupBy results spread into an object
({...Object.groupBy([1, 2, 3], x => x % 2), z: 1})
--- value
{"0":[2],"1":[1,3],"z":1}

=== Object.groupBy nests in itself
Object.groupBy(Object.entries(Object.groupBy([1, 2, 3, 4], x => x % 2)), e => e[1].length)
--- value
{"2":[["0",[2,4]],["1",[1,3]]]}

=== Object.groupBy faults on a string
Object.groupBy("abc", x => x)
--- fault takes an array

=== Object.groupBy faults on a set
Object.groupBy(new Set([1]), x => x)
--- fault takes an array

=== Object.groupBy faults on an object
Object.groupBy({a: 1}, x => x)
--- fault takes an array

=== Object.groupBy faults on undefined
Object.groupBy(undefined, x => x)
--- fault takes an array

=== Object.groupBy faults with no arguments
Object.groupBy()
--- fault Object.groupBy

=== Object.groupBy faults with no callback
Object.groupBy([1])
--- fault not a function

=== Object.groupBy faults on a callback that is not a function
Object.groupBy([1], "a")
--- fault not a function

=== Object.groupBy faults on an empty array with no callback
Object.groupBy([], 1)
--- fault not a function

=== Object.groupBy faults when the callback faults
Object.groupBy([1], x => x.a.b)
--- fault

=== Object.groupBy ignores extra arguments
Object.groupBy([1, 2], x => x, "extra", 3)
--- value
{"1":[1],"2":[2]}

=== Object.groupBy as a value
(f => f([1, 2, 3], x => x > 1))(Object.groupBy)
--- value
{"false":[1],"true":[2,3]}

=== Object.groupBy with a native-style callback name
Object.groupBy([1, 2, 3], Boolean)
--- value
{"true":[1,2,3]}

=== Object.groupBy with a block-body key callback
@shapes PLUCK(k)
Object.groupBy([{k: "a"}, {k: "b"}, {k: "a"}], x => { return x.k; })
--- value
{"a":[{"k":"a"},{"k":"a"}],"b":[{"k":"b"}]}

=== Object.groupBy with an index parameter is not a pluck
@shapes -
Object.groupBy([{k: 1}, {k: 2}, {k: 3}], (x, i) => i % 2)
--- value
{"0":[{"k":1},{"k":3}],"1":[{"k":2}]}

=== Object.groupBy with a key callback that finds __proto__
@shapes PLUCK(k)
Object.groupBy([{k: "__proto__"}], x => x.k)
--- fault no prototype

=== Object.groupBy with a key callback that finds a function
@shapes - PLUCK(k)
Object.groupBy([{k: () => 1}], x => x.k)
--- fault primitive

=== Object.groupBy with a key callback on a string element
@shapes PLUCK(length)
Object.groupBy(["ab", "c", "de"], x => x.length)
--- value
{"1":["c"],"2":["ab","de"]}

=== Object.groupBy with booleans from a key callback
@shapes PLUCK(k)
Object.groupBy([{k: true}, {k: false}, {k: null}, {k: true}], x => x.k)
--- value
{"true":[{"k":true},{"k":true}],"false":[{"k":false}],"null":[{"k":null}]}

=== toSpliced removes and inserts
[[1, 2, 3, 4].toSpliced(1, 2), [1, 2, 3, 4].toSpliced(1, 2, "a", "b", "c"), [1, 2, 3].toSpliced(1, 0, 9), [1, 2, 3].toSpliced(3, 0, 4), [1, 2, 3].toSpliced(0, 1, [7], {k: 1})]
--- value
[[1,4],[1,"a","b","c",4],[1,9,2,3],[1,2,3,4],[[7],{"k":1},2,3]]

=== toSpliced without a skipCount removes to the end
[[1, 2, 3].toSpliced(1), [1, 2, 3].toSpliced(), [1, 2, 3].toSpliced(undefined), [1, 2, 3].toSpliced(1, undefined), [1, 2, 3].toSpliced(-1), [1, 2, 3].toSpliced(5)]
--- value
[[1],[1,2,3],[],[1,2,3],[1,2],[1,2,3]]

=== toSpliced clamps start and skipCount
[[1, 2, 3].toSpliced(-9, 1), [1, 2, 3].toSpliced(-Infinity, 1), [1, 2, 3].toSpliced(Infinity, 1, 0), [1, 2, 3].toSpliced(1, -5), [1, 2, 3].toSpliced(1, 99), [1, 2, 3].toSpliced(1, Infinity), [1, 2, 3].toSpliced(1.9, 1.9), [1, 2, 3].toSpliced(NaN, NaN, 0), [1, 2, 3].toSpliced(-0, -0, 0), [1, 2, 3].toSpliced(null, true), [1, 2, 3].toSpliced(1e15, 1e15, 7)]
--- value
[[2,3],[2,3],[1,2,3,0],[1,2,3],[1],[1],[1,3],[0,1,2,3],[0,1,2,3],[2,3],[1,2,3,7]]

=== toSpliced on an empty array
[[].toSpliced(), [].toSpliced(0, 1), [].toSpliced(-3, 2, 1, 2), [].toSpliced(5, 0, "x")]
--- value
[[],[],[1,2],["x"]]

=== toSpliced inserts undefined, null and -0 as given
[[1].toSpliced(0, 0, undefined, null, -0, NaN)]
--- value
[[undefined,null,-0,NaN,1]]

=== toSpliced leaves the receiver alone while it is held
(xs => [xs.toSpliced(1, 1, 8, 9), xs])([1, 2, 3])
--- value
[[1,8,9,3],[1,2,3]]

=== toSpliced on a receiver nothing else holds
[[1, 2, 3].toSpliced(1, 1), [1, 2, 3].toSpliced(1, 1, 8), [1, 2, 3].toSpliced(1, 1, 8, 9, 10), [1, 2, 3].toSpliced(0, 3, 4), [1, 2, 3].toSpliced(0, 0, 4, 5, 6, 7), [[1], [2]].toSpliced(0, 1, [3])]
--- value
[[1,3],[1,8,3],[1,8,9,10,3],[4],[4,5,6,7,1,2,3],[[3],[2]]]

=== toSpliced inserts the receiver into itself
(xs => xs.toSpliced(1, 0, xs))([1, 2])
--- value
[1,[1,2],2]

=== toSpliced in a chain
[1, 2, 3, 4, 5].toSpliced(1, 1).toSpliced(1, 0, 7, 7).toSpliced(-1)
--- value
[1,7,7,3,4]

=== toSpliced faults on a start that would be coerced
[1].toSpliced("0", 1)
--- fault number

=== toSpliced faults on a skipCount that would be coerced
[1].toSpliced(0, "1")
--- fault number

=== toSpliced faults on an object start
[1].toSpliced({}, 1)
--- fault number

=== toSpliced faults on an empty array too
[].toSpliced([], 0)
--- fault number

=== toSpliced is not a string method
"abc".toSpliced(1)
--- fault does not have this method

=== toSpliced clamps huge arguments
[[1, 2, 3].toSpliced(4294967296, 4294967296, 0), [1, 2, 3].toSpliced(-4294967296, 4294967296), [1, 2, 3].toSpliced(1, -4294967296, 5), [1, 2, 3].toSpliced(4294967297, 1)]
--- value
[[1,2,3,0],[],[1,5,2,3],[1,2,3]]

=== toSpliced keeps closures as items
[1, 2].toSpliced(1, 0, x => x, new Set([1])).length
--- value
4

=== toSpliced faults on an array skipCount
[1].toSpliced(0, [1])
--- fault number
