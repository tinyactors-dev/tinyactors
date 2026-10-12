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
# Keys, key order and null

=== a string key is not read as a number
(({"01": a, ...r}) => [a, r])({1: "x", "01": "y"})
--- value
["y",{"1":"x"}]

=== numeric keys with separators, fractions and prefixes
(({1_0: a, .5: b, 0b11: c}) => [a, b, c])({10: 1, "0.5": 2, 3: 3})
--- value
[1,2,3]

=== a numeric key past 1e21
(({1e21: a}) => a)({"1e+21": 5})
--- value
5

=== 2^32 - 1 is not an array index, so object rest keeps it in insertion order
(({b, ...r}) => r)({x: 1, 4294967295: 2, 4294967294: 3})
--- value
{"4294967294":3,"x":1,"4294967295":2}

=== a computed key reads an earlier parameter
((a, {[a]: v = a}) => v)("k", {})
--- value
"k"

=== computed keys null and undefined are left out of the rest
(({[null]: v, [undefined]: w, ...r}) => [v, w, r])({null: 1, undefined: 2, x: 3})
--- value
[1,2,{"x":3}]

=== a computed key -0 is the key 0
(({[-0]: v, ...r}) => [v, r])({0: "a", 1: "b"})
--- value
["a",{"1":"b"}]

=== a key named twice is left out of the rest once
(({a: x, ["a"]: y, ...r}) => r)({a: 1})
--- value
{}

=== null does not take a parameter's default, and a closure sees both
((a = 1, b = a, c = () => [a, b]) => c())(undefined, null)
--- value
[1,null]

=== null does not take a property's default
(({a = 1, b = a}) => [a, b])({a: null})
--- value
[null,null]

=== a default over a set's element
(([a = 1, ...r]) => [a, r])(new Set([undefined, 2]))
--- value
[1,[2]]

=== a hole, then a default
(([, b = 2]) => b)([1])
--- value
2

=== a rest parameter after a default
((a = 1, ...r) => [a, r])(undefined, 2)
--- value
[1,[2]]

=== a default reads the outer name when the body declares it in a nested function
(c => ((a = c) => { const f = () => { const c = 1; return c; }; return [a, f()]; })())(5)
--- value
[5,1]

=== a function in a default declares its own name
(c => ((f = () => { const c = 2; return c; }) => { const c = 1; return [c, f()]; })())(5)
--- value
[1,2]

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

=== a parenthesized default is not a parameter
((a = 1)) => a
--- error syntax

=== a string among parameters
((a, "b") => a)
--- error syntax

=== a number among parameters
((a, 1) => a)
--- error syntax

=== an operation among parameters
((a, b + 1) => a)
--- error syntax

=== a member with a default among parameters
((a.b = 1) => 0)
--- error syntax

=== a parenthesized name after a default
((a = 1, (b)) => 0)
--- error syntax

=== a parenthesized name among parameters
((a, (b)) => 1)
--- error syntax

=== a parenthesized name with a default as a parameter
(((a) = 1) => a)
--- error syntax

=== a default that is an arrow function with a number for its parameter
(x = 1 => x)
--- error syntax

=== a member with a default in an array parameter
(([a.b = 1]) => 1)
--- error syntax

=== an index with a default in an array parameter
(([a[0] = 1]) => 1)
--- error syntax

=== a member with a default in an object parameter
(({a: b.c = 1}) => b)
--- error syntax

=== a call with a default in an object parameter
(({a: f() = 1}) => 1)
--- error syntax

=== a parenthesized element with a default in an array parameter
(([(a) = 1]) => 1)
--- error syntax

=== a compound assignment as a parameter
((a += 1) => 1)
--- error syntax

=== a logical assignment as a parameter
((a ||= 1) => 1)
--- error syntax

=== a compound assignment in an object parameter
(({a: b += 1}) => 1)
--- error syntax

=== a logical assignment in an object parameter
(({a: b ??= 1}) => 1)
--- error syntax

=== a compound assignment in an array parameter
(([a += 1]) => 1)
--- error syntax

=== a member in an array parameter
(([a.b]) => 0)
--- error syntax

=== a member as an object parameter's rest
(({...a.b}) => 0)
--- error syntax

=== a getter in an object parameter
(({get a(){}}) => 1)
--- error syntax

=== a setter in an object parameter
(({set a(v){}}) => 1)
--- error syntax

=== a method in an object parameter
(({a() {}}) => 1)
--- error syntax

=== a comparison staticjs leaves out, as a parameter
((a == b) => 1)
--- error syntax

=== a bitwise operator staticjs leaves out, as a parameter
((a | b) => 1)
--- error syntax

=== => on the line after a default
((a = 1)
=> a)
--- error syntax on the line

=== a name in two parameters
((a, {a}) => 0)
--- error syntax twice

=== a name and the rest element in one parameter
(([a, ...a]) => 0)
--- error syntax twice

=== an assignment to an assignment
a = 1 = 2
--- error syntax cannot be assigned to

=== an assignment to an assignment, parenthesized
(a = 1 = 2)
--- error syntax cannot be assigned to

=== an assignment to a member, then to an assignment
a.b = 1 = 2
--- error syntax cannot be assigned to

