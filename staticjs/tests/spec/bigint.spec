# BigInts: literals, values, conversions, arithmetic and comparison. Every
# expected value is what Node 22 gives (diff-node); a case that faults where
# Node gives a value says so.

=== decimal literals
[0n, 1n, 10n, 4294967295n, 4294967296n, 18446744073709551615n, 18446744073709551616n, 123456789012345678901234567890n]
--- value
[0n,1n,10n,4294967295n,4294967296n,18446744073709551615n,18446744073709551616n,123456789012345678901234567890n]

=== literals in other radices
[0x0n, 0x1Fn, 0XFFn, 0o7n, 0O777n, 0b1n, 0B1010n, 0xffffffffffffffffffffn, 0x0000000000000000000001n, 0b11111111111111111111111111111111n]
--- value
[0n,31n,255n,7n,511n,1n,10n,1208925819614629174706175n,1n,4294967295n]

=== separators in a literal
[1_000n, 0xFF_FFn, 0b1_0n, 1_2_3n]
--- value
[1000n,65535n,2n,123n]

=== a fraction is not a BigInt
1.5n
--- error syntax followed by a name

=== an exponent is not a BigInt
1e3n
--- error syntax followed by a name

=== a BigInt literal with a leading 0
01n
--- error syntax cannot start with 0

=== 00n
00n
--- error syntax cannot start with 0

=== a BigInt literal with a separator at its end
1_n
--- error syntax separator

=== a BigInt literal with a prefix and no digits
0xn
--- error syntax digits after its prefix

=== a BigInt literal followed by a name
1nn
--- error syntax followed by a name

=== a BigInt literal followed by a digit
0n1
--- error syntax followed by a name

=== typeof
[typeof 0n, typeof 10n, typeof 0x10n]
--- value
["bigint","bigint","bigint"]

=== strict equality compares the value
[1n === 1n, 1n !== 1n, 0n === 0n, 1n === 2n, 4294967296n === 4294967296n, 4294967296n === 4294967297n, 0x10n === 16n, 18446744073709551616n === 0x10000000000000000n]
--- value
[true,false,true,false,true,false,true,true]

=== a BigInt never strictly equals a number or a string
[1n === 1, 1 === 1n, 1n !== 1, 0n === 0, 1n === "1", 1n === true, 0n === null, 0n === undefined]
--- value
[false,false,true,false,false,false,false,false]

=== truthiness
[!0n, !1n, !!4294967296n, 0n ? "t" : "f", 1n ? "t" : "f", (0n || "or"), (1n && "and"), 0n ?? "nullish", Boolean(0n), Boolean(7n)]
--- value
[true,false,true,"f","t","or","and",0n,false,true]

=== a BigInt in an if
(x => { if (x) return "yes"; return "no"; })(0n)
--- value
"no"

=== + with a string gives the digits
["a" + 10n, 10n + "a", "" + 0n, "x" + 18446744073709551616n, 123456789012345678901234567890n + ""]
--- value
["a10","10a","0","x18446744073709551616","123456789012345678901234567890"]

=== templates give the digits
[`${10n}`, `a${0n}b${0x10n}c`, `${18446744073709551616n}`, `${1n}${2n}`]
--- value
["10","a0b16c","18446744073709551616","12"]

=== a string built up by + of BigInts
["a", "b", "c"].reduce((acc, x, i) => acc + x + 18446744073709551616n, "")
--- value
"a18446744073709551616b18446744073709551616c18446744073709551616"

=== a template building on a string nothing else holds
[1n, 22n, 333n].reduce((acc, x) => `${acc}${x},`, "")
--- value
"1,22,333,"

=== String
[String(0n), String(10n), String(4294967296n), String(0xffffffffffffffffffffffffffffffffn), String(1000000000n), String(999999999n), String(1000000000000000000n)]
--- value
["0","10","4294967296","340282366920938463463374607431768211455","1000000000","999999999","1000000000000000000"]

=== join
[[1n, 2n, 18446744073709551616n].join(), [0n].join("-")]
--- value
["1,2,18446744073709551616","0"]

