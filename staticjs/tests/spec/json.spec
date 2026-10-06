# JSON.stringify. Every expected value is what Node 22 gives (the case's
# value as diff-node shows it). Where JavaScript calls a function (a
# replacer function, toJSON), writes a lone surrogate or reads a prototype,
# staticjs faults: those cases are fault cases, as is every TypeError
# JavaScript throws.

# ------------------------------------------------------------------------
# JSON.stringify: primitives

=== stringify of the primitives
[JSON.stringify(1), JSON.stringify("s"), JSON.stringify(true), JSON.stringify(false), JSON.stringify(null)]
--- value
["1","\"s\"","true","false","null"]

=== stringify of undefined and of functions gives undefined, not a string
[JSON.stringify(undefined), JSON.stringify(), JSON.stringify(x => x), JSON.stringify(Math.max), typeof JSON.stringify(undefined)]
--- value
[undefined,undefined,undefined,undefined,"undefined"]

=== stringify of numbers uses Number::toString
[JSON.stringify(0.1), JSON.stringify(1e21), JSON.stringify(1e-7), JSON.stringify(123456789012345680000), JSON.stringify(5e-324), JSON.stringify(1.7976931348623157e308), JSON.stringify(-1.5), JSON.stringify(0.30000000000000004)]
--- value
["0.1","1e+21","1e-7","123456789012345680000","5e-324","1.7976931348623157e+308","-1.5","0.30000000000000004"]

=== stringify of NaN and the infinities gives null, and of -0 gives 0
[JSON.stringify(NaN), JSON.stringify(Infinity), JSON.stringify(-Infinity), JSON.stringify(-0), JSON.stringify([NaN, -0, Infinity]), JSON.stringify({a: -Infinity})]
--- value
["null","null","null","0","[null,0,null]","{\"a\":null}"]

=== stringify escapes quotes, backslashes and the short escapes
JSON.stringify("q\"b\\b\bf\fn\nr\rt\t/")
--- value
"\"q\\\"b\\\\b\\bf\\fn\\nr\\rt\\t/\""

=== stringify escapes every other control character as \u00XX
JSON.stringify(String.fromCharCode(0, 1, 2, 3, 4, 5, 6, 7, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 127))
--- value
"\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\u000b\\u000e\\u000f\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f \""

=== stringify keeps text that is not ASCII as it is
[JSON.stringify("é 日本 😀"), JSON.stringify("  "), JSON.stringify({"é": "😀"})]
--- value
["\"é 日本 😀\"","\"  \"","{\"é\":\"😀\"}"]

# Containers.

=== stringify of arrays and objects keeps their order
JSON.stringify({b: 1, a: [1, "x", null, true], c: {d: {}, e: []}, 2: 0, 1: 0})
--- value
"{\"1\":0,\"2\":0,\"b\":1,\"a\":[1,\"x\",null,true],\"c\":{\"d\":{},\"e\":[]}}"

=== stringify of empty containers
[JSON.stringify([]), JSON.stringify({}), JSON.stringify([[]]), JSON.stringify({a: {}})]
--- value
["[]","{}","[[]]","{\"a\":{}}"]

=== stringify leaves undefined and functions out of objects
JSON.stringify({a: undefined, b: x => x, c: 1, d: Math.abs, e: undefined})
--- value
"{\"c\":1}"

=== stringify of an object whose properties are all left out
[JSON.stringify({a: undefined}), JSON.stringify({a: undefined}, null, 2), JSON.stringify([{a: () => 1}], null, 2)]
--- value
["{}","{}","[\n  {}\n]"]

=== stringify writes undefined and functions in arrays as null
JSON.stringify([undefined, x => x, 1, Math.abs, undefined])
--- value
"[null,null,1,null,null]"

=== stringify of maps, sets and iterators gives {}
[JSON.stringify(new Map([[1, 2]])), JSON.stringify(new Set([1])), JSON.stringify(new Map([["a", 1]]).keys()), JSON.stringify({m: new Map(), s: [new Set([1, 2])]})]
--- value
["{}","{}","{}","{\"m\":{},\"s\":[{}]}"]

=== stringify of an iterator reads none of its values
(it => [JSON.stringify(it), [...it]])(new Map([[1, "a"], [2, "b"]]).values())
--- value
["{}",["a","b"]]

=== stringify of an iterator helper's iterator gives {}
JSON.stringify(new Map([[1, 2]]).keys().map(k => k * 2))
--- value
"{}"

=== stringify of a BigInt faults, a TypeError in JavaScript
JSON.stringify(1n)
--- fault BigInt

=== stringify of a BigInt in an array faults
JSON.stringify([1, 2n])
--- fault BigInt

=== stringify of a BigInt in an object faults
JSON.stringify({a: {b: 10n}})
--- fault BigInt

=== stringify of a toJSON that is not a function writes it as a property
JSON.stringify({toJSON: 1, a: [{toJSON: null}]})
--- value
"{\"toJSON\":1,\"a\":[{\"toJSON\":null}]}"

=== stringify of an object with a toJSON function faults, as JavaScript calls it
JSON.stringify({toJSON: () => 1})
--- fault toJSON

=== stringify of a nested toJSON function faults
JSON.stringify([1, {a: {toJSON: k => k}}])
--- fault toJSON

=== stringify of a key with characters to escape
JSON.stringify({"a\"b": 1, "\n": 2, "é": 3})
--- value
"{\"a\\\"b\":1,\"\\n\":2,\"é\":3}"

# space

=== stringify with a number of spaces
JSON.stringify({a: [1, {b: 2}], c: "d"}, null, 2)
--- value
"{\n  \"a\": [\n    1,\n    {\n      \"b\": 2\n    }\n  ],\n  \"c\": \"d\"\n}"

