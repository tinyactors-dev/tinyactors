# JSON.parse and JSON.stringify. Every expected value is what Node 22
# gives (JSON.stringify of the case's value, as diff-node shows it). Where
# JavaScript calls a function (a reviver, a replacer function, toJSON),
# makes a lone surrogate, gives an object the key __proto__ or reads a
# prototype, staticjs faults: those cases are fault cases, as is every
# SyntaxError and TypeError JavaScript throws.

# ------------------------------------------------------------------------
# JSON.parse: values

=== parse of the primitives
[JSON.parse("1"), JSON.parse("true"), JSON.parse("false"), JSON.parse("null"), JSON.parse('"s"')]
--- value
[1,true,false,null,"s"]

=== parse skips JSON's white space around a value: space, tab, LF and CR
JSON.parse(" \t\n\r [ 1 ,\t2 ,\r\n{ \"a\" : 3 } ] \n")
--- value
[1,2,{"a":3}]

=== parse of nested arrays and objects
JSON.parse('{"a":[1,{"b":null,"c":[]},{}],"d":"e","f":{"g":{"h":[true,false]}}}')
--- value
{"a":[1,{"b":null,"c":[]},{}],"d":"e","f":{"g":{"h":[true,false]}}}

=== parse of empty containers and strings
[JSON.parse("[]"), JSON.parse("{}"), JSON.parse('""'), JSON.parse("[[]]"), JSON.parse('[{}, ""]')]
--- value
[[],{},"",[[]],[{},""]]

=== parse reads every escape
JSON.parse('"q\\"b\\\\s\\/b\\bf\\fn\\nr\\rt\\t"')
--- value
"q\"b\\s/b\bf\fn\nr\rt\t"

=== parse reads \u escapes in either case
[JSON.parse('"\\u0041\\u00e9\\u00E9\\u65e5\\u672C"'), JSON.parse('"\\u0000"').length, JSON.parse('"\\u001f\\u007F"')]
--- value
["Aéé日本",1,"\u001f"]

=== parse joins an escaped surrogate pair into one character
[JSON.parse('"\\uD83D\\uDE00"'), JSON.parse('"\\ud83d\\ude00"').length, JSON.parse('"a\\uD834\\uDD1Eb"')]
--- value
["😀",2,"a𝄞b"]

=== parse keeps text that is not ASCII as it is
[JSON.parse('"é 日本 😀"'), JSON.parse('{"é":"日本","😀":1}'), JSON.parse('"😀"').length]
--- value
["é 日本 😀",{"é":"日本","😀":1},2]

=== parse of a key with escapes
JSON.parse('{"a\\nb":1,"\\u0063":2,"\\"":3}')
--- value
{"a\nb":1,"c":2,"\"":3}

=== parse of a string that is an escape and nothing else
[JSON.parse('"\\n"'), JSON.parse('"\\\\"'), JSON.parse('"\\""')]
--- value
["\n","\\","\""]

# Numbers: JSON's grammar, read exactly (correctly rounded).

=== parse of integers and fractions
[JSON.parse("0"), JSON.parse("-1"), JSON.parse("123"), JSON.parse("1.5"), JSON.parse("-0.25"), JSON.parse("10.0")]
--- value
[0,-1,123,1.5,-0.25,10]

=== parse of exponents
[JSON.parse("1e2"), JSON.parse("1E2"), JSON.parse("1e+2"), JSON.parse("1e-2"), JSON.parse("2.5E-3"), JSON.parse("-1.5e3"), JSON.parse("0e5"), JSON.parse("1e0")]
--- value
[100,100,100,0.01,0.0025,-1500,0,1]

=== parse of -0 gives -0
[JSON.parse("-0"), JSON.parse("-0.0"), JSON.parse("-0e5"), JSON.parse("0"), JSON.parse("[-0]")]
--- value
[-0,-0,-0,0,[-0]]

=== parse of numbers past the doubles' range
[JSON.parse("1e400"), JSON.parse("-1e400"), JSON.parse("1e-400"), JSON.parse("-1e-400"), JSON.parse("1e99999999999999999999")]
--- value
[Infinity,-Infinity,0,-0,Infinity]

=== parse of the smallest and largest doubles
[JSON.parse("5e-324"), JSON.parse("2e-324"), JSON.parse("3e-324"), JSON.parse("2.4703282292062328e-324"), JSON.parse("1.7976931348623157e308"), JSON.parse("1.7976931348623158e308"), JSON.parse("1.7976931348623159e308")]
--- value
[5e-324,0,5e-324,5e-324,1.7976931348623157e+308,1.7976931348623157e+308,Infinity]

