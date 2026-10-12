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

=== toSorted without a comparator sorts as strings
[2, 10, 1].toSorted()
--- value
[1,10,2]

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

=== reading an array method as a value
[1].forEach
--- fault calls methods

=== reading a set method as a value
new Set([1]).forEach
--- fault calls methods

=== in finds an array method
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

=== isSupersetOf: empty sets, equal sets, overlap, disjoint, the set itself
(a => [a.isSupersetOf(new Set()), new Set().isSupersetOf(a), new Set().isSupersetOf(new Set()), a.isSupersetOf(new Set([3, 1])), a.isSupersetOf(new Set([1, 2, 3])), a.isSupersetOf(new Set([1, 4])), a.isSupersetOf(new Set([4, 5])), a.isSupersetOf(new Set([1, 2, 3, 4])), a.isSupersetOf(a)])(new Set([1, 2, 3]))
--- value
[true,false,true,true,true,false,false,false,true]

=== isDisjointFrom: empty sets, equal sets, overlap, disjoint, the set itself
(a => [a.isDisjointFrom(new Set()), new Set().isDisjointFrom(a), new Set().isDisjointFrom(new Set()), a.isDisjointFrom(new Set([4, 5])), a.isDisjointFrom(new Set([5, 3])), a.isDisjointFrom(new Set([3, 2, 1])), a.isDisjointFrom(new Set([9, 8, 7, 6, 5, 4, 1])), a.isDisjointFrom(a)])(new Set([1, 2, 3]))
--- value
[true,true,true,true,false,false,false,false]

=== symmetricDifference: empty sets, equal sets, the set itself, and its order
(a => [[...a.symmetricDifference(new Set())], [...new Set().symmetricDifference(a)], [...new Set().symmetricDifference(new Set())], [...a.symmetricDifference(new Set([3, 2, 1]))], [...a.symmetricDifference(a)], [...a.symmetricDifference(new Set([4, 5]))], [...new Set([1, 2, 3, 4]).symmetricDifference(new Set([6, 4, 5, 2]))], [...new Set([6, 4, 5, 2]).symmetricDifference(new Set([1, 2, 3, 4]))]])(new Set([1, 2, 3]))
--- value
[[1,2,3],[1,2,3],[],[],[],[1,2,3,4,5],[1,3,6,5],[6,5,1,3]]

=== isSupersetOf, isDisjointFrom and symmetricDifference compare by SameValueZero
(a => [a.isSupersetOf(new Set([-0, NaN])), a.isSupersetOf(new Set(["1"])), a.isDisjointFrom(new Set([-0])), a.isDisjointFrom(new Set(["0", "NaN", 2, false])), new Set(["1"]).isDisjointFrom(new Set([1])), new Set([NaN]).isDisjointFrom(new Set([NaN])), [...a.symmetricDifference(new Set([-0, "x", NaN, 1, true, "1"]))]])(new Set([0, NaN, "x", 1, null, undefined]))
--- value
[true,false,false,true,true,false,[null,undefined,true,"1"]]

=== symmetricDifference keeps a set's -0 as 0
[[...new Set([-0, 1]).symmetricDifference(new Set([2, -0]))], [...new Set([-0, 1]).symmetricDifference(new Set([2]))]]
--- value
[[1,2],[0,1,2]]

=== isSupersetOf, isDisjointFrom and symmetricDifference on non-ASCII strings
[new Set(["é", "😀", "a"]).isSupersetOf(new Set(["😀", "é"])), new Set(["é"]).isDisjointFrom(new Set(["e", "😀"])), [...new Set(["é", "😀"]).symmetricDifference(new Set(["😀", "e"]))]]
--- value
[true,true,["é","e"]]

=== isSupersetOf, isDisjointFrom and symmetricDifference on large sets
(a => [a.isSupersetOf(new Set([19, 0, 7, 12, 3, 4, 5, 6, 8, 9])), a.isSupersetOf(new Set([19, 0, 7, 12, 3, 4, 5, 6, 8, 20])), new Set([1, 2]).isSupersetOf(a), a.isDisjointFrom(new Set([20, 21, 22, 23, 24, 25, 26, 27, 28, 29])), a.isDisjointFrom(new Set([20, 21, 22, 23, 24, 25, 26, 27, 28, 19])), new Set([30, 19]).isDisjointFrom(a), new Set([30, 31]).isDisjointFrom(a), [...a.symmetricDifference(new Set([25, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 30]))]])(new Set([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19]))
--- value
[true,false,false,true,false,false,true,[18,19,25,30]]

=== isSupersetOf of a symmetric difference
new Set([1, 2]).symmetricDifference(new Set([2, 3])).isSupersetOf(new Set([3, 1]))
--- value
true

=== isSupersetOf faults with no argument
new Set([1]).isSupersetOf()
--- fault combines a set only with another set

=== isSupersetOf faults with an array
new Set([1]).isSupersetOf([1])
--- fault combines a set only with another set

=== isSupersetOf faults with a number
new Set([1]).isSupersetOf(1)
--- fault combines a set only with another set

=== isSupersetOf faults with a string
new Set([1]).isSupersetOf("a")
--- fault combines a set only with another set

=== isSupersetOf faults with an object
new Set([1]).isSupersetOf({size: 1})
--- fault combines a set only with another set

=== isSupersetOf faults with null
new Set([1]).isSupersetOf(null)
--- fault combines a set only with another set

# A map is set-like in JavaScript, which gives a value; staticjs takes only a set.
=== isSupersetOf faults with a map
new Set([1]).isSupersetOf(new Map([[1, 1]]))
--- fault combines a set only with another set

=== isDisjointFrom faults with no argument
new Set([1]).isDisjointFrom()
--- fault combines a set only with another set

=== isDisjointFrom faults with an array
new Set([1]).isDisjointFrom([1])
--- fault combines a set only with another set

=== isDisjointFrom faults with a number
new Set([1]).isDisjointFrom(1)
--- fault combines a set only with another set

=== isDisjointFrom faults with a string
new Set([1]).isDisjointFrom("a")
--- fault combines a set only with another set

