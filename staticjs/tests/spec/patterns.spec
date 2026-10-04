# Default values and rest elements: in parameters, in array and object
# patterns, and in const. A default is taken only for undefined, evaluated
# only then, after the targets before it are bound.

# ------------------------------------------------------------------------
# Parameter defaults

=== a default fills a missing argument
((a, b = 1) => a + b)(1)
--- value
2

=== a default is not taken when the argument is given
((a, b = 1) => a + b)(1, 2)
--- value
3

=== a default is taken for undefined
((a, b = 1) => [a, b])(1, undefined)
--- value
[1,1]

=== a default is not taken for null
((a, b = 1) => [a, b])(1, null)
--- value
[1,null]

=== a default is not taken for 0
((a = 1) => a)(0)
--- value
0

=== a default is not taken for the empty string
((a = 1) => a)("")
--- value
""

=== a default is not taken for false
((a = 1) => a)(false)
--- value
false

=== a default reads an earlier parameter
((a, b = a * 2) => b)(3)
--- value
6

=== defaults read the ones before them
((a = 1, b = a + 1, c = a + b) => [a, b, c])()
--- value
[1,2,3]

=== a default is evaluated only when it is taken
((a = undefined.x) => a)(1)
--- value
1

=== a default can be an arrow function
((f = x => x * 2) => f(4))()
--- value
8

=== a default's closure captures an earlier parameter
((a, f = () => a) => f())(5)
--- value
5

=== a default's closure captures an earlier default
((a = 1, f = () => a) => f())()
--- value
1

=== a default holds a function with a default
((a = ((b = 2) => b)()) => a)()
--- value
2

=== a default holds a callback with a default
((x = [1, 2].map((y = 0) => y)) => x)()
--- value
[1,2]

=== a default before a block body
((a = 2) => { const b = a * a; return b; })()
--- value
4

=== a default before a trailing comma
((a, b = 1,) => a + b)(1)
--- value
2

=== a template as a default
((a = `x${1}`) => a)()
--- value
"x1"

=== ?? in a default
((a = null ?? 1) => a)()
--- value
1

=== in in a default
((a = "x" in {x: 1}) => a)()
--- value
true

=== brackets of every kind in defaults
((a = (1 + 2) * [3][0], b = {c: [1]}.c) => a + b[0])()
--- value
10

=== an object literal in a template in a default
((a = `${ {b: 1}.b }`) => a)()
--- value
"1"

=== a callback's default takes the place of undefined elements
[1, undefined, 3].map((x = 0) => x)
--- value
[1,0,3]

=== a default for an argument reduce passes
[1, 2].reduce((acc, x, i, all = []) => acc + x, 0)
--- value
3

=== a default for an object pattern
(({a, b} = {a: 1, b: 2}) => a + b)()
--- value
3

=== a default for an array pattern
(([x, y] = [1, 2]) => x + y)()
--- value
3

=== a pattern's default is not taken when the argument is given
(({a} = {a: 1}) => a)({a: 2})
--- value
2

=== a const in a default's block
((f = () => { const {x = 1} = {}; return x; }) => f())()
--- value
1

=== a conditional in a default
((a, b = a > 1 ? "big" : "small") => b)(2)
--- value
"big"

# ------------------------------------------------------------------------
# Rest parameters

=== a rest parameter takes every argument
((...args) => args)(1, 2, 3)
--- value
[1,2,3]

=== a rest parameter with no arguments is empty
((...args) => args)()
--- value
[]

=== a rest parameter after one that is missing
((a, ...rest) => [a, rest])(1)
--- value
[1,[]]

=== a rest parameter takes the arguments after the others
((a, ...rest) => [a, rest])(1, 2, 3)
--- value
[1,[2,3]]

=== a rest parameter when fewer arguments come than parameters
((a, b, ...rest) => rest)(1)
--- value
[]

=== a rest parameter destructured as an array
((...[a, b]) => a + b)(1, 2, 3)
--- value
3

=== a rest parameter destructured as an object
((...{length}) => length)(1, 2)
--- value
2

=== a callback's rest parameter sees what map passes
[10, 20].map((...a) => a.length)
--- value
[3,3]

=== a rest parameter takes spread arguments
((...a) => a)(...[1, 2], ...new Set([3]))
--- value
[1,2,3]

=== a rest parameter takes a spread string by code point
((...a) => a.length)(..."hé")
--- value
2

=== a default before a rest parameter
((a = 1, ...r) => [a, r])()
--- value
[1,[]]