=== parse rounds 17-digit and longer decimals to the nearest double
[JSON.parse("0.1"), JSON.parse("0.30000000000000004"), JSON.parse("9007199254740993"), JSON.parse("9007199254740995"), JSON.parse("123456789012345678901234567890"), JSON.parse("0.1000000000000000055511151231257827"), JSON.parse("1.00000000000000011102230246251565404236316680908203125")]
--- value
[0.1,0.30000000000000004,9007199254740992,9007199254740996,1.2345678901234568e+29,0.1,1]

=== parse of a number with many digits
JSON.parse("0." + "0".repeat(400) + "1e400")
--- value
0.1

# Objects: JavaScript's key order, and the last of duplicate keys.

=== parse puts integer-like keys first, ascending, then the others in order
Object.keys(JSON.parse('{"b":1,"2":2,"1":3,"a":4,"10":5,"01":6,"-1":7,"4294967295":8,"4294967294":9,"1.5":10}'))
--- value
["1","2","10","4294967294","b","a","01","-1","4294967295","1.5"]

=== parse of an object with integer-like keys gives them in order
JSON.parse('{"3":"c","1":"a","2":"b","x":0}')
--- value
{"1":"a","2":"b","3":"c","x":0}

=== parse of a duplicate key keeps the last value in the first key's place
[JSON.parse('{"a":1,"b":2,"a":3}'), JSON.parse('{"2":1,"1":2,"2":3}'), JSON.parse('{"a":{"x":1},"a":[2]}')]
--- value
[{"a":3,"b":2},{"1":2,"2":3},{"a":[2]}]

=== parse of many keys, past the hash index
Object.keys(JSON.parse('{"k9":9,"k8":8,"k7":7,"k6":6,"k5":5,"k4":4,"k3":3,"k2":2,"k1":1,"k0":0,"k5":"five","9":"nine","3":"three"}'))
--- value
["3","9","k9","k8","k7","k6","k5","k4","k3","k2","k1","k0"]

=== parse of many integer keys in descending order
Object.keys(JSON.parse('{' + Array.from({length: 20}, (x, i) => `"${19 - i}":${i}`).join(",") + '}')).join()
--- value
"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19"

=== parse of a key read through a variable
(o => [o.a, o["b c"], o[1]])(JSON.parse('{"a":1,"b c":2,"1":3}'))
--- value
[1,2,3]

=== the value parse gives works with the built-ins
JSON.parse('{"xs":[3,1,2]}').xs.toSorted((a, b) => a - b).map(x => x * 2)
--- value
[2,4,6]

# Nesting: bounded by max_nesting (256 by default), without recursion.

=== parse of arrays nested 256 deep
(t => [t.length, JSON.stringify(JSON.parse(t)) === t])("[".repeat(256) + "]".repeat(256))
--- value
[512,true]

=== parse of objects nested 256 deep
(t => [t.length, JSON.stringify(JSON.parse(t)) === t])('{"a":'.repeat(256) + "1" + "}".repeat(256))
--- value
[1537,true]

=== parse of arrays nested past max_nesting faults, where JavaScript reads them
JSON.parse("[".repeat(257) + "]".repeat(257))
--- fault limit

=== parse of objects nested past max_nesting faults too
JSON.parse('{"a":'.repeat(257) + "1" + "}".repeat(257))
--- fault max_nesting

# The argument: a primitive is read as its string, as JavaScript reads it.

=== parse of a number, a boolean or null reads its string
[JSON.parse(12), JSON.parse(1e21), JSON.parse(-0), JSON.parse(true), JSON.parse(null), JSON.parse(1.5)]
--- value
[12,1e+21,0,true,null,1.5]

=== parse of undefined reads "undefined", a SyntaxError
JSON.parse()
--- fault SyntaxError

=== parse of NaN reads "NaN", a SyntaxError
JSON.parse(NaN)
--- fault SyntaxError

=== parse of a BigInt faults, where JavaScript reads its digits
JSON.parse(10n)
--- fault BigInt

=== parse of an array faults, where JavaScript reads its string
JSON.parse([1])
--- fault primitives

=== parse of an object faults
JSON.parse({})
--- fault primitives

=== parse ignores a second argument that is not a function
[JSON.parse("[1]", 5), JSON.parse("[1]", null), JSON.parse("[1]", "x"), JSON.parse("[1]", [1])]
--- value
[[1],[1],[1],[1]]

=== parse with a reviver faults, as JavaScript calls it back
JSON.parse('{"a":1}', (k, v) => v)
--- fault reviver