=== isDisjointFrom faults with an object
new Set([1]).isDisjointFrom({size: 1})
--- fault combines a set only with another set

=== isDisjointFrom faults with null
new Set([1]).isDisjointFrom(null)
--- fault combines a set only with another set

# A map is set-like in JavaScript, which gives a value; staticjs takes only a set.
=== isDisjointFrom faults with a map
new Set([1]).isDisjointFrom(new Map([[1, 1]]))
--- fault combines a set only with another set

=== symmetricDifference faults with no argument
new Set([1]).symmetricDifference()
--- fault combines a set only with another set

=== symmetricDifference faults with an array
new Set([1]).symmetricDifference([1])
--- fault combines a set only with another set

=== symmetricDifference faults with a number
new Set([1]).symmetricDifference(1)
--- fault combines a set only with another set

=== symmetricDifference faults with a string
new Set([1]).symmetricDifference("a")
--- fault combines a set only with another set

=== symmetricDifference faults with an object
new Set([1]).symmetricDifference({size: 1})
--- fault combines a set only with another set

=== symmetricDifference faults with null
new Set([1]).symmetricDifference(null)
--- fault combines a set only with another set

# A map is set-like in JavaScript, which gives a value; staticjs takes only a set.
=== symmetricDifference faults with a map
new Set([1]).symmetricDifference(new Map([[1, 1]]))
--- fault combines a set only with another set

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

# Object.hasOwn: own keys only, the key converted as ToPropertyKey does from
# a primitive. Faults where JavaScript would call an object key's toString
# (Node gives a value there).

=== Object.hasOwn finds own keys of an object only
[Object.hasOwn({a: 1}, "a"), Object.hasOwn({a: 1}, "b"), Object.hasOwn({}, "toString"), Object.hasOwn({}, "hasOwnProperty"), Object.hasOwn({toString: 1}, "toString"), Object.hasOwn({a: undefined}, "a")]
--- value
[true,false,false,false,true,true]

=== Object.hasOwn of an empty object
[Object.hasOwn({}, "a"), Object.hasOwn({}, ""), Object.hasOwn({"": 0}, "")]
--- value
[false,false,true]

=== Object.hasOwn converts a number key to its string
[Object.hasOwn({1: "a"}, 1), Object.hasOwn({0: "a"}, -0), Object.hasOwn({"1.5": 1}, 1.5), Object.hasOwn({NaN: 1}, NaN), Object.hasOwn({Infinity: 1}, Infinity), Object.hasOwn({"-1": 1}, -1), Object.hasOwn({"1e+21": 1}, 1e21), Object.hasOwn({1: "a"}, "01")]
--- value
[true,true,true,true,true,true,true,false]

=== Object.hasOwn converts true, false, null and undefined keys by name
[Object.hasOwn({true: 1}, true), Object.hasOwn({false: 1}, false), Object.hasOwn({null: 1}, null), Object.hasOwn({undefined: 1}, undefined), Object.hasOwn({undefined: 1}), Object.hasOwn({a: 1})]
--- value
[true,true,true,true,true,false]

=== Object.hasOwn converts a BigInt key
[Object.hasOwn({1: "a"}, 1n), Object.hasOwn({"18446744073709551616": 1}, 18446744073709551616n), Object.hasOwn({"-1": 1}, -1n)]
--- value
[true,true,true]

=== Object.hasOwn of an array: its indices and length
[Object.hasOwn([1], "length"), Object.hasOwn([1], 0), Object.hasOwn([1], "0"), Object.hasOwn([1], 1), Object.hasOwn([1], -1), Object.hasOwn([1], "00"), Object.hasOwn([1], "map"), Object.hasOwn([], "length"), Object.hasOwn([], 0), Object.hasOwn([1, 2, 3], 2.0), Object.hasOwn([1, 2], 0.5)]
--- value
[true,true,true,false,false,false,false,true,false,true,false]

=== Object.hasOwn of an array holding undefined
[Object.hasOwn([undefined], 0), Object.hasOwn([undefined, null], 1)]
--- value
[true,true]

=== Object.hasOwn of a string: its indices and length
[Object.hasOwn("ab", 0), Object.hasOwn("ab", 1), Object.hasOwn("ab", 2), Object.hasOwn("ab", "length"), Object.hasOwn("ab", "1"), Object.hasOwn("", "length"), Object.hasOwn("", 0), Object.hasOwn("ab", "at"), Object.hasOwn("ab", -0)]
--- value
[true,true,false,true,true,true,false,false,true]

=== Object.hasOwn of a string counts UTF-16 units
[Object.hasOwn("é😀", 0), Object.hasOwn("é😀", 1), Object.hasOwn("é😀", 2), Object.hasOwn("é😀", 3)]
--- value
[true,true,true,false]

=== Object.hasOwn of numbers, booleans and BigInts
[Object.hasOwn(1, "toFixed"), Object.hasOwn(1, 0), Object.hasOwn(NaN, "length"), Object.hasOwn(true, "valueOf"), Object.hasOwn(false, "x"), Object.hasOwn(1n, "toString")]
--- value
[false,false,false,false,false,false]

=== Object.hasOwn of a set and a map: no own keys
[Object.hasOwn(new Set([1]), 1), Object.hasOwn(new Set([1]), "size"), Object.hasOwn(new Map([["a", 1]]), "a"), Object.hasOwn(new Map(), "size"), Object.hasOwn(new Map([["a", 1]]).keys(), "next")]
--- value
[false,false,false,false,false]

=== Object.hasOwn of a function: its length and name
[Object.hasOwn(x => x, "length"), Object.hasOwn(x => x, "name"), Object.hasOwn(x => x, "prototype"), Object.hasOwn(Math.max, "length"), Object.hasOwn(Math.max, "name"), Object.hasOwn(Math.max, "x")]
--- value
[true,true,false,true,true,false]

=== Object.hasOwn of String, Boolean, Number and BigInt: length and name
[String, Boolean, Number, BigInt].map((f) => [Object.hasOwn(f, "length"), Object.hasOwn(f, "name")])
--- value
[[true,true],[true,true],[true,true],[true,true]]