=== a set holds a BigInt once
[new Set([1n, 1n]).size, new Set([1n, 1, "1"]).size, new Set([0n, 0x0n, 4294967296n, 0x100000000n]).size, new Set([1n, 2n]).has(2n), new Set([1n]).has(1)]
--- value
[1,3,2,true,false]

=== a set of BigInts
new Set([3n, 1n, 3n, 18446744073709551616n, 18446744073709551616n])
--- value
new Set([3n,1n,18446744073709551616n])

=== a large set of BigInts, with its index
(xs => new Set(xs).size)([1n, 2n, 3n, 4n, 5n, 6n, 7n, 8n, 9n, 10n, 1n, 2n, 3n, 4n, 5n, 6n, 7n, 8n, 9n, 10n, 4294967296n, 4294967296n])
--- value
11

=== set operations with BigInts
[new Set([1n, 2n, 3n, 4n, 5n, 6n, 7n, 8n, 9n]).intersection(new Set([9n, 10n, 1n])), new Set([1n, 2n]).union(new Set([2n, 3n])), new Set([1n, 2n]).difference(new Set([2n]))]
--- value
[new Set([9n,1n]),new Set([1n,2n,3n]),new Set([1n])]

=== includes and indexOf compare the value
[[1n, 2n].includes(2n), [1n, 2n].includes(2), [1n, 4294967296n].indexOf(4294967296n), [1n].indexOf(1), [1, 1n].lastIndexOf(1n)]
--- value
[true,false,1,-1,1]

=== a BigInt as a computed key is its digits
[{[10n]: 1}, {[0n]: "a", [18446744073709551616n]: "b"}, {1n: 2}, {0x10n: 3}]
--- value
[{"10":1},{"0":"a","18446744073709551616":"b"},{"1":2},{"16":3}]

=== a BigInt reads a property by its digits
[{10: "a"}[10n], [5, 6, 7][1n], "abc"[2n], 1n in {1: 2}, 2n in [1, 2], 3n in [1, 2]]
--- value
["a",6,"c",true,false,false]

=== a BigInt key in a pattern
(o => { const {10n: a, [20n]: b} = o; return [a, b]; })({10: "x", 20: "y"})
--- value
["x","y"]

=== a hexadecimal BigInt key in a pattern
(o => { const {0x10n: a} = o; return a; })({16: "x"})
--- error unsupported decimal only

=== Object.fromEntries with BigInt keys
Object.fromEntries([[1n, "a"], [18446744073709551616n, "b"]])
--- value
{"1":"a","18446744073709551616":"b"}

=== Object.groupBy by a BigInt
Object.groupBy([1n, 2n, 3n, 4n], x => x === 2n || x === 4n ? 2n : 1n)
--- value
{"1":[1n,3n],"2":[2n,4n]}

=== an array of BigInts and numbers
[1n, 1, "1", [2n], {k: 3n}]
--- value
[1n,1,"1",[2n],{"k":3n}]

=== a BigInt as a string method's argument faults
"ab".padStart(5, 1n)
--- fault BigInt into a string

=== concat with a BigInt faults
"ab".concat(1n)
--- fault BigInt into a string

=== replaceAll with a BigInt faults
"aba".replaceAll("a", 1n)
--- fault BigInt into a string

=== a BigInt has no properties staticjs reads
10n.length
--- fault reads properties

=== addition and subtraction, signs and zero
[1n + 2n, 1n - 2n, -1n + -2n, -1n - -2n, 5n + -5n, -5n - -5n, 0n - 0n, 0n - 7n, 7n - 0n, 0n + -7n, -0n, -(-3n)]
--- value
[3n,-1n,-3n,1n,0n,0n,0n,-7n,7n,-7n,0n,3n]

=== carries and borrows across limbs
[4294967295n + 1n, 4294967296n - 1n, 18446744073709551615n + 1n, 18446744073709551616n - 1n, 0xffffffffffffffffffffffffn + 1n, 0x1000000000000000000000000n - 1n, -4294967296n + 1n, 1n - 18446744073709551616n]
--- value
[4294967296n,4294967295n,18446744073709551616n,18446744073709551615n,79228162514264337593543950336n,79228162514264337593543950335n,-4294967295n,-18446744073709551615n]