=== parse with a built-in function as the reviver faults too
JSON.parse("1", Number)
--- fault reviver

# What staticjs cannot hold.

=== parse of a lone high surrogate faults, where JavaScript keeps it
JSON.parse('"\\uD800"')
--- fault lone surrogate

=== parse of a lone low surrogate faults
JSON.parse('"\\uDC00"')
--- fault lone surrogate

=== parse of a high surrogate before another escape faults
JSON.parse('"\\uD83D\\u0041"')
--- fault lone surrogate

=== parse of a high surrogate before plain text faults
JSON.parse('"\\uD83Dx"')
--- fault lone surrogate

=== parse of two high surrogates faults
JSON.parse('"\\uD83D\\uD83D\\uDE00"')
--- fault lone surrogate

=== parse of the key __proto__ faults, where JavaScript makes an own property
JSON.parse('{"__proto__":1}')
--- fault __proto__

=== parse of an escaped __proto__ key faults too
JSON.parse('[{"a":1,"__pr\\u006fto__":{}}]')
--- fault __proto__

=== parse of the string __proto__ as a value is a string
JSON.parse('{"a":"__proto__"}')
--- value
{"a":"__proto__"}

# Syntax errors: each a SyntaxError in JavaScript.

=== parse of an empty text
JSON.parse("")
--- fault ends too early (a SyntaxError in JavaScript) [at position 0]

=== parse of white space alone
JSON.parse(" \n")
--- fault ends too early

=== parse of a trailing comma in an array
JSON.parse("[1,]")
--- fault unexpected character (a SyntaxError in JavaScript) [at position 3]

=== parse of a trailing comma in an object
JSON.parse('{"a":1,}')
--- fault a key must be a string (a SyntaxError in JavaScript) [at position 7]

=== parse of a comma alone
JSON.parse("[,]")
--- fault unexpected character

=== parse of a missing comma
JSON.parse("[1 2]")
--- fault a comma or ] must follow an item

=== parse of a missing comma between properties
JSON.parse('{"a":1 "b":2}')
--- fault a comma or } must follow a property

=== parse of a missing colon
JSON.parse('{"a" 1}')
--- fault a colon must follow a key

=== parse of a missing value
JSON.parse('{"a":}')
--- fault unexpected character

=== parse of a key that is not a string
JSON.parse("{a:1}")
--- fault a key must be a string

=== parse of a number as a key
JSON.parse("{1:1}")
--- fault a key must be a string

=== parse of a leading zero
JSON.parse("01")
--- fault text after the value (a SyntaxError in JavaScript) [at position 1]

=== parse of a negative leading zero
JSON.parse("-01")
--- fault text after the value

=== parse of a leading zero in an array
JSON.parse("[00]")
--- fault a comma or ] must follow an item

=== parse of a fraction without its integer part
JSON.parse(".5")
--- fault unexpected character

=== parse of a fraction without digits
JSON.parse("5.")
--- fault ends too early

=== parse of a fraction without digits before a bracket
JSON.parse("[5.]")
--- fault a number needs a digit here

=== parse of a fraction without digits before an exponent
JSON.parse("[5.e3]")
--- fault a number needs a digit here

=== parse of an exponent without digits
JSON.parse("1e")
--- fault ends too early

=== parse of an exponent with a sign and no digits
JSON.parse("[1e+]")
--- fault a number needs a digit here

=== parse of a leading plus
JSON.parse("+1")
--- fault unexpected character

=== parse of a minus alone
JSON.parse("-")
--- fault ends too early

=== parse of a minus before something else
JSON.parse("-a")
--- fault a number needs a digit here

=== parse of a hexadecimal number
JSON.parse("0x10")
--- fault text after the value

=== parse of NaN and Infinity as text
JSON.parse("[Infinity]")
--- fault unexpected character

=== parse of a bad escape
JSON.parse('"\\x"')
--- fault a bad escape in a string (a SyntaxError in JavaScript) [at position 2]

=== parse of an escaped single quote
JSON.parse('"\\\'"')
--- fault a bad escape

=== parse of a short \u escape
JSON.parse('"\\u12"')
--- fault a bad Unicode escape

=== parse of a \u escape with a letter that is not hex
JSON.parse('"\\u12G4"')
--- fault a bad Unicode escape

=== parse of a high surrogate before a bad \u escape
JSON.parse('"\\uD83D\\uZZZZ"')
--- fault a bad Unicode escape

=== parse of a backslash at the end
JSON.parse('"abc\\')
--- fault a string is not closed