=== Object.hasOwn of a built-in function without statics
[Object.hasOwn(String.raw, "prototype"), Object.hasOwn(parseInt, "prototype"), Object.hasOwn(Number.isInteger, "prototype")]
--- value
[false,false,false]

# JavaScript gives these true: the prototype and the statics are own.
=== Object.hasOwn of String's prototype faults
Object.hasOwn(String, "prototype")
--- fault Object.hasOwn of String, Boolean, Number or BigInt reads only length and name

=== Object.hasOwn of a static of String faults
Object.hasOwn(String, "fromCharCode")
--- fault Object.hasOwn of String, Boolean, Number or BigInt reads only length and name

=== Object.hasOwn of a constant of Number faults
Object.hasOwn(Number, "MAX_SAFE_INTEGER")
--- fault Object.hasOwn of String, Boolean, Number or BigInt reads only length and name

=== Object.hasOwn of a static of BigInt faults
Object.hasOwn(BigInt, "asIntN")
--- fault Object.hasOwn of String, Boolean, Number or BigInt reads only length and name

=== Object.hasOwn of String held in a variable faults
(() => { const S = String; return Object.hasOwn(S, "raw"); })()
--- fault Object.hasOwn of String, Boolean, Number or BigInt reads only length and name

# JavaScript gives false; staticjs faults on any key but length and name
# rather than list each constructor's statics.
=== Object.hasOwn of another key of Boolean faults
Object.hasOwn(Boolean, "x")
--- fault Object.hasOwn of String, Boolean, Number or BigInt reads only length and name

=== Object.hasOwn of a regular expression: lastIndex
[Object.hasOwn(/a/g, "lastIndex"), Object.hasOwn(/a/g, "source"), Object.hasOwn(/a/g, 0)]
--- value
[true,false,false]

=== Object.hasOwn of a match result
[Object.hasOwn("ab".match(/b/), "index"), Object.hasOwn("ab".match(/b/), 0)]
--- value
[true,true]

=== Object.hasOwn of a large object, through its index
(o => [Object.hasOwn(o, "k1"), Object.hasOwn(o, "k11"), Object.hasOwn(o, 5), Object.hasOwn(o, "5"), Object.hasOwn(o, "k99"), Object.hasOwn(o, "constructor")])({k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7, k8: 8, k9: 9, k10: 10, 5: "a", k11: 11})
--- value
[true,true,true,true,false,false]

=== Object.hasOwn with a computed key in a callback
["a", "b", "c"].filter(k => Object.hasOwn({a: 1, c: 0}, k))
--- value
["a","c"]

=== Object.hasOwn of a nested object reaches no deeper
[Object.hasOwn({a: {b: 1}}, "b"), Object.hasOwn({a: {b: 1}}.a, "b")]
--- value
[false,true]

=== Object.hasOwn as a value
(f => f({x: 1}, "x"))(Object.hasOwn)
--- value
true

=== Object.hasOwn faults on undefined
Object.hasOwn(undefined, "a")
--- fault not undefined or null

=== Object.hasOwn faults on null
Object.hasOwn(null, "a")
--- fault not undefined or null

=== Object.hasOwn faults with no arguments
Object.hasOwn()
--- fault not undefined or null

=== Object.hasOwn faults on an object key
Object.hasOwn({a: 1}, {})
--- fault a key must be a primitive

=== Object.hasOwn faults on an array key
Object.hasOwn({a: 1}, ["a"])
--- fault a key must be a primitive

=== Object.hasOwn faults on an object key even on a number
Object.hasOwn(1, {})
--- fault a key must be a primitive

# Object.assign called as any function is: its target is not a variable the
# function owns (that is a mutation, in mutation.spec), so it must be a
# value nothing else holds, which no program can see change. Faults where
# JavaScript would wrap a primitive target, or put into an array, a map or a
# function, or change a target something else holds (Node gives a value
# there).

=== Object.assign into a new object
Object.assign({}, {a: 1}, {b: 2})
--- value
{"a":1,"b":2}

=== Object.assign with no sources gives its target
[Object.assign({}), Object.assign({a: 1}), Object.assign([1, 2]), Object.assign(new Set([1])), Object.assign(new Map([[1, 2]]))]
--- value
[{},{"a":1},[1,2],new Set([1]),new Map([[1,2]])]

=== Object.assign with sources that put nothing gives its target
[Object.assign({a: 1}, null, undefined, 5, true, 1n, ""), Object.assign([1], {}, []), Object.assign(new Set([1]), null)]
--- value
[{"a":1},[1],new Set([1])]

=== Object.assign overwrites in source order, a key keeping its place
Object.assign({a: 1, b: 2}, {b: 3, c: 4}, {a: 5}, {d: 6, c: 7})
--- value
{"a":5,"b":3,"c":7,"d":6}

=== Object.assign puts array-index keys first, ascending
[Object.assign({b: 1}, {2: "x", a: 2, 1: "y"}), Object.assign({10: 1, z: 2}, {9: 3, 100: 4, y: 5}), Object.keys(Object.assign({b: 1, a: 2}, {"01": 0, 0: 1}))]
--- value
[{"1":"y","2":"x","b":1,"a":2},{"9":3,"10":1,"100":4,"z":2,"y":5},["0","b","a","01"]]

=== Object.assign skips null and undefined sources and puts nothing for numbers, booleans and BigInts
Object.assign({}, null, {a: 1}, undefined, 2, false, 3n, {b: 2})
--- value
{"a":1,"b":2}

=== Object.assign of a string source puts its characters
[Object.assign({}, "ab"), Object.assign({x: 1}, "", "é!"), Object.assign({0: "z", 5: "q"}, "abc")]
--- value
[{"0":"a","1":"b"},{"0":"é","1":"!","x":1},{"0":"a","1":"b","2":"c","5":"q"}]