=== multiplication
[3n * 4n, -3n * 4n, -3n * -4n, 0n * -5n, -5n * 0n, 1n * -9n, -1n * 9n, -1n * -9n, 4294967295n * 4294967295n, 18446744073709551615n * 18446744073709551615n, 123456789012345678901234567890n * -987654321098765432109876543210n]
--- value
[12n,-12n,12n,0n,0n,-9n,-9n,9n,18446744065119617025n,340282366920938463426481119284349108225n,-121932631137021795226185032733622923332237463801111263526900n]

=== division truncates toward zero
[7n / 2n, -7n / 2n, 7n / -2n, -7n / -2n, 1n / 3n, -1n / 3n, 0n / 5n, 6n / 3n, 18446744073709551616n / 4294967296n, 18446744073709551615n / 4294967295n, 340282366920938463463374607431768211455n / 18446744073709551617n, 123456789012345678901234567890123456789n / 987654321987654321n]
--- value
[3n,-3n,-3n,3n,0n,0n,0n,2n,4294967296n,4294967297n,18446744073709551615n,124999998748437501153n]

=== remainder takes the sign of the dividend
[7n % 2n, -7n % 2n, 7n % -2n, -7n % -2n, 6n % 3n, -6n % 3n, 1n % 18446744073709551616n, -1n % 18446744073709551616n, 18446744073709551617n % 4294967296n, 123456789012345678901234567890123456789n % 987654321987654321n, 340282366920938463463374607431768211455n % -18446744073709551617n]
--- value
[1n,-1n,1n,-1n,0n,0n,1n,-1n,1n,142745764920524676n,0n]

=== division by 0n
1n / 0n
--- fault divided by 0n

=== remainder by 0n
0n % 0n
--- fault divided by 0n

=== exponentiation
[2n ** 0n, 0n ** 0n, 0n ** 5n, 1n ** 1000n, (-1n) ** 3n, (-1n) ** 4n, (-2n) ** 3n, (-2n) ** 4n, 2n ** 32n, 2n ** 64n, 3n ** 40n, (-3n) ** 41n, 10n ** 30n, 2n ** 3n ** 2n, (2n ** 3n) ** 2n]
--- value
[1n,1n,0n,1n,-1n,1n,-8n,16n,4294967296n,18446744073709551616n,12157665459056928801n,-36472996377170786403n,1000000000000000000000000000000n,512n,64n]

=== a large power
[String(7n ** 500n).length, 2n ** 1000n % 1000000007n, (3n ** 2000n) / (3n ** 1998n), String(2n ** 10000n).slice(0, 20)]
--- value
[423,688423210n,9n,"19950631168807583848"]

=== a negative exponent
2n ** -1n
--- fault negative power

=== a power past max_bigint_bits
2n ** 100000000n
--- fault larger than the limits allow

=== a product past max_bigint_bits
(x => x * x)(2n ** 200000n)
--- fault larger than the limits allow

=== ** with a unary operand
(-2n ** 2n)
--- error syntax unary operator

=== ** of numbers
2 ** 2
--- value
4

=== unary + of a BigInt
+1n
--- fault unary +

=== a BigInt and a number in arithmetic
1n + 1
--- fault mixes a BigInt

=== a number and a BigInt in arithmetic
2 * 1n
--- fault mixes a BigInt

=== a BigInt and a boolean in arithmetic
1n - true
--- fault mixes a BigInt

=== a BigInt and undefined in +
1n + undefined
--- fault mixes a BigInt

=== a BigInt and a number in **
2n ** 2
--- fault mixes a BigInt

=== a number and a BigInt in **
2 ** 2n
--- fault mixes a BigInt

=== a BigInt and null in %
1n % null
--- fault mixes a BigInt

=== comparison of BigInts
[1n < 2n, 2n < 1n, 1n <= 1n, 1n >= 2n, -1n < 0n, -5n < -4n, -4294967296n < -4294967295n, 18446744073709551616n > 18446744073709551615n, 0n >= -0n]
--- value
[true,false,true,false,true,true,true,true,true]

