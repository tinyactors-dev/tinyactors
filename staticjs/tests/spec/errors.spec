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

=== an operator with no operand before ]
[typeof]
--- error syntax expected an expression

=== ! with no operand after a comma
[1, !]
--- error syntax expected an expression

=== - with no operand before )
String(-)
--- error syntax expected an expression

=== return of an operator with no operand
(() => { return !; })
--- error syntax expected an expression

=== new with nothing to construct
[new]
--- error syntax what new constructs

=== new of a unary operator
new !x
--- error syntax what new constructs

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

=== assignment to a number
(1 = 2)
--- error syntax cannot be assigned to

=== assignment to a sum
(a + b = 1)
--- error syntax cannot be assigned to

=== assignment to an optional member
a?.b = 1
--- error syntax cannot be assigned to

=== assignment to a parenthesized array
([a]) = 1
--- error syntax cannot be assigned to

=== compound assignment to an array
[a] += 1
--- error syntax cannot be assigned to

=== logical assignment to a call
f() ||= 1
--- error syntax cannot be assigned to

=== assignment to an array of a number
[1] = []
--- error syntax cannot be assigned to

=== assignment to an array of a call
[f()] = []
--- error syntax cannot be assigned to

=== assignment to an object of a number
({a: 1} = {})
--- error syntax cannot be assigned to

=== assignment to an array with an element after its rest
[...a, b] = []
--- error syntax cannot be assigned to

=== assignment to an array with a comma after its rest
[...a,] = []
--- error syntax cannot be assigned to

=== assignment to an object whose rest is a pattern
({...[a]} = {})
--- error syntax cannot be assigned to

=== assignment to an object with a property after its rest
({...a, b} = {})
--- error syntax cannot be assigned to

=== assignment to an arrow function
(() => 0) = 1
--- error syntax cannot be assigned to

=== assignment to a template
`a` = 1
--- error syntax cannot be assigned to

=== assignment to a new set
new Set() = 1
--- error syntax cannot be assigned to

=== assignment to an array of a conditional
[a ? b : c] = []
--- error syntax cannot be assigned to

=== assignment after a unary operator
typeof a = 1
--- error syntax cannot be assigned to

=== assignment to a parenthesized array element
[([a])] = []
--- error syntax cannot be assigned to

=== assignment to a member
a.b = 1
--- error unsupported no assignment

=== assignment to a parenthesized name
(a) = 1
--- error unsupported no assignment

=== assignment to a call
f() = 1
--- error unsupported no assignment

=== compound assignment to a call
f() += 1
--- error unsupported no assignment

=== logical assignment to a name
a ||= 1
--- error unsupported no assignment

=== assignment to an array with a rest pattern
[a, ...[b]] = []
--- error unsupported no assignment

=== assignment to an object whose rest is a member
({...a.b} = {})
--- error unsupported no assignment

=== assignment to an object of an array of a member
({a: [b.c]} = {})
--- error unsupported no assignment

=== assignment in the else of a conditional
a ? b : c = 1
--- error unsupported no assignment

=== assignment of an assignment
[a] = [b] = []
--- error unsupported no assignment

=== assignment to a parenthesized member in an array
[(a.b)] = []
--- error unsupported no assignment

=== assignment in an arrow function's body
x => y = 1
--- error unsupported no assignment

=== increment
[1][0]++
--- error unsupported no ++

=== loose equality
1 == 1
--- error unsupported === and !== only

=== a regular expression with a lookahead
/a(?=b)/.test("ab")
--- error unsupported lookahead

=== a function expression
(function () { return 1; })
--- error unsupported does not have this

=== a class
(class {})
--- error unsupported does not have this

=== this
this
--- error unsupported does not have this

=== a reserved word no expression starts with
[default]
--- error syntax expected an expression

=== a word only strict code reserves
[let]
--- error unsupported does not have this

=== super is no expression outside a method
super.x
--- error syntax expected an expression

=== a private name outside a class
#x in {}
--- error syntax expected an expression

=== a private member outside a class
({}).#x
--- error syntax

=== a decorator
[@x]
--- error syntax

=== new.target in a script's expression, which is a function's body
(() => new.target)
--- error unsupported new.target

=== new. is only new.target
new.foo
--- error syntax new.target

# A script is sloppy: strict mode's reserved words, eval and arguments are
# names there, which staticjs does not bind; let is not a const's name.

