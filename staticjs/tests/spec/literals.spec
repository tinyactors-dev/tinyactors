# Literals: numbers, strings, templates, arrays, objects.

=== integers
42
--- value
42

=== decimals print as JavaScript prints them
[0.1, 1.5, 1e21, 1e-7, 123e-20, 0.000001, 100, 1e20, 1e21 * 1e21, 1 / 3e22]
--- value
[0.1,1.5,1e+21,1e-7,1.23e-18,0.000001,100,100000000000000000000,1e+42,3.3333333333333333e-23]

=== a sum that is not exact
0.1 + 0.2
--- value
0.30000000000000004

=== hexadecimal, octal and binary
[0x1F, 0o17, 0b101, 0XFF]
--- value
[31,15,5,255]

=== numeric separators
1_000_000 + 0.000_1
--- value
1000000.0001

=== separators in every part of a number
[1_0.2_5, 0x_F] === undefined
--- error syntax separator

=== separators where JavaScript allows them
[1_0.2_5, 1e1_0, 0xF_F, 0.0_1]
--- value
[10.25,10000000000,255,0.01]

=== a leading dot and an exponent
[.5, 5., 2e3, 2E-3, 1.5e+2]
--- value
[0.5,5,2000,0.002,150]

=== negative zero
-0
--- value
-0

=== the largest safe integer
9007199254740991
--- value
9007199254740991

=== string escapes
["a\nb", 'it\'s', "tab\there", "\x41B\u{43}", "\0", "\\"]
--- value
["a\nb","it's","tab\there","ABC","\u0000","\\"]

=== a surrogate pair from escapes
"😀" === "😀"
--- value
true

=== a surrogate pair across a line continuation
"\uD83D\
\uDE00" === "😀"
--- value
true

=== an empty string
""
--- value
""

=== UTF-8 passes through
"héllo wörld 日本"
--- value
"héllo wörld 日本"

=== a template with no substitutions
`plain`
--- value
"plain"

=== a template with substitutions
`a${1 + 1}b${"c"}d${true}e${null}f${undefined}`
--- value
"a2bcdtrueenullfundefined"

=== a template with a number that is not an integer
`${0.5}|${-0}|${1e21}`
--- value
"0.5|0|1e+21"

=== nested templates
`outer ${`inner ${1}`} done`
--- value
"outer inner 1 done"

=== braces inside a substitution
`${({a: 1}).a}`
--- value
"1"

=== a template across lines
`one
two`
--- value
"one\ntwo"

=== an empty array
[]
--- value
[]

=== an array with a trailing comma
[1, 2, 3,]
--- value
[1,2,3]

=== nested arrays
[[1, [2, [3]]], []]
--- value
[[1,[2,[3]]],[]]

=== spreading arrays, sets and strings into an array
[...[1, 2], ...new Set([3, 3, 4]), ..."hé"]
--- value
[1,2,3,4,"h","é"]

=== an empty object
({})
--- value
{}

=== properties keep their order
({b: 1, a: 2, c: 3})
--- value
{"b":1,"a":2,"c":3}

=== array-index keys come first, in ascending order
({b: 1, 10: "ten", a: 2, 2: "two", "01": "not an index"})
--- value
{"2":"two","10":"ten","b":1,"a":2,"01":"not an index"}

=== a repeated key keeps its first place and its last value
({a: 1, b: 2, a: 3})
--- value
{"a":3,"b":2}

=== shorthand properties
((x, y) => ({x, y}))(1, 2)
--- value
{"x":1,"y":2}

=== computed keys
({["a" + "b"]: 1, [1 + 1]: 2, [true]: 3})
--- value
{"2":2,"ab":1,"true":3}

=== numeric keys are canonical
({1.50: "a", 0x10: "b"})
--- value
{"16":"b","1.5":"a"}

=== keywords as keys
({default: 1, new: 2, if: 3, class: 4})
--- value
{"default":1,"new":2,"if":3,"class":4}

=== spreading objects
({a: 1, ...{b: 2, a: 3}, c: 4})
--- value
{"a":3,"b":2,"c":4}

=== spreading an array into an object
({...["x", "y"]})
--- value
{"0":"x","1":"y"}

=== spreading null, undefined and a number into an object
({...null, ...undefined, ...1, a: 1})
--- value
{"a":1}

=== a set
new Set([1, "1", 1, NaN, NaN, 0, -0])
--- value
new Set([1,"1",NaN,0])

=== an empty set
new Set()
--- value
new Set([])

=== undefined, NaN and Infinity
[undefined, NaN, Infinity, -Infinity]
--- value
[undefined,NaN,Infinity,-Infinity]

=== comments
[1, /* two */ 2, // three
3]
--- value
[1,2,3]