=== comparison of a BigInt and a number, exactly
[1n < 1.5, 2n > 1.5, 1n <= 1, 1n >= 1, 1n < 1, 0n < -0, 0n <= -0, -1n < -0.5, -1n > -1.5, 1.5 < 2n, 1.5 > 1n, 9007199254740993n > 9007199254740992, 9007199254740992 < 9007199254740993n, 18446744073709551616n >= 18446744073709551616, 18446744073709551617n > 18446744073709551616, 2n ** 1024n > 1.7976931348623157e308, 1n > 5e-324, 0n < 5e-324, -1n < -5e-324]
--- value
[true,true,true,true,false,false,true,true,true,true,true,true,true,true,true,true,true,true,true]

=== comparison with NaN and Infinity
[1n < NaN, 1n > NaN, 1n <= NaN, 1n >= NaN, NaN < 1n, NaN >= 1n, 2n ** 2000n < Infinity, -(2n ** 2000n) > -Infinity, Infinity > 0n, -Infinity < 0n]
--- value
[false,false,false,false,false,false,true,true,true,true]

=== a BigInt compared with a string
1n < "2"
--- fault BigInt with a BigInt or a number

=== a BigInt compared with a boolean
0n < true
--- fault BigInt with a BigInt or a number

=== sorting BigInts with a comparator
[3n, 18446744073709551616n, -1n, 2n, 0n].toSorted((a, b) => a < b ? -1 : a > b ? 1 : 0)
--- value
[-1n,0n,2n,3n,18446744073709551616n]

=== a comparator that subtracts BigInts gives a BigInt, which JavaScript does not convert
@shapes COMPARE_NUMERIC(false)
[3n, 1n, 2n].toSorted((a, b) => a - b)
--- fault BigInt

=== a sum of BigInts in reduce
@shapes SUM(false)
[1n, 2n, 3n, 18446744073709551616n].reduce((a, x) => a + x, 0n)
--- value
18446744073709551622n

=== a sum mixing BigInts and numbers in reduce
@shapes SUM(false)
[1n, 2n, 3].reduce((a, x) => a + x, 0n)
--- fault mixes a BigInt

=== a sum that starts from a number
@shapes SUM(false)
[1n, 2n].reduce((a, x) => a + x, 0)
--- fault mixes a BigInt

=== a sum of a key of BigInts
@shapes SUM_KEY(n, false)
[{n: 1n}, {n: 18446744073709551615n}].reduce((a, x) => a + x.n, 0n)
--- value
18446744073709551616n

=== filters comparing BigInts with a literal
@shapes ELEMENT_COMPARE(GREATER, 2) ELEMENT_COMPARE(STRICT_EQUAL, 1) KEY_COMPARE(k, LESS, 1.5)
[[1n, 2n, 3n, 4n, 1.5].filter(x => x > 2), [1n, 1].filter(x => x === 1), [{k: 1n}, {k: 2n}].filter(r => r.k < 1.5)]
--- value
[[3n,4n],[1],[{"k":1n}]]

=== filters comparing BigInts with a BigInt literal
[[1n, 2n, 3n].filter(x => x === 2n), [1n, 2n, 3n].find(x => x > 1n), [{k: 1n}, {k: 2n}].findIndex(r => r.k >= 2n)]
--- value
[[2n],2n,1]

=== a predicate comparing a BigInt with a string
@shapes ELEMENT_COMPARE(LESS, "b")
[1n].filter(x => x < "b")
--- fault BigInt with a BigInt or a number

=== the largest BigInt the default limit allows, and one more
[String(2n ** 262143n).length, String((2n ** 262143n - 1n) * 2n + 1n).length]
--- value
[78913,78914]

=== past the default limit
2n ** 262144n
--- fault larger than the limits allow

=== += on a let of a BigInt
(xs => { let n = 0n; n += xs[0]; n *= 3n; n -= 1n; n /= 2n; n %= 7n; return n; })([10n])
--- value
0n

=== BigInt of numbers
[BigInt(0), BigInt(-0), BigInt(1), BigInt(-1), BigInt(4294967296), BigInt(9007199254740991), BigInt(9007199254740992 * 2048), BigInt(1e21), BigInt(-1e300), BigInt(Number.MAX_VALUE) === 2n ** 1024n - 2n ** 971n]
--- value
[0n,0n,1n,-1n,4294967296n,9007199254740991n,18446744073709551616n,1000000000000000000000n,-1000000000000000052504760255204420248704468581108159154915854115511802457988908195786371375080447864043704443832883878176942523235360430575644792184786706982848387200926575803737830233794788090059368953234970799945081119038967640880074652742780142494579258788820056842838115669472196386865459400540160n,true]