=== stringify with space between 0 and 10, clamped
[JSON.stringify([1], null, 0), JSON.stringify([1], null, -1), JSON.stringify([1], null, 1), JSON.stringify([1], null, 3.7), JSON.stringify([1], null, 10), JSON.stringify([1], null, 11), JSON.stringify([1], null, Infinity), JSON.stringify([1], null, NaN), JSON.stringify([1], null, -0.5)]
--- value
["[1]","[1]","[\n 1\n]","[\n   1\n]","[\n          1\n]","[\n          1\n]","[\n          1\n]","[1]","[1]"]

=== stringify with a space between 0 and 1 faults, where V8 breaks lines without indenting and the specification does not
JSON.stringify([1], null, 0.5)
--- fault between 0 and 1

=== stringify with a string as space
[JSON.stringify([1, [2]], null, "\t"), JSON.stringify([1], null, ""), JSON.stringify({a: 1}, null, "--")]
--- value
["[\n\t1,\n\t[\n\t\t2\n\t]\n]","[1]","{\n--\"a\": 1\n}"]

=== stringify takes the first 10 code units of a string as space
[JSON.stringify([1], null, "abcdefghijkl"), JSON.stringify([1], null, "😀😀😀😀😀😀"), JSON.stringify([1], null, "éééééééééééé")]
--- value
["[\nabcdefghij1\n]","[\n😀😀😀😀😀1\n]","[\néééééééééé1\n]"]

=== stringify with a space that would end inside a surrogate pair faults, where JavaScript writes a lone surrogate
JSON.stringify([1], null, "a😀😀😀😀😀")
--- fault surrogate pair

=== stringify ignores a space that is neither a number nor a string
[JSON.stringify([1], null, true), JSON.stringify([1], null, null), JSON.stringify([1], null, [2]), JSON.stringify([1], null, {}), JSON.stringify([1], null, 2n)]
--- value
["[1]","[1]","[1]","[1]","[1]"]

=== stringify writes empty containers as [] and {} with space
JSON.stringify({a: [], b: {}, c: [[]], d: [{}], e: {f: []}}, null, 2)
--- value
"{\n  \"a\": [],\n  \"b\": {},\n  \"c\": [\n    []\n  ],\n  \"d\": [\n    {}\n  ],\n  \"e\": {\n    \"f\": []\n  }\n}"

=== stringify indents nested containers
JSON.stringify([{a: [1, 2], b: {c: {d: null}}}, [], "x"], null, 4)
--- value
"[\n    {\n        \"a\": [\n            1,\n            2\n        ],\n        \"b\": {\n            \"c\": {\n                \"d\": null\n            }\n        }\n    },\n    [],\n    \"x\"\n]"

# The replacer.

=== stringify with an array replacer keeps those keys, in its order
JSON.stringify({a: 1, b: 2, c: 3}, ["c", "a"])
--- value
"{\"c\":3,\"a\":1}"

=== stringify's array replacer applies to nested objects but not to arrays
JSON.stringify({a: [{a: 1, b: 2}], b: {a: 3, c: 4}}, ["a"])
--- value
"{\"a\":[{\"a\":1}]}"

=== stringify's array replacer drops duplicates and keeps the first place
JSON.stringify({a: 1, b: 2}, ["b", "a", "b", "a"])
--- value
"{\"b\":2,\"a\":1}"

=== stringify's array replacer takes numbers as their strings
JSON.stringify({1: "one", 2: "two", "1.5": "x", a: 0}, [2, 1, 1.5, "1"])
--- value
"{\"2\":\"two\",\"1\":\"one\",\"1.5\":\"x\"}"

=== stringify's array replacer skips what is neither a string nor a number
JSON.stringify({a: 1, true: 2, null: 3}, [true, null, {}, ["a"], undefined, "a"])
--- value
"{\"a\":1}"

=== stringify's array replacer leaves out keys the object lacks
JSON.stringify({a: 1}, ["b", "a", "c"], 1)
--- value
"{\n \"a\": 1\n}"

=== stringify with an empty array replacer
[JSON.stringify({a: 1}, []), JSON.stringify([{a: 1}], []), JSON.stringify(1, [])]
--- value
["{}","[{}]","1"]

=== stringify with an array replacer and a map
JSON.stringify({m: new Map([["x", 1]])}, ["m", "x"])
--- value
"{\"m\":{}}"

=== stringify with an array replacer naming a key of Object.prototype faults, where JavaScript reads the prototype
JSON.stringify({a: 1}, ["toString"])
--- fault prototype [toString]

=== stringify with an array replacer naming __proto__ faults
JSON.stringify({a: 1}, ["__proto__"])
--- fault prototype

=== stringify with an array replacer naming a map's size faults, where JavaScript reads it
JSON.stringify(new Map([[1, 2]]), ["size"])
--- fault prototype [size]

=== stringify with an array replacer naming a set's method faults
JSON.stringify([new Set()], ["has"])
--- fault prototype

=== stringify with a replacer function faults, as JavaScript calls it back
JSON.stringify({a: 1}, (k, v) => v)
--- fault replacer function

=== stringify ignores a replacer that is neither a function nor an array
[JSON.stringify({a: 1}, 5), JSON.stringify({a: 1}, "a"), JSON.stringify({a: 1}, {a: 0}), JSON.stringify({a: 1}, new Set(["b"])), JSON.stringify({a: 1}, null, 1)]
--- value
["{\"a\":1}","{\"a\":1}","{\"a\":1}","{\"a\":1}","{\n \"a\": 1\n}"]