=== parse of a line feed in a string
JSON.parse('"a\nb"')
--- fault a control character in a string (a SyntaxError in JavaScript) [at position 2]

=== parse of a tab in a string
JSON.parse('["a\tb"]')
--- fault a control character

=== parse of a NUL in a string
JSON.parse('"' + String.fromCharCode(0) + '"')
--- fault a control character

=== parse of a control character after an escape
JSON.parse('"\\n' + String.fromCharCode(31) + '"')
--- fault a control character

=== parse of an unterminated string
JSON.parse('"abc')
--- fault a string is not closed (a SyntaxError in JavaScript) [at position 4]

=== parse of an unterminated string after an escape
JSON.parse('"a\\nbc')
--- fault a string is not closed

=== parse of an unterminated array
JSON.parse("[1")
--- fault ends too early

=== parse of an unterminated object
JSON.parse('{"a":1')
--- fault ends too early

=== parse of an object that ends after its key
JSON.parse('{"a"')
--- fault ends too early

=== parse of an opening brace alone
JSON.parse("{")
--- fault ends too early

=== parse of a closing bracket alone
JSON.parse("]")
--- fault unexpected character

=== parse of a mismatched bracket
JSON.parse('[1}')
--- fault a comma or ] must follow an item

=== parse of a second value
JSON.parse("1 2")
--- fault text after the value (a SyntaxError in JavaScript) [at position 2]

=== parse of text after an object
JSON.parse('{"a":1}x')
--- fault text after the value (a SyntaxError in JavaScript) [at position 7]

=== parse counts the position in UTF-16 code units, as JavaScript does
JSON.parse('["😀é", x]')
--- fault unexpected character (a SyntaxError in JavaScript) [at position 8]

=== parse of single quotes
JSON.parse("'a'")
--- fault unexpected character

=== parse of a comment
JSON.parse("/* c */ 1")
--- fault unexpected character

=== parse of a misspelled true
JSON.parse("tru")
--- fault ends too early

=== parse of a misspelled null
JSON.parse("nul1")
--- fault unexpected character

=== parse of true followed by a letter
JSON.parse("truex")
--- fault text after the value

=== parse of a capitalized literal
JSON.parse("True")
--- fault unexpected character

=== parse of undefined as text
JSON.parse("undefined")
--- fault unexpected character

=== parse of a no-break space, which JavaScript's white space has and JSON's does not
JSON.parse("\u00A01")
--- fault unexpected character

=== parse of a vertical tab
JSON.parse("\v1")
--- fault unexpected character

=== parse of a line separator
JSON.parse("1 ")
--- fault text after the value

=== parse of a byte order mark
JSON.parse("﻿1")
--- fault unexpected character

=== parse of text that is not ASCII outside a string
JSON.parse("[é]")
--- fault unexpected character

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

# Round trips.

=== stringify of what parse gives is the text, without its white space
JSON.stringify(JSON.parse(' { "b" : [ 1 , 2.5e3 , -0 , "\\u00e9\\n" ] , "a" : { } , "1" : null } '))
--- value
"{\"1\":null,\"b\":[1,2500,0,\"é\\n\"],\"a\":{}}"

=== a round trip of the rows the benchmarks use
(rows => JSON.stringify(JSON.parse(JSON.stringify(rows))) === JSON.stringify(rows) && JSON.stringify(rows))([{id: "r000000", kwh: 0, priority: 0, tags: ["a", "b"]}, {id: "r000001", kwh: 3.7, priority: 1, tags: ["a", "b"]}, {id: "r000002", kwh: 7.4, priority: 2, tags: ["a", "b"]}, {id: "r000003", kwh: 11.1, priority: 3, tags: ["a", "b"]}])
--- value
"[{\"id\":\"r000000\",\"kwh\":0,\"priority\":0,\"tags\":[\"a\",\"b\"]},{\"id\":\"r000001\",\"kwh\":3.7,\"priority\":1,\"tags\":[\"a\",\"b\"]},{\"id\":\"r000002\",\"kwh\":7.4,\"priority\":2,\"tags\":[\"a\",\"b\"]},{\"id\":\"r000003\",\"kwh\":11.1,\"priority\":3,\"tags\":[\"a\",\"b\"]}]"

=== a round trip of the doubles the benchmarks use
JSON.parse(JSON.stringify([859.7941207808166, 394.3013383563367, 480.58787404949175, 188.94988582774553, 175.46217903306626, 590.0896671941921, 604.7802727761426, 850.2574670373677, 865.8799244302276, 560.7706204257373]))
--- value
[859.7941207808166,394.3013383563367,480.58787404949175,188.94988582774553,175.46217903306626,590.0896671941921,604.7802727761426,850.2574670373677,865.8799244302276,560.7706204257373]