=== BigInt of a number that is not an integer
BigInt(1.5)
--- fault RangeError

=== BigInt of NaN
BigInt(NaN)
--- fault RangeError

=== BigInt of Infinity
BigInt(Infinity)
--- fault RangeError

=== BigInt of strings
[BigInt(""), BigInt("   "), BigInt("0"), BigInt("-0"), BigInt("+12"), BigInt("-12"), BigInt(" \n\t 42  "), BigInt("0x1F"), BigInt("0XfF"), BigInt("0o17"), BigInt("0b101"), BigInt("000123"), BigInt("123456789012345678901234567890"), BigInt("0x" + "f".repeat(40))]
--- value
[0n,0n,0n,0n,12n,-12n,42n,31n,255n,15n,5n,123n,123456789012345678901234567890n,1461501637330902918203684832716283019655932542975n]

=== BigInt of a decimal point
BigInt("1.0")
--- fault SyntaxError

=== BigInt of an exponent
BigInt("1e3")
--- fault SyntaxError

=== BigInt of a literal's n
BigInt("10n")
--- fault SyntaxError

=== BigInt of a signed hexadecimal string
BigInt("-0x10")
--- fault SyntaxError

=== BigInt of a prefix alone
BigInt("0x")
--- fault SyntaxError

=== BigInt of a separator
BigInt("1_000")
--- fault SyntaxError

=== BigInt of a binary string with a 2
BigInt("0b102")
--- fault SyntaxError

=== BigInt of Infinity's text
BigInt("Infinity")
--- fault SyntaxError

=== BigInt of booleans and BigInts
[BigInt(true), BigInt(false), BigInt(10n), BigInt(-4294967296n)]
--- value
[1n,0n,10n,-4294967296n]

=== BigInt of undefined
BigInt(undefined)
--- fault TypeError

=== BigInt of nothing
BigInt()
--- fault TypeError

=== BigInt of null
BigInt(null)
--- fault TypeError

=== BigInt of an object
BigInt({})
--- fault converts only primitives

=== BigInt of an array, which JavaScript turns into a string
BigInt([1])
--- fault converts only primitives

=== BigInt is a function
[typeof BigInt, typeof BigInt.asIntN]
--- value
["function","function"]

=== BigInt.asIntN and asUintN
[BigInt.asIntN(8, 255n), BigInt.asIntN(8, 127n), BigInt.asIntN(8, 128n), BigInt.asIntN(8, -128n), BigInt.asIntN(8, -129n), BigInt.asUintN(8, -1n), BigInt.asUintN(8, 256n), BigInt.asUintN(8, 257n), BigInt.asIntN(0, 5n), BigInt.asUintN(0, -5n), BigInt.asIntN(1, 1n), BigInt.asIntN(1, -1n)]
--- value
[-1n,127n,-128n,-128n,127n,255n,0n,1n,0n,0n,-1n,-1n]

=== asIntN and asUintN at 32 and 64 bits
[BigInt.asIntN(64, 2n ** 63n), BigInt.asIntN(64, 2n ** 63n - 1n), BigInt.asIntN(64, -(2n ** 63n)), BigInt.asIntN(64, -(2n ** 63n) - 1n), BigInt.asUintN(64, -1n), BigInt.asUintN(64, 2n ** 64n), BigInt.asUintN(64, -(2n ** 64n)), BigInt.asIntN(32, 4294967295n), BigInt.asUintN(32, -4294967296n), BigInt.asUintN(65, -1n), BigInt.asIntN(33, 4294967296n)]
--- value
[-9223372036854775808n,9223372036854775807n,-9223372036854775808n,9223372036854775807n,18446744073709551615n,0n,0n,-1n,0n,36893488147419103231n,-4294967296n]

=== asIntN and asUintN with many bits keep the value
[BigInt.asIntN(1000, -5n), BigInt.asUintN(1000, 5n), BigInt.asIntN(9007199254740991, 123n), BigInt.asIntN(9007199254740991, -(2n ** 100n)), BigInt.asUintN(9007199254740991, 2n ** 100n)]
--- value
[-5n,5n,123n,-1267650600228229401496703205376n,1267650600228229401496703205376n]

