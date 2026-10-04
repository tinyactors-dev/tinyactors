# Operators, and the order they bind in.

=== precedence
1 + 2 * 3 - 4 / 2
--- value
5

=== parentheses
(1 + 2) * 3
--- value
9

=== unary minus binds tighter than *
-2 * 3
--- value
-6

=== remainder is exact and takes the sign of the left
[7 % 3, -7 % 3, 7 % -3, 5.5 % 2, 0.3 % 0.1, -0 % 1, 1 % 0, 1e22 % 7]
--- value
[1,-1,1,1.5,0.09999999999999998,-0,NaN,4]

=== division
[1 / 0, -1 / 0, 0 / 0, 1 / 3]
--- value
[Infinity,-Infinity,NaN,0.3333333333333333]

=== adding strings
"a" + "b" + 1 + 2
--- value
"ab12"

=== a number first, then a string
1 + 2 + "a"
--- value
"3a"

=== adding primitives to a string
["" + null, "" + undefined, "" + true, "x" + 1.5, 2 + "x"]
--- value
["null","undefined","true","x1.5","2x"]

=== comparing numbers
[1 < 2, 2 <= 2, 3 > 4, 4 >= 4, NaN < 1, NaN >= NaN]
--- value
[true,true,false,true,false,false]

=== comparing strings by UTF-16 code unit
["a" < "b", "B" < "a", "abc" < "abd", "ab" < "abc", "￿" < "😀", "é" > "e"]
--- value
[true,true,true,true,false,true]

=== strict equality of primitives
[1 === 1, 1 === "1", NaN === NaN, 0 === -0, null === undefined, "a" === "a", 1 !== 2]
--- value
[true,false,false,true,false,true,true]

=== an object is never === a primitive
[[1] === 1, ({}) !== "x", null === ({})]
--- value
[false,true,false]

=== && and || return an operand
[1 && 2, 0 && 2, 1 || 2, 0 || 2, "" || null, null && undefined.x]
--- value
[2,0,1,2,null,null]

=== ?? replaces only null and undefined
[null ?? 1, undefined ?? 2, 0 ?? 3, "" ?? 4, false ?? 5]
--- value
[1,2,0,"",false]

=== ?? with parentheses around ||
(null || null) ?? "default"
--- value
"default"

=== short-circuit skips the right side
false && undefined.x
--- value
false

=== the conditional operator
[1 ? "yes" : "no", 0 ? "yes" : "no"]
--- value
["yes","no"]

=== nested conditionals associate to the right
(n => n < 0 ? "negative" : n === 0 ? "zero" : "positive")(0)
--- value
"zero"

=== a conditional in a conditional's test
(true ? false : true) ? 1 : 2
--- value
2

=== not
[!0, !1, !"", !"x", !null, !undefined, !NaN, ![], !{}]
--- value
[true,false,true,false,true,true,true,false,false]

=== typeof
[typeof 1, typeof "s", typeof true, typeof undefined, typeof null, typeof [], typeof {}, typeof (x => x), typeof new Set(), typeof Math.max]
--- value
["number","string","boolean","undefined","object","object","object","function","object","function"]

=== unary plus and minus on numbers
[+1, -(-2), -0 === 0]
--- value
[1,2,true]

=== in looks at own properties
["a" in {a: 1}, "b" in {a: 1}, 0 in [5], 1 in [5], "length" in []]
--- value
[true,false,true,false,true]

=== optional chaining on a value
({a: {b: 2}})?.a?.b
--- value
2

=== optional chaining stops at null
((o) => o?.a.b.c)(null)
--- value
undefined

=== optional chaining stops the whole chain
((o) => [o?.a.b, o?.a?.b])({a: null})
--- fault cannot read a property of null

=== optional index and call
((xs, f) => [xs?.[1], f?.(2), (null)?.[0], (undefined)?.(1)])([1, 2], x => x * 10)
--- value
[2,20,undefined,undefined]

=== optional chaining inside an argument
[1, 2].map(x => ({v: x})?.v)
--- value
[1,2]

=== member access on arrays and strings
[[1, 2, 3].length, [1, 2, 3][1], "héllo".length, "abc"[1], [1][5]]
--- value
[3,2,5,"b",undefined]

=== string length counts UTF-16 units
["😀".length, "a😀b".length]
--- value
[2,4]

=== an index that is not an integer
[[1, 2][1.5], [1, 2]["1"], [1, 2][-1]]
--- value
[undefined,2,undefined]