=== a closure captures a rest parameter
((...xs) => () => xs)(1)()
--- value
[1]

=== a rest parameter in a block body
((first, ...others) => { const n = others.length; return [first, n]; })("a", "b", "c")
--- value
["a",2]

# ------------------------------------------------------------------------
# Array patterns

=== a default and a rest element in const
(() => { const [first = 0, ...others] = [undefined, 2, 3]; return [first, others]; })()
--- value
[0,[2,3]]

=== an element's default reads an earlier element
(() => { const [a = 1, b = a + 1] = []; return [a, b]; })()
--- value
[1,2]

=== an element's default is not taken for null
(() => { const [a = 1] = [null]; return a; })()
--- value
null

=== head and tail
(([head, ...tail]) => [head, tail])([1, 2, 3])
--- value
[1,[2,3]]

=== head and tail of an empty array
(([head, ...tail]) => [head, tail])([])
--- value
[undefined,[]]

=== a rest element after a hole
(([, ...tail]) => tail)([1, 2])
--- value
[2]

=== a rest element after two holes and an element
(([a, , ...r]) => r)([1, 2, 3, 4])
--- value
[3,4]

=== a rest element past the end
(([a, b, c, ...r]) => r)([1])
--- value
[]

=== a rest element destructured as an array
(([a, ...[b, c]]) => [a, b, c])([1, 2, 3])
--- value
[1,2,3]

=== a rest element destructured as an object
(([a, ...{length}]) => length)([1, 2, 3])
--- value
2

=== a rest element of a set follows insertion order
(([a, ...r]) => r)(new Set([1, 2, 3]))
--- value
[2,3]

=== a rest element of a string goes by code point
(([a, ...r]) => [a, r])("😀xy")
--- value
["😀",["x","y"]]

=== the elements of a string go by code point
(([a, b]) => [a, b])("hé")
--- value
["h","é"]

=== an element of a string past a character outside the BMP
(([a, b, c]) => [a, b, c])("😀😁")
--- value
["😀","😁",undefined]

=== an element's default for an empty string
(([a = "z"]) => a)("")
--- value
"z"

=== a nested array pattern with a default
(([[a, b] = [1, 2]]) => a + b)([])
--- value
3

=== a nested object pattern with a default
(([{x} = {x: 5}]) => x)([undefined])
--- value
5

=== a nested pattern's default is not taken for a value
(([[a] = [1]]) => a)([[2]])
--- value
2

=== a default in a nested pattern inside a parameter's default
(([a = 1, [b = a + 1] = []] = []) => [a, b])()
--- value
[1,2]

=== a rest element in a nested const
(x => { const [[a, ...b], ...c] = x; return [a, b, c]; })([[1, 2], 3])
--- value
[1,[2],[3]]

# ------------------------------------------------------------------------
# Object patterns

=== defaults, a nested default and a rest property in const
(() => { const { a = 1, b: { c = 2 } = {}, ...more } = { d: 4, a: undefined }; return [a, c, more]; })()
--- value
[1,2,{"d":4}]

=== a renamed property with a default
(({a: b = 1}) => b)({})
--- value
1

=== a property's default is not taken for null
(({a = 1}) => a)({a: null})
--- value
null

=== a property's default is not taken for 0
(({a = 1}) => a)({a: 0})
--- value
0

=== a rest property leaves out the named keys
(({limit = 10, ...rest}) => [limit, rest])({a: 1, limit: undefined, b: 2})
--- value
[10,{"a":1,"b":2}]

=== a rest property keeps the source's key order
(({b, ...r}) => r)({b: 1, z: 2, 10: 3, a: 4, 2: 5})
--- value
{"2":5,"10":3,"z":2,"a":4}

=== a rest property of an array takes its indices
(({0: a, ...r}) => [a, r])(["x", "y", "z"])
--- value
["x",{"1":"y","2":"z"}]

=== a rest property of an array has no length
(({length, ...r}) => [length, r])([1, 2])
--- value
[2,{"0":1,"1":2}]

=== numeric keys read as their canonical text
(({1.50: a, 0x10: b}) => [a, b])({"1.5": 1, 16: 2})
--- value
[1,2]

=== a numeric key in const
(() => { const {1: b} = [1, 2]; return b; })()
--- value
2

=== a numeric key in a parameter
(({0: a}) => a)(["q"])
--- value
"q"

=== a string key with a default
(({"a-b": x = 3}) => x)({})
--- value
3