=== Object.assign of an array source puts its indices, not its length
[Object.assign({}, ["a", "b"]), Object.assign({length: 5}, [1]), Object.assign({1: "x", k: 0}, [], [true, null, undefined])]
--- value
[{"0":"a","1":"b"},{"0":1,"length":5},{"0":true,"1":null,"2":undefined,"k":0}]

=== Object.assign of sets, maps, functions, regular expressions and iterators puts nothing
Object.assign({k: 1}, new Map([["a", 1]]), new Set([1]), x => x, Math.max, /a/g, new Map([["a", 1]]).keys())
--- value
{"k":1}

=== Object.assign keeps nested values
(o => [o, o.a.b])(Object.assign({}, {a: {b: [1, {c: 2}]}}, {d: [3]}))
--- value
[{"a":{"b":[1,{"c":2}]},"d":[3]},[1,{"c":2}]]

=== Object.assign of many sources
Object.assign({}, {a: 1}, {b: 2}, {c: 3}, {d: 4}, {e: 5}, {f: 6}, {g: 7}, {h: 8}, {i: 9}, {j: 10}, {a: 11}, {k: 12})
--- value
{"a":11,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9,"j":10,"k":12}

=== Object.assign into a large object, through its index
(o => [o.k3, o.k12, o.k20, Object.keys(o)])(Object.assign(Object.fromEntries([["k1", 1], ["k2", 2], ["k3", 3], ["k4", 4], ["k5", 5], ["k6", 6], ["k7", 7], ["k8", 8], ["k9", 9], ["k10", 10], ["k11", 11], ["k12", 12]]), {k3: "c", k20: 20, 7: "seven", k12: "l"}))
--- value
["c","l",20,["7","k1","k2","k3","k4","k5","k6","k7","k8","k9","k10","k11","k12","k20"]]

=== Object.assign from a large source
Object.assign({z: 0, k5: "old"}, Object.fromEntries([["k1", 1], ["k2", 2], ["k3", 3], ["k4", 4], ["k5", 5], ["k6", 6], ["k7", 7], ["k8", 8], ["k9", 9], ["k10", 10]]))
--- value
{"z":0,"k5":5,"k1":1,"k2":2,"k3":3,"k4":4,"k6":6,"k7":7,"k8":8,"k9":9,"k10":10}

=== Object.assign with spread sources
Object.assign({}, ...[{a: 1}, {b: 2}], {c: 3}, ...[])
--- value
{"a":1,"b":2,"c":3}

=== Object.assign with spread arguments, the target among them
Object.assign(...[{a: 1}, {b: 2}])
--- value
{"a":1,"b":2}

=== Object.assign into what a function returns
Object.assign((() => ({a: 1}))(), {b: 2})
--- value
{"a":1,"b":2}

=== Object.assign leaves its sources as they were
(a => [Object.assign({}, a, {x: 2}), a])({x: 1, y: [1]})
--- value
[{"x":2,"y":[1]},{"x":1,"y":[1]}]

=== Object.assign of an object holding undefined and -0
Object.assign({a: 1}, {a: undefined, b: -0, c: NaN})
--- value
{"a":undefined,"b":-0,"c":NaN}

=== Object.assign in map, each target new
[{a: 1}, {a: 2}].map(x => Object.assign({}, x, {b: x.a * 2}))
--- value
[{"a":1,"b":2},{"a":2,"b":4}]

=== Object.assign as a value
(f => f({}, {a: 1}))(Object.assign)
--- value
{"a":1}

=== Object.assign of non-ASCII keys and values
Object.assign({"ключ": 1}, {"é": "ü", "ключ": "значение", "😀": "x"})
--- value
{"ключ":"значение","é":"ü","😀":"x"}

=== Object.assign faults on an undefined target
Object.assign(undefined, {a: 1})
--- fault takes an object as its target

=== Object.assign faults on a null target
Object.assign(null)
--- fault takes an object as its target

=== Object.assign faults with no arguments
Object.assign()
--- fault takes an object as its target

=== Object.assign faults on a number target, which JavaScript wraps
Object.assign(1, {a: 1})
--- fault takes an object as its target

=== Object.assign faults on a string target, even with nothing to put
Object.assign("ab")
--- fault takes an object as its target

=== Object.assign faults on an array target with something to put
Object.assign([1, 2], [3])
--- fault puts properties only into an object

=== Object.assign faults on a map target with something to put
Object.assign(new Map(), {a: 1})
--- fault puts properties only into an object

=== Object.assign faults on a function target with something to put
Object.assign(x => x, {a: 1}).a
--- fault puts properties only into an object

=== Object.assign into a member of an object nothing else holds
(o => Object.assign(o.inner, {a: 1}))({inner: {}})
--- value
{"a":1}

=== Object.assign faults on a target something else holds
(o => [Object.assign(o.inner, {a: 1}), o])({inner: {}})
--- fault something else holds this one

=== Object.assign through a value faults on a target a callback's array holds
[{a: 1}].map(x => (f => f(x, {b: 2}))(Object.assign))
--- fault something else holds this one

=== Object.assign faults on a match result as a source
Object.assign({}, "ab".match(/b/))
--- fault properties besides its elements

=== Object.assign faults on a character outside the BMP
Object.assign({}, "a😀")
--- fault half of a character outside the BMP

=== Object.assign faults on a __proto__ key from JSON.parse
Object.assign({}, JSON.parse('{"__proto__": {"a": 1}}'))
--- fault JSON.parse would make an object with the key __proto__

# Review of Object.hasOwn and of Object.assign called on a target no
# variable owns: every kind of value and key, a hash index, and the faults.

=== Object.hasOwn of each kind of value
[Object.hasOwn([10, 20], 1), Object.hasOwn([10, 20], "length"), Object.hasOwn([10, 20], 2), Object.hasOwn([10], -0), Object.hasOwn([10], "01"), Object.hasOwn([10], 4294967295), Object.hasOwn("a😀", 2), Object.hasOwn("a😀", 3), Object.hasOwn("", "length"), Object.hasOwn(5, "toFixed"), Object.hasOwn(true, "valueOf"), Object.hasOwn(1n, "toString"), Object.hasOwn(/a/g, "lastIndex"), Object.hasOwn(/a/g, "source"), Object.hasOwn(new Set([1]), "size"), Object.hasOwn(new Map([[1, 2]]), 1), Object.hasOwn(new Map([[1, 2]]).keys(), "next")]
--- value
[true,true,false,true,false,false,true,false,true,false,false,false,true,false,false,false,false]

