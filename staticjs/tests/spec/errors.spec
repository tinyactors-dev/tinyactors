# What staticjs refuses: not JavaScript (syntax), JavaScript outside staticjs
# (unsupported), names that are not bound (name), and faults at run time.

=== an unterminated string
"abc
--- error syntax inside a string

=== a string across lines
["abc
def"]
--- error syntax cannot span lines

=== a missing operand
1 +
--- error syntax ends inside an expression

=== an unbalanced parenthesis
(1 + 2
--- error syntax expected )

=== ?? mixed with || needs parentheses
null ?? 1 || 2
--- error syntax cannot be mixed

=== && mixed with ?? needs parentheses
true && false ?? 1
--- error syntax cannot be mixed

=== a number followed by a name
3in [1]
--- error syntax cannot be followed by a name

=== => on the next line
(x)
=> x
--- error syntax on the line of its parameters

=== an arrow function as an operand
1 + x => x
--- error syntax cannot be an operand

=== a block-bodied arrow cannot be called directly
x => {}(1)
--- error syntax ends here

=== assignment
[1][0] = 2
--- error unsupported no assignment

=== compound assignment
[1][0] += 1
--- error unsupported no assignment

=== increment
[1][0]++
--- error unsupported no ++

=== loose equality
1 == 1
--- error unsupported === and !== only

=== a regular expression
/ab+c/.test("abc")
--- error unsupported regular expressions

=== a function expression
(function () { return 1; })
--- error unsupported does not have this

=== a class
(class {})
--- error unsupported does not have this

=== this
this
--- error unsupported does not have this

=== new of anything but Set
new Map()
--- error unsupported constructs only Set

=== exponent operator
2 ** 10
--- error unsupported no **

=== bitwise operators
5 & 3
--- error unsupported no bitwise

=== the comma operator
(1, 2)
--- error unsupported no comma operator

=== an array hole
[1, , 3]
--- error unsupported no holes

=== tagged templates
String.raw`x`
--- error unsupported no tagged templates

=== a getter
({ get x() { return 1; } })
--- error unsupported getters or setters

=== a method in an object literal
({ f() { return 1; } })
--- error unsupported no methods

=== __proto__ in a literal
({ __proto__: null })
--- error unsupported no prototype

=== a statement in a block
(x => { if (x) {} return 1; })
--- error unsupported holds const declarations and one return

=== code after return
(x => { return 1; const y = 2; })
--- error unsupported ends with its return

=== a value on the line after return
(() => { return
1; })
--- error unsupported on the line of its return

=== a default parameter
((a = 1) => a)
--- error unsupported default

=== an assignment in parentheses reads as a default
(a = 1)
--- error unsupported default

=== a rest parameter
((...xs) => xs)
--- error unsupported rest

=== a separator right after the point
0._5
--- error syntax separator

=== a separator at the end of a number
1_
--- error syntax separator

=== a legacy octal number
017
--- error unsupported legacy octal

=== a BigInt
10n
--- error unsupported BigInt

=== a number that needs more precision than staticjs reads
0.1234567890123456789
--- error unsupported precision

=== a number too small for the fast path
5e-324
--- error unsupported precision

=== an unknown name
nope + 1
--- error name not bound

=== a const that refers to itself
(() => { const f = x => f(x); return f; })
--- error name not bound

=== a const that refers to a later one through a closure
(() => { const g = () => h(); const h = () => 1; return g(); })
--- error name not bound

=== shadowing after use in the same block
(a => { const b = a; const a2 = 1; return b + a2; })(1)
--- value
2

=== a name read from outside, then declared in the block
((a) => () => { const b = a; const a = 2; return b; })(1)
--- error unsupported used before

=== rebinding undefined
(undefined => 1)
--- error unsupported rebind

=== a lone surrogate escape
"\uD800"
--- error unsupported lone surrogate

=== reading a property of undefined
undefined.x
--- fault of undefined

=== reading a property of null
null.x
--- fault of null

=== comparing two objects with ===
[1] === [1]
--- fault identity

=== adding an object to a string
"x" + [1]
--- fault a string and a primitive

=== adding a number and a boolean
1 + true
--- fault two numbers

=== arithmetic on a string
"3" * 2
--- fault on numbers

=== comparing a number and a string
1 < "2"
--- fault two numbers or two strings

=== a property found on Object.prototype
({}).toString
--- fault no prototype

=== hasOwnProperty is not there either
({}).hasOwnProperty("a")
--- fault no prototype

=== destructuring undefined
(([a]) => a)(undefined)
--- fault destructured

=== with() outside the array
[1].with(5, 2)
--- fault inside the array