=== a round trip of the strings the benchmarks use
JSON.parse(JSON.stringify(["say \"hi\" #0\nnaïve café ✓", "say \"hi\" #1\nnaïve café ✓"]))
--- value
["say \"hi\" #0\nnaïve café ✓","say \"hi\" #1\nnaïve café ✓"]

=== a round trip of an event the benchmarks use, indented
JSON.parse(JSON.stringify({name: "notify.ack", data: {id: "n2"}}, null, 2))
--- value
{"name":"notify.ack","data":{"id":"n2"}}

=== a round trip of the counts the benchmarks use
JSON.stringify(JSON.parse(JSON.stringify({sent: 4, acked: 1, rejected: 0, failed: 0})))
--- value
"{\"sent\":4,\"acked\":1,\"rejected\":0,\"failed\":0}"

# Spot-check cases for JSON (values generated by Node 22).

=== parse then stringify a nested document
JSON.stringify(JSON.parse('{"b":[1,2.5,-0,1e400,"x\\u00e9\\ud83d\\ude00"],"2":null,"a":{"1":true,"0":false},"b2":{}}'))
--- value
"{\"2\":null,\"b\":[1,2.5,0,null,\"xé😀\"],\"a\":{\"0\":false,\"1\":true},\"b2\":{}}"

=== integer-like keys and duplicate keys
JSON.stringify(JSON.parse('{"z":1,"10":2,"2":3,"z":4,"a":5}'))
--- value
"{\"2\":3,\"10\":2,\"z\":4,\"a\":5}"

=== exact numbers
JSON.parse("[0.1, 5e-324, 1.7976931348623157e308, 123456789012345678901, -0]").map(String)
--- value
["0.1","5e-324","1.7976931348623157e+308","123456789012345680000","0"]

=== stringify primitives and holes
[JSON.stringify("a\"b\n\u0001é"), JSON.stringify([undefined, () => 1, NaN, -0, Infinity]), JSON.stringify({a: undefined, f: () => 1, b: 1}), JSON.stringify(undefined) === undefined, JSON.stringify(null)]
--- value
["\"a\\\"b\\n\\u0001é\"","[null,null,null,0,null]","{\"b\":1}",true,"null"]

=== space as a number and a string
[JSON.stringify({a: [1, {b: 2}], c: []}, null, 2), JSON.stringify([1, [2]], null, "--"), JSON.stringify({}, null, 4)]
--- value
["{\n  \"a\": [\n    1,\n    {\n      \"b\": 2\n    }\n  ],\n  \"c\": []\n}","[\n--1,\n--[\n----2\n--]\n]","{}"]

=== array replacer
JSON.stringify({b: 1, a: 2, c: {a: 3, d: 4}, 1: 5}, ["a", "c", 1, "a"])
--- value
"{\"a\":2,\"c\":{\"a\":3},\"1\":5}"

=== Map, Set and iterators give {}
[JSON.stringify(new Map([[1, 2]])), JSON.stringify(new Set([1])), JSON.stringify({m: new Map(), s: [new Set()]})]
--- value
["{}","{}","{\"m\":{},\"s\":[{}]}"]

=== round trip with whitespace
JSON.stringify(JSON.parse(' \t\n\r [ 1 , { "a" : "b" } ] \n'))
--- value
"[1,{\"a\":\"b\"}]"

=== syntax error 0: "\"a\u0001\""
JSON.parse("\"a\u0001\"")
--- fault

=== syntax error 1: "\"\\q\""
JSON.parse("\"\\q\"")
--- fault

=== syntax error 2: "[1,]"
JSON.parse("[1,]")
--- fault

=== syntax error 3: "01"
JSON.parse("01")
--- fault

=== syntax error 4: "{\"a\":1}x"
JSON.parse("{\"a\":1}x")
--- fault

=== syntax error 5: "["
JSON.parse("[")
--- fault

=== syntax error 6: ".5"
JSON.parse(".5")
--- fault

=== syntax error 7: "+1"
JSON.parse("+1")
--- fault

=== syntax error 8: "{'a':1}"
JSON.parse("{'a':1}")
--- fault

=== syntax error 9: "[1 2]"
JSON.parse("[1 2]")
--- fault

=== stringify of a BigInt
JSON.stringify(1n)
--- fault

=== a reviver
JSON.parse("1", (k, v) => v)
--- fault
