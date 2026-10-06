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