=== Object.hasOwn of keys that ToPropertyKey converts
(() => { const o = { 0: 2, "": 3, "-0": 4, "1.5": 5, undefined: 6, null: 7, true: 8, NaN: 9, Infinity: 10 }; return [0, -0, "", "-0", 1.5, undefined, null, true, false, NaN, Infinity, -Infinity, 1n, 0n].map((k) => Object.hasOwn(o, k)); })()
--- value
[true,true,true,true,true,true,true,true,false,true,true,false,false,true]

=== Object.hasOwn of a function's and a match result's own keys
[Object.hasOwn((a, b) => a, "length"), Object.hasOwn((a) => a, "name"), Object.hasOwn((a) => a, "prototype"), Object.hasOwn(Math.max, "length"), Object.hasOwn(Math.max, "prototype"), Object.hasOwn("ab".match(/b/), "index"), Object.hasOwn("ab".match(/b/), "groups"), Object.hasOwn(/(?<x>a)/.exec("a"), "groups"), Object.hasOwn(/a/d.exec("a"), "indices"), Object.hasOwn(/a/.exec("a"), "indices")]
--- value
[true,true,false,true,false,true,true,true,true,false]

=== Object.hasOwn with a hash index
(() => { const o = { a0: 0, a1: 1, a2: 2, a3: 3, a4: 4, a5: 5, a6: 6, a7: 7, a8: 8, a9: 9 }; return [Object.hasOwn(o, "a9"), Object.hasOwn(o, "a5"), Object.hasOwn(o, "a10"), Object.hasOwn(o, "constructor"), Object.hasOwn(o, "__proto__")]; })()
--- value
[true,true,false,false,false]

=== Object.hasOwn of a key undefined, with no key given
[Object.hasOwn({ undefined: 1 }), Object.hasOwn({})]
--- value
[true,false]

=== Object.hasOwn of an array key, which JavaScript converts
Object.hasOwn([10], [0])
--- fault a key must be a primitive

=== Object.hasOwn of no value
Object.hasOwn()
--- fault Object.hasOwn takes a value that is

=== Object.assign of fresh targets
[Object.assign({ a: 1 }, { b: 2 }, { a: 3 }), Object.assign({}, { a: { b: 1 } }).a, Object.assign({}, ...[{ a: 1 }, { b: 2 }]), Object.assign({ b: 1, 1: 1 }, { 0: 0, a: 2, b: 3 }), Object.assign({}, "ab", [5]), Object.assign({}, 1, true, null, undefined, 1n, "", new Set([1]), new Map([[1, 2]]), /a/g), Object.assign({ a: 1 }, { a: undefined })]
--- value
[{"a":3,"b":2},{"b":1},{"a":1,"b":2},{"0":0,"1":1,"b":3,"a":2},{"0":5,"1":"b"},{},{"a":undefined}]

=== Object.assign of fresh targets with a hash index
(() => { const src = { a0: 0, a1: 1, a2: 2, a3: 3, a4: 4, a5: 5, a6: 6, a7: 7, a8: 8, a9: 9 }; const r = Object.assign({ a3: "x" }, src, { a3: "y" }); return [src, r.a3, r.a9, Object.keys(r)]; })()
--- value
[{"a0":0,"a1":1,"a2":2,"a3":3,"a4":4,"a5":5,"a6":6,"a7":7,"a8":8,"a9":9},"y",9,["a3","a0","a1","a2","a4","a5","a6","a7","a8","a9"]]

=== Object.assign of a target a function returns, and of a copy
(() => { const mk = () => ({ a: 1 }); const base = { k: 0 }; return [Object.assign(mk(), { b: 2 }), Object.assign({ ...base }, { y: 2 }), base, [1, 2].map((x) => Object.assign({}, base, { x })), Object.assign(JSON.parse("{}"), { a: 1 }), Object.assign(Object.fromEntries([["a", 1]]), { b: 2 })]; })()
--- value
[{"a":1,"b":2},{"k":0,"y":2},{"k":0},[{"k":0,"x":1},{"k":0,"x":2}],{"a":1},{"a":1,"b":2}]

=== Object.assign of a target something else holds
(() => { const shared = { a: 1 }; const get = () => shared; const r = Object.assign(get(), { b: 2 }); return [shared, r]; })()
--- fault Object.assign changes its target, and something

=== Object.assign of a target and a source that both hold it
(() => { const o = { z: 1 }; const r = Object.assign(...[o, { b: 2 }]); return [o, r]; })()
--- fault Object.assign changes its target, and something

=== Object.assign with nothing to put gives an array target as it is
(() => { const a = [1]; const r = Object.assign(a, {}, "", null); return [a, r]; })()
--- value
[[1],[1]]

=== Object.assign into an array, with something to put
Object.assign([1, 2], { 0: 9 })
--- fault staticjs's Object.assign puts properties only into

=== Object.assign of a string source outside the BMP
Object.assign({}, "a😀")
--- fault Object.assign would put half of a

=== Object.assign of a number target, with nothing to put
Object.assign(1, {})
--- fault Object.assign takes an object as its

=== Object.assign of no target
Object.assign()
--- fault Object.assign takes an object as its

# Object.freeze, Object.isFrozen, Object.seal and Object.isSealed
# (docs/mutation.md, "Frozen and sealed values"). Each gives what Node 22
# gives; the changes they forbid are in mutation.spec. Every expected value
# was generated by running the case in Node 22.

=== isFrozen and isSealed of primitives are true
[Object.isFrozen(1), Object.isFrozen("s"), Object.isFrozen(""), Object.isFrozen(undefined), Object.isFrozen(null), Object.isFrozen(true), Object.isFrozen(10n), Object.isFrozen(NaN), Object.isFrozen(), Object.isSealed(1), Object.isSealed("s"), Object.isSealed(null), Object.isSealed(undefined), Object.isSealed(false), Object.isSealed(-1n), Object.isSealed()]
--- value
[true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true]