=== an assignment pattern, then an assignment to it
[a] = 1 = 2
--- error syntax cannot be assigned to

=== an assignment whose value is assigned to
a = b + 1 = 2
--- error syntax cannot be assigned to

=== an arrow function's body assigns to an assignment
x => a = 1 = 2
--- error syntax cannot be assigned to

=== an element's default assigns to a number
[a = 1 = 2] = x
--- error syntax cannot be assigned to

=== a rest element with a default in an assignment pattern
[...a = 1] = x
--- error syntax cannot be assigned to

=== a rest property with a default in an assignment pattern
({...a = 1} = x)
--- error syntax cannot be assigned to

=== a rest element that is not last, in an assignment pattern
[...a, b = 1] = x
--- error syntax cannot be assigned to

=== a parenthesized assignment as an element of an assignment pattern
[(a = 1)] = x
--- error syntax cannot be assigned to

=== a call with a default in an assignment pattern
[f() = 1] = x
--- error syntax cannot be assigned to

=== this as an element of an assignment pattern
[this] = x
--- error syntax cannot be assigned to

=== new.target as an element of an assignment pattern
[new.target] = x
--- error syntax cannot be assigned to

=== a comma expression assigned to
(a, b) = 1
--- error syntax cannot be assigned to

=== a parenthesized comma expression with a trailing comma
(a, b,)
--- error syntax

=== a unary operand of **
(-a ** 2)
--- error syntax unary

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

=== an assignment in parentheses
(a = 1)
--- error unsupported assignment

=== an assignment to a parenthesized name
((a)) = 1
--- error unsupported assignment

=== an assignment pattern with a member and a default
[a.b = 1, (c) = 2] = []
--- error unsupported assignment

=== an assignment pattern with a default that assigns
[a = b = 1] = []
--- error unsupported assignment

=== an assignment pattern with this in a member
[this.x] = []
--- error unsupported this

=== an assignment in the branch of a conditional
(true ? 1 : a = 2)
--- error unsupported assignment

=== an assignment whose value is an arrow function
a = x => 1
--- error unsupported assignment

=== a shorthand default whose name is eval
({eval = 1} = {})
--- error unsupported

=== the comma operator as the whole expression
1, 2
--- error unsupported comma

=== the comma operator in an index
[1][0, 0]
--- error unsupported comma

=== the comma operator in a template
`${1, 2}`
--- error unsupported comma

=== the comma operator in a return
(x => { return 1, 2; })()
--- error unsupported comma

=== a regular expression in a default value, read ahead as one token
((a = /'/) => a.source)()
--- value
"'"

=== a regular expression in a shorthand default of an assignment pattern
({a = 1, b = /'/} = {})
--- error unsupported assignment

=== a generator method
({*a(){}})
--- error unsupported methods

=== an async method
({async a(){}})
--- error unsupported methods

=== an async generator method
({async *a(){}})
--- error unsupported methods

=== a method with a string for its name
({"a"(){}})
--- error unsupported methods

=== a method with a number for its name
({1(){}})
--- error unsupported methods

=== a method with a computed name
({[1](){}})
--- error unsupported methods

=== a getter with a number for its name
({get 1(){}})
--- error unsupported getters or setters

=== a getter with a computed name, then a property
({get [1](){ return 1; }, b: 2})
--- error unsupported getters or setters

=== ** with a parenthesized unary operand
((-1) ** 2)
--- value
1

=== new.target in a script's function
(x => new.target)
--- error unsupported new.target

=== a default's closure reads a name the body declares, through another default
(c => ((f = (g = () => c) => g()) => { const c = 1; return f(); })())(5)
--- error unsupported before its declaration

=== a default's function reads a name the body declares, in its own default
(c => ((f = ({x = c}) => x) => { const c = 1; return f({}); })())(5)
--- error unsupported before its declaration

=== a computed key reads a name the body declares
(c => (({[c]: v}) => { const c = "y"; return v; })({x: 1, y: 2}))("x")
--- error unsupported before its declaration

=== a rest parameter's default reads a name the body declares
(c => ((...[a = c]) => { const c = 1; return a; })())(5)
--- error unsupported before its declaration

=== an array parameter's default reads a name the body declares
(c => (([a = c]) => { const c = 1; return a; })([]))(5)
--- error unsupported before its declaration

=== a computed key reads a later parameter
(({[b]: v}, b) => v)
--- error unsupported before its declaration

=== a computed key reads its own target
(({[v]: v}) => v)
--- error unsupported before its declaration

=== a computed key in a const reads a later const
(x => { const {[b]: a} = x; const b = "k"; return a; })
--- error unsupported before its declaration

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
--- fault an array, a set, a map, an iterator or a string

=== a rest element of undefined
(([...r]) => r)()
--- fault an array, a set, a map, an iterator or a string

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

=== a nested pattern's default is not taken for null
(({a: {b} = {}}) => b)({a: null})
--- fault destructured

=== an object rest of null
(({...r}) => r)(null)
--- fault destructured

=== an object pattern over a number
(({a}) => a)(5)
--- fault reads properties

=== a default over a number's property is not taken
[1].map(({a = 2}) => a)
--- fault reads properties

=== an object pattern key that indexes half of a surrogate pair
(({0: c}) => c)("😀")
--- fault half of a character
