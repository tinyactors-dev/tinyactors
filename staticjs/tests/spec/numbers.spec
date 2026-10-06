# Numbers: literals read exactly, Number, parseInt and parseFloat, and the
# Number and Math constants and functions. Every expected value is Node's.

# A literal is read to the nearest double, ties to even, however many digits
# it has: Clinger's fast path where one rounding is exact, a bignum past it.
=== literals past the fast path
[0.1234567890123456789, 5e-324, 2e-324, 3e-324, 1e300, 1e308, 1.7976931348623157e308, 1.7976931348623158e308, 1.7976931348623159e308, 1e309, 1e999, 1e-400, 123456789012345678901234567890, 0.30000000000000004]
--- value
[0.12345678901234568,5e-324,0,5e-324,1e+300,1e+308,1.7976931348623157e+308,1.7976931348623157e+308,Infinity,Infinity,Infinity,0,1.2345678901234568e+29,0.30000000000000004]

=== literals at the edges of the subnormals
[2.2250738585072011e-308, 2.2250738585072012e-308, 2.2250738585072014e-308, 4.9406564584124654e-324, 2.4703282292062327e-324, 2.4703282292062328e-324, 1e-323, 1.5e-323, -0.0e-400]
--- value
[2.225073858507201e-308,2.2250738585072014e-308,2.2250738585072014e-308,5e-324,0,5e-324,1e-323,1.5e-323,-0]

=== a literal halfway between two doubles rounds to the even one
[9007199254740993, 9007199254740995, 9007199254740993.000000000000000000001, 1.00000000000000011102230246251565404236316680908203125, 1.00000000000000011102230246251565404236316680908203125000000000000000001, 1.00000000000000011102230246251565404236316680908203124]
--- value
[9007199254740992,9007199254740996,9007199254740994,1,1.0000000000000002,1]

=== long literals
[10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000e-400, 0.00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001e401, 1.00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001, 1.00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001e-1, 10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000, 0.000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001]
--- value
[1,1,1,0.1,Infinity,1e-321]

=== hex, octal and binary literals past 2^53
[0x20000000000001, 0x20000000000003, 0x1fffffffffffff8, 0xFFFFFFFFFFFFFFFFFFFF, 0o1777777777777777777777, 0b1111111111111111111111111111111111111111111111111111111111111, 0x1_0000_0000_0000_0001, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff]
--- value
[9007199254740992,9007199254740996,144115188075855870,1.2089258196146292e+24,18446744073709552000,2305843009213694000,18446744073709552000,Infinity]

=== numeric separators in a long literal
[1_2.3_4e1_0, 1_000_000_000_000_000_000_001, 0.000_000_000_000_000_000_000_1]
--- value
[123400000000,1e+21,1e-22]

=== an infinite literal compared
[1e999 === Infinity, -1e999 < -1e308, [1, 2].filter(x => x < 1e999).length, [1e400, 1e-400]]
--- value
[true,true,2,[Infinity,0]]

# Number(x): ToNumber of a primitive, StringToNumber on a string. An
# object faults, as JavaScript would call its valueOf or toString.
=== Number of each primitive
[Number(), Number(undefined), Number(null), Number(true), Number(false), Number(5), Number(-0), Number(NaN), Number(-Infinity)]
--- value
[0,NaN,0,1,0,5,-0,NaN,-Infinity]

=== Number of strings
["", "  ", " 42 ", "-0", "+0", "-", "+", ".", "5.", ".5", "-.5", "+.5e1", "1e", "1e+", "1e-2", "1E2", "1_000", "1 2", "12abc", "abc", "010", "00.5", "0.0000001", "NaN", "1n", "١", "١"].map(Number)
--- value
[0,0,42,-0,0,NaN,NaN,NaN,5,0.5,-0.5,5,NaN,NaN,0.01,100,NaN,NaN,NaN,NaN,10,0.5,1e-7,NaN,NaN,NaN,NaN]

=== Number trims every white space and line terminator
[Number("\t\n\v\f\r         　﻿12　"), Number("᠎1"), Number("​1"), Number(" "), Number("﻿-0\n")]
--- value
[12,NaN,NaN,0,-0]

=== Number of hex, octal and binary strings
["0x1F", "0X1f", "0o17", "0O17", "0b101", "0B101", "-0x10", "+0x10", "0x", "0b2", "0o8", "0xg", " 0x10 ", "0x1fffffffffffff8", "0x" + "f".repeat(300), "00x1", "0x1.5", "0x1e3"].map(Number)
--- value
[31,31,15,15,5,5,NaN,NaN,NaN,NaN,NaN,NaN,16,144115188075855870,Infinity,NaN,NaN,483]

