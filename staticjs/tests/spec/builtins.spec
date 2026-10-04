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

=== join
[[1, null, "a", undefined, true].join(), [1, 2].join(" - "), [].join()]
--- value
["1,,a,,true","1 - 2",""]

=== the original array does not change
(xs => { const sorted = xs.toSorted((a, b) => a - b); return [xs, sorted]; })([2, 1])
--- value
[[2,1],[1,2]]

=== mutating methods are not there
[1, 2].push(3)
--- fault do not change

=== reading a method without calling it
[1].map
--- fault calls methods

=== reading a string method staticjs does not have
"x".at
--- fault calls methods

=== typeof a string method staticjs does not have
typeof "x".padStart
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
--- error unsupported **

=== Number without **
[Number.isInteger(5), Number.isInteger(5.5), Number.isInteger("5"), Number.isSafeInteger(9007199254740992), Number.isFinite(Infinity), Number.isNaN(NaN), Number.MAX_SAFE_INTEGER]
--- value
[true,false,false,false,false,true,9007199254740991]

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