=== isFrozen and isSealed of new objects and arrays are false
[Object.isFrozen({}), Object.isFrozen([]), Object.isFrozen({ a: 1 }), Object.isFrozen([1, 2]), Object.isSealed({}), Object.isSealed([]), Object.isSealed({ a: [1] }), Object.isSealed([[1]])]
--- value
[false,false,false,false,false,false,false,false]

=== isFrozen and isSealed of every other kind are false
[Object.isFrozen(new Set([1])), Object.isFrozen(new Map([[1, 2]])), Object.isFrozen(new Date(0)), Object.isFrozen((x) => x), Object.isFrozen(Math.max), Object.isFrozen(/a/g), Object.isFrozen([1].values()), Object.isFrozen(new Set([1]).values().map((x) => x)), Object.isSealed(new Set()), Object.isSealed(new Map()), Object.isSealed(new Date(0)), Object.isSealed(String), Object.isSealed(/a/), Object.isSealed([].keys())]
--- value
[false,false,false,false,false,false,false,false,false,false,false,false,false,false]

=== isFrozen and isSealed of Temporal values are false
[Object.isFrozen(Temporal.PlainDate.from("2024-01-31")), Object.isSealed(Temporal.Duration.from("P1D")), Object.isFrozen(Temporal.Instant.from("2024-01-01T00:00Z"))]
--- value
[false,false,false]

=== freeze and seal give their argument
[Object.freeze({ a: 1 }), Object.freeze([1, 2]), Object.seal({ b: [2] }), Object.seal([3]), Object.freeze({}), Object.seal([])]
--- value
[{"a":1},[1,2],{"b":[2]},[3],{},[]]

=== freeze and seal of a primitive give it
[Object.freeze(5), Object.freeze("s"), Object.freeze(undefined), Object.freeze(null), Object.freeze(true), Object.seal(-0), Object.seal("x"), Object.seal(null), Object.freeze(), Object.seal(), String(Object.freeze(10n)), String(Object.seal(-3n))]
--- value
[5,"s",undefined,null,true,-0,"x",null,undefined,undefined,"10","-3"]

=== freeze makes an object frozen and sealed
[Object.isFrozen(Object.freeze({ a: 1 })), Object.isSealed(Object.freeze({ a: 1 })), Object.isFrozen(Object.freeze({})), Object.isSealed(Object.freeze({}))]
--- value
[true,true,true,true]

=== freeze makes an array frozen and sealed
[Object.isFrozen(Object.freeze([])), Object.isSealed(Object.freeze([])), Object.isFrozen(Object.freeze([1, [2]])), Object.isSealed(Object.freeze([1, [2]]))]
--- value
[true,true,true,true]

=== seal makes an object sealed, frozen only when it has no key
[Object.isSealed(Object.seal({ a: 1 })), Object.isFrozen(Object.seal({ a: 1 })), Object.isSealed(Object.seal({})), Object.isFrozen(Object.seal({}))]
--- value
[true,false,true,true]

=== seal makes an array sealed, frozen only when it is empty, as V8 tells
[Object.isSealed(Object.seal([1])), Object.isFrozen(Object.seal([1])), Object.isSealed(Object.seal([])), Object.isFrozen(Object.seal([]))]
--- value
[true,false,true,true]

=== freeze and seal of a frozen or sealed value
[Object.isFrozen(Object.seal(Object.freeze({ a: 1 }))), Object.isFrozen(Object.freeze(Object.seal({ a: 1 }))), Object.isSealed(Object.seal(Object.seal([1]))), Object.isFrozen(Object.freeze(Object.freeze([1]))), Object.isFrozen(Object.freeze(Object.seal([])))]
--- value
[true,true,true,true,true]

=== freeze is shallow
(() => { const o = Object.freeze({ inner: { x: 0 }, xs: [1] }); return [Object.isFrozen(o), Object.isFrozen(o.inner), Object.isFrozen(o.xs), Object.isSealed(o.inner)]; })()
--- value
[true,false,false,false]

=== freeze of a match result
[Object.isFrozen("abc".match(/b/)), Object.isFrozen(Object.freeze("abc".match(/b/))), Object.isSealed(Object.seal("abc".match(/b/))), Object.isFrozen(Object.seal("abc".match(/b/))), Object.freeze("abc".match(/b/))]
--- value
[false,true,true,false,Object.assign(["b"],{"index":1,"input":"abc","groups":undefined})]

=== a frozen value reads, lists and writes as before
(() => { const o = Object.freeze({ b: 1, a: [2], 1: "x" }); return [o, o.a, o.b, o[1], Object.keys(o), Object.values(o), Object.entries(o), JSON.stringify(o), Object.hasOwn(o, "a"), "b" in o, o.missing]; })()
--- value
[{"1":"x","b":1,"a":[2]},[2],1,"x",["1","b","a"],["x",1,[2]],[["1","x"],["b",1],["a",[2]]],"{\"1\":\"x\",\"b\":1,\"a\":[2]}",true,true,undefined]

=== a frozen array reads, lists and writes as before
(() => { const a = Object.freeze([3, 1, 2]); return [a, a.length, a[0], a.at(-1), a.includes(1), a.indexOf(2), a.join("-"), JSON.stringify(a), [...a.keys()], a.map((x) => x * 2), a.filter((x) => x > 1)]; })()
--- value
[[3,1,2],3,3,2,true,2,"3-1-2","[3,1,2]",[0,1,2],[6,2,4],[3,2]]

=== a frozen value is the same value through copies of it
(() => { const o = Object.freeze({ a: 1 }); const p = o; const xs = [o, o]; const r = { o }; return [Object.isFrozen(p), Object.isFrozen(xs[1]), Object.isFrozen(r.o), Object.isFrozen(xs.at(0)), Object.isFrozen(xs.find((x) => x.a === 1)), Object.isFrozen([o].map((x) => x)[0])]; })()
--- value
[true,true,true,true,true,true]