=== Number of Infinity spelled every way
["Infinity", "-Infinity", "+Infinity", " Infinity\n", "infinity", "Infinityx", "INFINITY", "Inf", "1e1000", "-1e1000", "1e-1000", "-1e-1000"].map(Number)
--- value
[Infinity,-Infinity,Infinity,Infinity,NaN,NaN,NaN,NaN,Infinity,-Infinity,0,-0]

=== Number reads a decimal to the nearest double
["0.1", "1e300", "123456789012345678901234567890", "9007199254740993", "9007199254740995", "2.4703282292062327e-324", "2.4703282292062328e-324", "1.7976931348623158e308", "1.7976931348623159e308", "0.30000000000000004", "1.00000000000000011102230246251565404236316680908203125", "1.000000000000000111022302462515654042363166809082031250001"].map(Number)
--- value
[0.1,1e+300,1.2345678901234568e+29,9007199254740992,9007199254740996,0,5e-324,1.7976931348623157e+308,Infinity,0.30000000000000004,1,1.0000000000000002]

=== Number reads long strings exactly
[Number("1".repeat(308)), Number("1".repeat(309)), Number("0." + "0".repeat(400) + "1e401"), Number("1" + "0".repeat(1000) + "e-1000"), Number("9".repeat(800) + "e-800"), Number("1." + "0".repeat(2000) + "1")]
--- value
[1.1111111111111111e+307,1.1111111111111112e+308,1,1,1,1]

=== Number as a callback
[["1", "2.5", " 3 ", ""].map(Number), [true, null, undefined].map(Number)]
--- value
[[1,2.5,3,0],[1,0,NaN]]

=== Number of an array faults
Number([])
--- fault converts only primitives

=== Number of an object faults
Number({})
--- fault converts only primitives

=== Number of a set faults
Number(new Set())
--- fault converts only primitives

=== Number of a function faults
Number(x => x)
--- fault converts only primitives

=== Number is a function, not a namespace, unless a member follows
[typeof Number, typeof Number.isNaN, Number.MAX_SAFE_INTEGER]
--- value
["function","function",9007199254740991]

# parseFloat: the longest prefix that is a decimal literal, after leading
# white space. A primitive is first turned into a string, as String does.
=== parseFloat of strings
["3.14abc", "  -0", "-0", ".5", "-.5e-3x", "5.", ".", "-", "+", "e5", "1e", "1e+", "1e5x", "1.5.3", "1_0", "  7", " -8", "", "  ", "abc", "0x10", "0b1", "007", "1e1000", "-1e1000", "1e-1000", "-1e-1000"].map(s => parseFloat(s))
--- value
[3.14,-0,-0,0.5,-0.0005,5,NaN,NaN,NaN,NaN,1,1,100000,1.5,1,7,-8,NaN,NaN,NaN,0,0,7,Infinity,-Infinity,0,-0]

=== parseFloat of Infinity
["Infinity", "Infinityx", "-Infinity", "+Infinityabc", "Inf", "infinity", " \n-Infinity1"].map(s => parseFloat(s))
--- value
[Infinity,Infinity,-Infinity,Infinity,NaN,NaN,-Infinity]

=== parseFloat reads a decimal to the nearest double
["0.1", "123456789012345678901234567890", "1e300", "2.4703282292062327e-324", "2.4703282292062328e-324x", "9007199254740993abc", "1.7976931348623158e308", "0." + "0".repeat(400) + "1e401", "1".repeat(400) + "e-390"].map(s => parseFloat(s))
--- value
[0.1,1.2345678901234568e+29,1e+300,0,5e-324,9007199254740992,1.7976931348623157e+308,1,1111111111.1111112]

=== parseFloat of primitives that are not strings
[parseFloat(1.5), parseFloat(-0), parseFloat(1e21), parseFloat(1e-7), parseFloat(-1e-7), parseFloat(true), parseFloat(null), parseFloat(undefined), parseFloat(), parseFloat(Infinity), parseFloat(-Infinity), parseFloat(NaN), Number.parseFloat("2.5e1")]
--- value
[1.5,0,1e+21,1e-7,-1e-7,NaN,NaN,NaN,NaN,Infinity,-Infinity,NaN,25]

=== parseFloat of an array faults
parseFloat([1])
--- fault primitives into strings

=== parseFloat of an object faults
parseFloat({})
--- fault primitives into strings

# parseInt: white space, a sign, 0x for radix 16 or none, then the digits
# of the radix. Radix 10 and the powers of two are exact; the others are
# V8's, as the specification lets an implementation approximate them.
=== parseInt of strings
["42", "  -42abc", "+0", "-0", "-00", "0x1F", "0X1f", "-0x1f", "+0x1f", "0x", "-0x", "0xg", "0b11", "0o17", "017", "08", "", "  ", "-", "abc", "12.9", "1e3", "Infinity", " ﻿ 9", "1_000", "00x1"].map(s => parseInt(s))
--- value
[42,-42,0,-0,-0,31,31,-31,31,NaN,NaN,NaN,0,0,17,8,NaN,NaN,NaN,NaN,12,1,NaN,9,1,0]