=== a computed key, left out of the rest
(() => { const k = "z"; const {[k]: v, ...r} = {z: 1, y: 2}; return [v, r]; })()
--- value
[1,{"y":2}]

=== a computed number key, left out of the rest
(({[1 + 1]: v, ...r}) => [v, r])({2: "a", 3: "b"})
--- value
["a",{"3":"b"}]

=== a computed number key, left out of an array's rest
(({[0]: v, ...r}) => [v, r])(["a", "b"])
--- value
["a",{"1":"b"}]

=== a computed key with a default
(({["a" + "b"]: v = 9}) => v)({})
--- value
9

=== a computed key reads an earlier parameter
((k, {[k]: v}) => v)("x", {x: 7})
--- value
7

=== a rest property of a number is empty
(({...r}) => r)(5)
--- value
{}

=== a rest property of a boolean is empty
(({...r}) => r)(true)
--- value
{}

=== a rest property of a set is empty
(({...r}) => r)(new Set([1]))
--- value
{}

=== a rest property of a function is empty
(({...r}) => r)(x => x)
--- value
{}

=== a rest property of an empty object
(({...r}) => r)({})
--- value
{}

=== rest properties nested
(({a: {...inner}, ...outer}) => [inner, outer])({a: {b: 1}, c: 2})
--- value
[{"b":1},{"c":2}]

=== a key named twice is left out once
(({a, a: b, ...r}) => [a, b, r])({a: 1, c: 2})
--- value
[1,1,{"c":2}]

=== a property's default reads an earlier property
(({a, b = a * 10}) => b)({a: 2})
--- value
20

=== a const in a held default
(o => { const {f = () => { const x = 1; return x; }} = o; return f(); })({})
--- value
1

=== a default that is an arrow function with a pattern in a const
(o => { const {f = ({x = 3}) => x} = o; return f({}); })({})
--- value
3

# ------------------------------------------------------------------------
# Modules

=== a module exports what a pattern with a default and a rest binds
@module patterned
export const {a = 1, ...rest} = {b: 2};
--- value
{"a":1,"rest":{"b":2}}

=== a module whose exports are destructured
@module pattern_source
export const a = 1;
export const c = 3;
export const b = 2;
--- value
{"a":1,"b":2,"c":3}

=== a rest property of a namespace takes the other exports
@module pattern_rest
import * as ns from "pattern_source";
const {a, ...others} = ns;
export default others;
--- value
{"default":{"b":2,"c":3}}

=== an array pattern with defaults at module level
@module pattern_array
export const [h = 0, ...t] = [undefined, 1, 2];
--- value
{"h":0,"t":[1,2]}

# ------------------------------------------------------------------------
# Not JavaScript

=== a trailing comma after a rest element
(([...a,]) => 0)
--- error syntax must be last

=== a trailing comma after a rest parameter
((...a,) => 0)
--- error syntax must be last

=== a parameter after a rest parameter
((...a, b) => 0)
--- error syntax must be last

=== a rest parameter with a default
((...a = []) => a)
--- error syntax no default value

=== a rest element with a default
(([...a = []]) => 0)
--- error syntax no default value

=== a rest property that is an object pattern
(({...{a}}) => 0)
--- error syntax takes a name

=== a rest property that is an array pattern
(({...[a]}) => 0)
--- error syntax takes a name

=== a property after a rest property
(({...a, b}) => 0)
--- error syntax must be last

=== a trailing comma after a rest property
(({...a,}) => 0)
--- error syntax must be last

=== a rest property with a default
(({...a = {}}) => 0)
--- error syntax no default value

=== an empty default
(([a = ]) => 0)
--- error syntax expected an expression

=== a parameter given twice
((a = 1, a) => a)
--- error syntax declared twice

=== a parameter given twice after a default that reads it
((a = b, b, b) => a)
--- error syntax declared twice

=== a parameter given twice after an early read and a function
((a = b, b, c = (x) => x, b) => a)
--- error syntax declared twice

=== a trailing comma after a rest element in const
(x => { const [a, ...b,] = x; return b; })
--- error syntax must be last

=== a shorthand default in an object literal
({a = 1})
--- error syntax only in a pattern

=== a shorthand default in parenthesized object literal assigned to
(({a = 1}) = {})
--- error syntax only in a pattern

=== a shorthand default in an operand of an array literal
([x + {a = 1}] = [])
--- error syntax only in a pattern

=== a shorthand default in a spread object literal
({...{a = 1}} = {})
--- error syntax only in a pattern