=== eval as a parameter in a script
(eval) => 1
--- error unsupported does not bind

=== yield as a parameter in a script
(yield) => 1
--- error unsupported does not bind

=== let as a parameter in a script
let => 1
--- error unsupported does not bind

=== await in a parameter's pattern in a script
({ await }) => 1
--- error unsupported does not bind

=== yield as a const in a script
(() => { const yield = 1; return yield; })()
--- error unsupported does not bind

=== let cannot be a const's name
(() => { const let = 1; return 1; })()
--- error syntax cannot be bound

=== let cannot be a const's name in a pattern
(() => { const [let] = [1]; return 1; })()
--- error syntax cannot be bound

=== async is a name staticjs does not bind
(async) => 1
--- error unsupported does not bind

=== this cannot be a parameter
(this) => 1
--- error syntax cannot be bound

=== eval in a shorthand property
({ eval })
--- error unsupported does not have this

=== this cannot be a shorthand property
({ this })
--- error syntax cannot stand for a value

=== new of anything but Set and Map
new WeakMap()
--- error unsupported constructs only Set and Map

=== exponent operator of a string
"2" ** 10
--- fault arithmetic in staticjs is on numbers

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
String`x`
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

=== an if with an empty block
(x => { if (x) {} return 1; })(1)
--- value
1

=== a loop in a block
(x => { while (x) {} return 1; })
--- error unsupported no loops

=== a token no statement starts with, in a block
(() => { const a = 1; .foo })()
--- error syntax expected a statement

=== a token no statement starts with, after a block's return
(() => { return 1; ) })
--- error syntax expected a statement

=== break outside a loop
(() => { break; })
--- error syntax expected a statement

=== if with no condition
(() => { if; return 1; })
--- error syntax expected (

=== a statement of new with nothing to construct
(() => { new; return 1; })
--- error syntax expected a statement

=== a statement of typeof with no operand
(() => { typeof; return 1; })
--- error syntax expected a statement

=== let as a name in a script's block
(() => { let; return 1; })
--- error unsupported does not have this

=== a line break before ! ends a const in a block
(() => { const a = 1
!a; return a; })()
--- error unsupported expression statement

=== a line break before ~ ends a const in a block
(() => { const a = 1
~a; return a; })()
--- error unsupported expression statement

=== code after return is accepted, and never runs
(x => { return 1; const y = 2; })(0)
--- value
1

=== a value on the line after return
(() => { return
1; })
--- error unsupported on the line of its return

=== a token no statement starts with, on the line after return
(() => { return
.x })
--- error syntax expected a statement

=== a default parameter
((a = 1) => a)()
--- value
1

=== an assignment in parentheses is not a default
(a = 1)
--- error unsupported assignment

=== a rest parameter
((...xs) => xs)(1, 2)
--- value
[1,2]

=== a separator right after the point
0._5
--- error syntax separator

=== a separator at the end of a number
1_
--- error syntax separator

=== a legacy octal number
017
--- error unsupported legacy octal

=== a legacy octal number as a BigInt
01n
--- error syntax cannot start with 0

=== a numeric separator after a leading 0
0_1
--- error syntax separator

=== an unknown name
nope + 1
--- error name not bound

=== a const that refers to itself
(() => { const f = x => f(x); return f; })
--- error unsupported used before

=== a const that refers to a later one through a closure
(() => { const g = () => h(); const h = () => 1; return g(); })
--- error unsupported used before

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

=== a lone low surrogate in an object's key
({ "\uDC00": 1 })
--- error unsupported lone surrogate

=== a high surrogate before a line continuation is still alone
"\uD800\
x"
--- error unsupported lone surrogate

=== a high surrogate and a line continuation in a template
`\uD800\
x`
--- error unsupported lone surrogate

=== a string with a lone surrogate where an operator belongs
x "\uD800"
--- error syntax expected an operator

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

=== a fault in a callback on the third element
[1, 2, 3, 4].map(x => x === 3 ? x.k.j : x)
--- fault reads properties

=== a fault in a nested callback on the outer loop's third element
[1, 2, 3, 4].map(x => [x].map(y => y === 3 ? y.k.j : y))
--- fault reads properties

=== a fault in a comparator on its third call
[4, 3, 2, 1].toSorted((a, b) => { const d = a - b; return d === 2 || d === -1 ? a.k.j : d; })
--- fault reads properties