=== asIntN's bits converted
[BigInt.asIntN(8.9, 255n), BigInt.asUintN(undefined, 5n), BigInt.asUintN(null, 5n), BigInt.asUintN(true, 3n), BigInt.asIntN(8, true)]
--- value
[-1n,0n,0n,1n,1n]

=== asUintN of a negative value with more bits than the limit
BigInt.asUintN(9007199254740991, -1n)
--- fault larger than the limits allow

=== asIntN with negative bits
BigInt.asIntN(-1, 5n)
--- fault RangeError

=== asIntN with too many bits
BigInt.asIntN(9007199254740992, 5n)
--- fault RangeError

=== asIntN of a number
BigInt.asIntN(8, 255)
--- fault TypeError

=== asIntN of a string, which JavaScript reads as a BigInt
BigInt.asIntN(8, "255")
--- fault only in BigInt()

=== asIntN with a string of bits, which JavaScript converts
BigInt.asIntN("8", 255n)
--- fault converts only

=== asIntN with BigInt bits
BigInt.asIntN(8n, 255n)
--- fault TypeError

=== toString
[(0n).toString(), (255n).toString(), (255n).toString(16), (-255n).toString(16), (255n).toString(2), (-1n).toString(36), (12345678901234567890n).toString(36), (2n ** 64n).toString(16), (2n ** 64n - 1n).toString(8), (10n ** 30n).toString(7), (35n).toString(36), (255n).toString(undefined), (255n).toString(16.9)]
--- value
["0","255","ff","-ff","11111111","-1","2lsohxawjui8i","10000000000000000","1777777777777777777777","243230604464041356413054436032064451","z","255","ff"]

=== toString with a radix out of range
(10n).toString(37)
--- fault RangeError

=== toString with radix 1
(10n).toString(1)
--- fault RangeError

=== toString with a string radix, which JavaScript converts
(10n).toString("16")
--- fault converts only

=== other methods of a BigInt
(10n).valueOf()
--- fault does not have this method

=== toLocaleString of a BigInt
(10n).toLocaleString()
--- fault does not have this method

=== a method of a number, which staticjs does not have
(10).valueOf()
--- fault methods on arrays, strings, sets, maps, iterators and BigInts only

=== Number of BigInts, correctly rounded
[Number(0n), Number(-1n), Number(4294967296n), Number(9007199254740993n), Number(9007199254740995n), Number(-9007199254740993n), Number(18446744073709551615n), Number(2n ** 64n + 1n), Number(2n ** 1023n), Number(2n ** 1024n), Number(-(2n ** 1024n)), Number(2n ** 1024n - 2n ** 970n), Number(2n ** 1024n - 2n ** 970n - 1n), Number(123456789012345678901234567890n)]
--- value
[0,-1,4294967296,9007199254740992,9007199254740996,-9007199254740992,18446744073709552000,18446744073709552000,8.98846567431158e+307,Infinity,-Infinity,Infinity,1.7976931348623157e+308,1.2345678901234568e+29]

=== parseInt and parseFloat read a BigInt's digits
[parseInt(10n), parseInt(-10n), parseInt(10n, 16), parseInt(10n, 2), parseInt(123456789012345678901234567890n), parseFloat(10n), parseFloat(-123456789012345678901234567890n), Number.parseInt(255n, 36), Number.parseFloat(9007199254740993n)]
--- value
[10,-10,16,2,1.2345678901234568e+29,10,-1.2345678901234568e+29,2777,9007199254740992]

=== isNaN of a BigInt
isNaN(1n)
--- fault TypeError

=== Math of a BigInt
Math.abs(-1n)
--- fault TypeError

=== a BigInt as an index
[1, 2].at(0n)
--- fault TypeError

=== Number.isInteger of a BigInt
[Number.isInteger(1n), Number.isSafeInteger(1n), Number.isNaN(1n), Number.isFinite(1n)]
--- value
[false,false,false,false]