=== parseInt with a radix
[parseInt("ff", 16), parseInt("0xff", 16), parseInt("0xff", 15), parseInt("0xff", 0), parseInt("0xff", 10), parseInt("z", 36), parseInt("Z", 36), parseInt("10", 37), parseInt("10", 1), parseInt("10", -1), parseInt("10", 2), parseInt("12", 2), parseInt("2", 2), parseInt("10", 2.9), parseInt("10", 4294967312), parseInt("10", -4294967280), parseInt("10", NaN), parseInt("10", Infinity), parseInt("10", undefined), parseInt("10", null), parseInt("10", true), parseInt("11", false), parseInt("-0", 7), parseInt("0x", 16), parseInt("0x0", 16), parseInt("-0x", 16), parseInt("0", 1), parseInt("10", -0)]
--- value
[255,255,0,255,0,35,35,NaN,NaN,NaN,2,1,NaN,2,16,16,10,10,10,10,NaN,11,-0,NaN,0,NaN,NaN,10]

=== parseInt reads long digits as Node does
[parseInt("123456789012345678901234567890"), parseInt("9007199254740993"), parseInt("9007199254740995"), parseInt("1".repeat(309)), parseInt("1".repeat(310)), parseInt("1".repeat(400)), parseInt("f".repeat(20), 16), parseInt("1fffffffffffff8", 16), parseInt("1fffffffffffff9", 16), parseInt("1".repeat(64), 2), parseInt("1".repeat(54), 2), parseInt("v".repeat(30), 32), parseInt("7".repeat(40), 8), parseInt("3".repeat(40), 4), parseInt("f".repeat(300), 16)]
--- value
[1.2345678901234568e+29,9007199254740992,9007199254740996,1.1111111111111112e+308,Infinity,Infinity,1.2089258196146292e+24,144115188075855870,144115188075855870,18446744073709552000,18014398509481984,1.42724769270596e+45,1.329227995784916e+36,1.2089258196146292e+24,Infinity]

=== parseInt in a radix that is not a power of two
[parseInt("z".repeat(20), 36), parseInt("2".repeat(40), 3), parseInt("zzzzzzzzzzzzz", 36), parseInt("123456789abcdefghij", 20), parseInt("6".repeat(30), 7), parseInt("0".repeat(30) + "1234567890123456789", 36), parseInt("1234567890123456789", 36), parseInt("4".repeat(100), 5), parseInt("z".repeat(250), 36), parseInt("9".repeat(25), 11)]
--- value
[1.3367494538843734e+31,12157665459056929000,170581728179578200000,2.9046426592797785e+23,2.2539340290692256e+25,1.0912240439871393e+28,1.0912240439871393e+28,7.888609052210118e+69,Infinity,9.751235349049536e+25]

=== parseInt of primitives that are not strings
[parseInt(15.99), parseInt(-15.99), parseInt(0.0000005), parseInt(-0.5), parseInt(-0), parseInt(1e21), parseInt(null), parseInt(null, 36), parseInt(undefined, 36), parseInt(true, 36), parseInt(), parseInt(NaN), parseInt(Infinity, 36), parseInt(123, 5), Number.parseInt("08")]
--- value
[15,-15,5,-0,0,1,NaN,1112745,86464843759093,1389110,NaN,NaN,1461559270678,38,8]

=== parseInt as a callback gets the index as its radix
["1", "2", "3", "10", "10"].map(parseInt)
--- value
[1,NaN,NaN,3,4]

=== parseInt of an array faults
parseInt([10])
--- fault primitives into strings

=== parseInt of an object faults
parseInt({}, 10)
--- fault primitives into strings

=== parseInt with a string radix faults
parseInt("10", "16")
--- fault converts only numbers

# The global isNaN and isFinite convert their argument as the Math
# functions do: a string faults, as JavaScript would coerce it
# (Number.isNaN(Number(s)) says the same without coercing).
=== isNaN and isFinite
[isNaN(NaN), isNaN(1), isNaN(-0), isNaN(undefined), isNaN(null), isNaN(true), isNaN(), isFinite(1), isFinite(Infinity), isFinite(-Infinity), isFinite(NaN), isFinite(null), isFinite(undefined), isFinite(false), isFinite(), isFinite(Number.MAX_VALUE)]
--- value
[true,false,false,true,false,false,true,true,false,false,false,true,false,true,false,true]

=== isNaN of a string faults
isNaN("abc")
--- fault converts only numbers

=== isFinite of a string faults
isFinite("1")
--- fault converts only numbers

=== isNaN of an object faults
isNaN({})
--- fault converts only numbers