=== a shorthand default in an object literal read for a member
({a = 1}.a)
--- error syntax only in a pattern

=== a shorthand default in an argument
String({a = 1})
--- error syntax only in a pattern

=== a shorthand default in an object literal in a default
((x = {a = 1}) => x)
--- error syntax only in a pattern

=== a computed key needs a colon
(({[k]}) => 0)
--- error syntax expected :

=== a numeric key needs a colon
(({0}) => 0)
--- error syntax expected :

=== a string key cannot be a shorthand
(({"a" = 1}) => 0)
--- error syntax cannot be bound

=== use strict after a default
((a = 1) => { "use strict"; return a; })
--- error syntax use strict

=== use strict after an array pattern
(([a]) => { "use strict"; return a; })
--- error syntax use strict

=== use strict after a rest parameter
((...a) => { "use strict"; return a; })
--- error syntax use strict

=== use strict later in the directive prologue
(({a}) => { "a"; 'use strict'; return a; })
--- error syntax use strict

=== use strict in a prologue whose statements end at line breaks
(({a}) => { "a"
"use strict"
return a; })
--- error syntax use strict

=== a default that assigns to a number
((a = 1 = 2) => a)
--- error syntax cannot be assigned to

=== a default that assigns to a name
((a = b = 2) => a)
--- error unsupported assignment

# ------------------------------------------------------------------------
# JavaScript staticjs leaves out

=== an assignment pattern with a default
({a = 1} = {})
--- error unsupported assignment

=== a nested assignment pattern with a default
({b: {a = 1}} = {b: {}})
--- error unsupported assignment

=== an array assignment pattern with a default
([{a = 1}] = [{}])
--- error unsupported assignment

=== an assignment pattern with a default as a rest element
([...{a = 1}] = [{}])
--- error unsupported assignment

=== an assignment pattern with a default in a conditional
(true ? {a = 1} = {} : 0)
--- error unsupported assignment

=== a default reads a later parameter
((a = b, b) => a)
--- error unsupported before its declaration

=== a default reads its own parameter
((a = a) => a)
--- error unsupported before its declaration

=== a default reads its own parameter, which an outer function binds too
(x => ((x = x) => x)())
--- error unsupported before its declaration

=== a default's closure reads a name the body declares
((f = () => c) => { const c = 1; return f(); })
--- error unsupported before its declaration

=== a default reads a name the body declares
((a = c) => { const c = 1; return a; })
--- error unsupported before its declaration

=== a default reads a name the body declares, which is bound outside too
(c => ((a = c) => { const c = 1; return a; })())
--- error unsupported before its declaration

=== a property's default reads a later const
(x => { const {a = b} = x; const b = 1; return a; })
--- error unsupported before its declaration

=== a property's default reads its own name
(x => { const {a = a} = x; return a; })
--- error unsupported before its declaration

=== a default reads a later parameter, before a function in a default
((a = b, b, c = (x) => x) => a)
--- error unsupported before its declaration

=== a default's closure reads its own parameter
((f = () => f) => f)
--- error unsupported before its declaration

=== use strict after plain parameters
((a) => { "use strict"; return a; })
--- error unsupported

=== an escaped use strict is not a directive
((a = 1) => { "use\x20strict"; return a; })
--- error unsupported

=== use strict after an empty statement is not a directive
(({a}) => { ; "use strict"; return a; })
--- error unsupported

=== a string statement that goes on is not a directive
(({a}) => { "use strict" + 1; return a; })
--- error unsupported

# ------------------------------------------------------------------------
# Names

=== a default that reads a name bound nowhere
((a = nothing) => a)()
--- error name not bound

# ------------------------------------------------------------------------
# Faults

=== destructuring a number with [ ]
(([a]) => a)(5)
--- fault an array, a set or a string

=== a rest element of undefined
(([...r]) => r)()
--- fault an array, a set or a string

=== an object pattern of a missing argument
(({a}) => a)()
--- fault destructured

=== an object pattern after a default, of a missing argument
((a = 1, {b}) => b)()
--- fault destructured

=== a rest property of a string
(({...r}) => r)("ab")
--- fault spread a string

=== a default that reads a prototype's property
(({a = {}.toString}) => a)({})
--- fault no prototype

=== a computed key that is an object
(({[{}]: v}) => v)({})
--- fault a key must be a primitive

=== a default that faults when it is taken
((a = undefined.x) => a)()
--- fault property of undefined