=== a round trip through text
(xs => xs.map(x => BigInt(String(x)) === x && BigInt("0x" + (x < 0n ? -x : x).toString(16)) === (x < 0n ? -x : x)))([0n, 1n, -1n, 4294967295n, -18446744073709551616n, 123456789012345678901234567890n, 3n ** 300n])
--- value
[true,true,true,true,true,true,true]

=== factorial of 30 by reduce
Array.from({length: 30}, (_, i) => BigInt(i + 1)).reduce((a, x) => a * x, 1n)
--- value
265252859812191058636308480000000n

=== an accumulator nothing else holds, added to in place, across limbs and signs
[[4294967295n, 1n, -4294967297n, -1n, 18446744073709551615n, -18446744073709551614n].reduce((a, x) => a + x, 0n), [3n, 5n, 7n].reduce((a, x) => x - a, 4294967296n), [1n, 2n].reduce((a, x) => -a - x, 5n)]
--- value
[-1n,-4294967291n,4n]

=== an accumulator multiplied in place, past its room
Array.from({length: 60}, (_, i) => BigInt(i + 4294967290)).reduce((a, x) => a * x, 1n) % 1000000000000000000007n
--- value
472010457201373978637n

=== an accumulator held elsewhere too is not changed
(xs => { const start = 4294967295n; const r = xs.reduce((a, x) => a + x, start); return [start, r, xs.reduce((a, x) => [...a, a.length === 0 ? x * 3n : a[a.length - 1] * x], [])]; })([1n, 2n, 3n])
--- value
[4294967295n,4294967301n,[3n,6n,18n]]

=== a let of a BigInt updated in place
(xs => { let n = 1n; n *= xs[0]; n *= xs[0]; n += 1n; n = -n; return [n, xs[0]]; })([18446744073709551615n])
--- value
[-340282366920938463426481119284349108226n,18446744073709551615n]

# Spot-check cases for BigInt (values generated by Node 22).

=== arithmetic across limb boundaries
[2n**64n - 1n, 2n**32n * 2n**32n, -(2n**70n) / 3n, -(2n**70n) % 3n, 7n % -2n, -7n / 2n, 0n - 0n].map(String)
--- value
["18446744073709551615","18446744073709551616","-393530540239137101141","-1","1","-3","0"]

=== literals in every radix
[0x1Fn, 0o17n, 0b101n, 123456789012345678901234567890n].map(String)
--- value
["31","15","5","123456789012345678901234567890"]

=== comparisons, mixed with numbers
[1n < 1.5, 2n > 1.5, 1n < NaN, 2n**60n > 1152921504606846976, 3n === 3n, 3n === 3, 10n >= 10]
--- value
[true,true,false,false,true,false,true]

=== BigInt() from numbers and strings
[BigInt(42), BigInt(-0), BigInt("  0x10 "), BigInt("-123"), BigInt(""), BigInt(true), BigInt(1152921504606846976)].map(String)
--- value
["42","0","16","-123","0","1","1152921504606846976"]

=== asIntN and asUintN
[BigInt.asIntN(8, 255n), BigInt.asUintN(8, -1n), BigInt.asIntN(64, 2n**63n), BigInt.asUintN(0, 5n)].map(String)
--- value
["-1","255","-9223372036854775808","0"]

=== toString radix, templates, Number()
[(255n).toString(16), (-255n).toString(2), `${10n}x`, String(Number(2n**80n + 1n)), String(Number(-(2n**1100n)))]
--- value
["ff","-11111111","10x","1.2089258196146292e+24","-Infinity"]

=== typeof, sets and keys
[typeof 1n, new Set([1n, 1n, 2n, -0n, 0n]).size, Object.keys({[10n]: 1, [-5n]: 2}), [3n, 1n, 2n].toSorted((a, b) => (a < b ? -1 : a > b ? 1 : 0)).map(String)]
--- value
["bigint",3,["10","-5"],["1","2","3"]]

=== reduce sum of bigints
String([1n, 2n, 3n, 2n**64n].reduce((a, x) => a + x, 0n))
--- value
"18446744073709551622"

=== mixing with a number
1n + 1
--- fault

=== division by zero
1n / 0n
--- fault

=== unary plus
+1n
--- fault

=== BigInt of a fraction
BigInt(1.5)
--- fault

=== a bad string
BigInt("1.5")
--- fault