=== copies JavaScript makes new are not frozen: arrays
(() => { const a = Object.freeze([2, 1]); return [Object.isFrozen([...a]), Object.isFrozen(Array.from(a)), Object.isFrozen(a.slice()), Object.isFrozen(a.concat()), Object.isFrozen(a.concat([3])), Object.isFrozen(a.toSorted((x, y) => x - y)), Object.isFrozen(a.toReversed()), Object.isFrozen(a.toSpliced(0, 0)), Object.isFrozen(a.with(0, 5)), Object.isFrozen(a.map((x) => x)), Object.isFrozen(a.filter(() => true)), Object.isFrozen(a.flat()), Object.isFrozen(a.flatMap((x) => [x])), Object.isFrozen(a.values().toArray()), Object.isFrozen(Array.from(a.values())), Object.isFrozen([...a.entries()][0]), Object.isFrozen(Array.of(...a))]; })()
--- value
[false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false]

=== copies JavaScript makes new are not frozen: objects
(() => { const o = Object.freeze({ a: 1, b: 2 }); const { a, ...rest } = o; return [Object.isFrozen({ ...o }), Object.isSealed({ ...o }), Object.isFrozen({ ...o, c: 3 }), Object.isFrozen(rest), Object.isFrozen(Object.assign({}, o)), Object.isFrozen(Object.fromEntries(Object.entries(o))), Object.isFrozen(JSON.parse(JSON.stringify(o))), Object.isFrozen(Object.groupBy([o], (x) => x.a)), Object.isFrozen(Object.entries(o)), Object.isFrozen(Object.keys(o)), a]; })()
--- value
[false,false,false,false,false,false,false,false,false,false,1]

=== copies of a sealed value are not sealed
(() => { const a = Object.seal([2, 1]); const o = Object.seal({ k: 1 }); return [Object.isSealed([...a]), Object.isSealed(a.concat([3])), Object.isSealed(a.toSorted((x, y) => x - y)), Object.isSealed(a.with(0, 5)), Object.isSealed(a.toSpliced(0, 1)), Object.isSealed({ ...o }), Object.isSealed({ ...o, k: 2 })]; })()
--- value
[false,false,false,false,false,false,false]

=== a frozen array a last read moves is copied, not changed, by concat, with and toSpliced
(() => { const mk = () => Object.freeze([1, 2]); const c = mk().concat([3]); const w = mk().with(0, 9); const s = mk().toSpliced(0, 1, 7, 8); return [c, Object.isFrozen(c), w, Object.isFrozen(w), s, Object.isFrozen(s)]; })()
--- value
[[1,2,3],false,[9,2],false,[7,8,2],false]

=== a frozen array a last read moves is copied, not changed, by toSorted
@shapes - - COMPARE_NUMERIC(false) COMPARE_NUMERIC(true)
(() => { const mk = () => Object.freeze([3, 1, 2]); const s = mk().toSorted((a, b) => a - b); const t = Object.seal([2, 1]).toSorted((a, b) => b - a); return [s, Object.isFrozen(s), t, Object.isSealed(t)]; })()
--- value
[[1,2,3],false,[2,1],false]

=== a frozen value nothing else holds, spread into a literal, is copied
(() => { const o = [Object.freeze({ a: 1 }), Object.freeze([1])]; const p = { ...Object.freeze({ a: 1 }), b: 2 }; const q = [...Object.freeze([1]), 2]; const r = { ...Object.seal({ a: 1 }) }; return [p, Object.isFrozen(p), q, Object.isFrozen(q), r, Object.isSealed(r), o.length]; })()
--- value
[{"a":1,"b":2},false,[1,2],false,{"a":1},false,2]

=== reduce's accumulator, frozen, is copied by a spread
[1, 2, 3].reduce((acc, x) => ({ ...acc, [x]: x }), Object.freeze({}))
--- value
{"1":1,"2":2,"3":3}

=== reduce's accumulator, frozen, is copied by a spread, and stays so
(() => { const r = [1, 2].reduce((acc, x) => [...acc, x], Object.freeze([0])); return [r, Object.isFrozen(r)]; })()
--- value
[[0,1,2],false]

=== a frozen accumulator returned unchanged stays frozen
(() => { const r = [1, 2].reduce((acc) => acc, Object.freeze({ a: 1 })); return [r, Object.isFrozen(r)]; })()
--- value
[{"a":1},true]

=== Object.assign of a sealed target nothing else holds, with keys it has
[Object.assign(Object.seal({ a: 1, b: 2 }), { a: 3 }, { b: 4 }), Object.isSealed(Object.assign(Object.seal({ a: 1 }), { a: 2 })), Object.assign(Object.seal({ 0: 1, 1: 2 }), [5]), Object.assign(Object.seal({ 0: 1 }), "x"), Object.assign(Object.seal({ a: 1 }), { a: 1 }, null, 1, "")]
--- value
[{"a":3,"b":4},true,{"0":5,"1":2},{"0":"x"},{"a":1}]

=== Object.assign of a frozen target with nothing to put gives it
[Object.assign(Object.freeze({ a: 1 })), Object.assign(Object.freeze({ a: 1 }), {}, null, "", []), Object.isFrozen(Object.assign(Object.freeze({ a: 1 }), {}))]
--- value
[{"a":1},{"a":1},true]

=== Object.assign of a frozen target with something to put
Object.assign(Object.freeze({ a: 1 }), { a: 1 })
--- fault staticjs changes no key or element of a frozen

=== Object.assign of a sealed target with a key it lacks
Object.assign(Object.seal({ a: 1 }), { a: 2 }, { c: 1 })
--- fault staticjs adds and removes no key or element of a sealed

=== Object.assign of a sealed target with an index it lacks
Object.assign(Object.seal({ 0: 1 }), [5, 6])
--- fault staticjs adds and removes no key or element of a sealed

=== Object.assign of a sealed target with a character index it lacks
Object.assign(Object.seal({ 0: 1 }), "ab")
--- fault staticjs adds and removes no key or element of a sealed

=== Object.assign of a sealed empty target
Object.assign(Object.seal({}), { a: 1 })
--- fault staticjs adds and removes no key or element of a sealed

=== Object.assign of a sealed target with a hash index
(() => { const t = Object.seal({ a0: 0, a1: 1, a2: 2, a3: 3, a4: 4, a5: 5, a6: 6, a7: 7, a8: 8, a9: 9 }); const r = Object.assign(t, { a9: "x", a0: "y" }); return [r, Object.isSealed(r), Object.isFrozen(r)]; })()
--- value
[{"a0":"y","a1":1,"a2":2,"a3":3,"a4":4,"a5":5,"a6":6,"a7":7,"a8":8,"a9":"x"},true,false]

# Deliberate: JavaScript gives new Set([1]).
=== freeze of a Set
Object.freeze(new Set([1]))
--- fault staticjs freezes and seals objects and arrays only

# Deliberate: JavaScript gives new Map([]).
=== seal of a Map
Object.seal(new Map())
--- fault staticjs freezes and seals objects and arrays only

# Deliberate: JavaScript gives {}.
=== freeze of a Date
Object.freeze(new Date(0))
--- fault staticjs freezes and seals objects and arrays only

# Deliberate: JavaScript gives <function>.
=== freeze of a function
Object.freeze((x) => x)
--- fault staticjs freezes and seals objects and arrays only

# Deliberate: JavaScript gives <function>.
=== freeze of a built-in function
Object.freeze(Math.max)
--- fault staticjs freezes and seals objects and arrays only

# Deliberate: JavaScript gives /a/.
=== freeze of a regular expression
Object.freeze(/a/)
--- fault staticjs freezes and seals objects and arrays only

# Deliberate: JavaScript gives [1].values().
=== freeze of an iterator
Object.freeze([1].values())
--- fault staticjs freezes and seals objects and arrays only

# Deliberate: JavaScript gives {}.
=== freeze of a Temporal value
Object.freeze(Temporal.PlainDate.from("2024-01-31"))
--- fault staticjs freezes and seals objects and arrays only

# Deliberate: JavaScript gives true.
=== freeze of an object something else holds
(() => { const shared = { a: 1 }; const get = () => shared; return Object.isFrozen(Object.freeze(get())); })()
--- fault freezing or sealing changes a value, and something else holds

# Deliberate: JavaScript gives [true,[{"a":1}]].
=== seal of an element of an array
(() => { const xs = [{ a: 1 }]; return [Object.isSealed(Object.seal(xs[0])), xs]; })()
--- fault freezing or sealing changes a value, and something else holds

# Deliberate: JavaScript gives [{"a":1},{"a":2}].
=== freeze of each element in map
[{ a: 1 }, { a: 2 }].map((o) => Object.freeze(o))
--- fault freezing or sealing changes a value, and something else holds

=== freeze of a frozen value something else holds gives it
(() => { const shared = Object.freeze({ a: 1 }); const get = () => shared; const xs = [Object.seal([1])]; return [Object.freeze(get()), Object.isFrozen(Object.freeze(get())), Object.seal(get()), Object.seal(xs[0]), Object.freeze(Object.freeze(Object.freeze([2])))]; })()
--- value
[{"a":1},true,{"a":1},[1],[2]]

=== freeze of a sealed empty object something else holds gives it
(() => { const shared = Object.seal({}); const get = () => shared; return [Object.freeze(get()), Object.isFrozen(get())]; })()
--- value
[{},true]

# Deliberate: JavaScript gives [].
=== freeze of a sealed empty array something else holds
(() => { const shared = Object.seal([]); const get = () => shared; return Object.freeze(get()); })()
--- fault freezing or sealing changes a value, and something else holds

=== freeze and seal called through a value
(() => { const f = Object.freeze; const s = Object.seal; const [isFrozen, isSealed] = [Object.isFrozen, Object.isSealed]; return [Object.isFrozen(f({ a: 1 })), Object.isSealed(s([1])), isFrozen(f([])), isSealed({}), f(1), s()]; })()
--- value
[true,true,true,false,1,undefined]

=== an element that a last read moves out of its array is the value alone
(() => { const xs = [{ a: 1 }]; return Object.isSealed(Object.seal(xs[0])); })()
--- value
true

=== freeze's other arguments are ignored
[Object.freeze({ a: 1 }, { b: 2 }), Object.isFrozen({}, 1), Object.isSealed(Object.seal({}), {})]
--- value
[{"a":1},false,true]

# A module's namespace is sealed, as JavaScript's is: frozen only when it
# has no export, and freezing one with exports throws (Node 22).
=== a module for the namespace cases
@module integrity_ns
export const x = 1;
export const o = { a: 1 };
--- value
{"o":{"a":1},"x":1}

=== a module with no export
@module integrity_empty
import "integrity_ns";
--- value
{}

=== a namespace is sealed, and frozen only when it has no export
@module integrity_user
import * as ns from "integrity_ns";
import * as none from "integrity_empty";
export const sealed = Object.isSealed(ns);
export const frozen = Object.isFrozen(ns);
export const emptySealed = Object.isSealed(none);
export const emptyFrozen = Object.isFrozen(none);
export const copySealed = Object.isSealed({ ...ns });
export const resealed = Object.isSealed(Object.seal(ns));
export const refrozen = Object.isFrozen(Object.freeze(none));
export const inner = Object.isFrozen(ns.o);
--- value
{"copySealed":false,"emptyFrozen":true,"emptySealed":true,"frozen":false,"inner":false,"refrozen":true,"resealed":true,"sealed":true}

=== freeze of a namespace with exports
@module integrity_freeze
import * as ns from "integrity_ns";
export const f = Object.freeze(ns);
--- fault freezing or sealing changes a value, and something else holds

# Deliberate: JavaScript gives {"f":true}.
=== freeze of an export, which its module's namespace holds
@module integrity_inner
import * as ns from "integrity_ns";
export const f = Object.isFrozen(Object.freeze(ns.o));
--- fault freezing or sealing changes a value, and something else holds
