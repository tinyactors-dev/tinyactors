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

=== a decimal of up to 19 digits halfway between two doubles rounds to the even one
[4503599627370496.5, 4503599627370497.5, Number("4503599627370496.5"), Number("4503599627370497.5"), 2251799813685248.25, 2251799813685248.75, 1125899906842624.125, 1125899906842624.375, 1125899906842625.625, parseFloat("9007199254740993.0"), 0.30000000000000004, 859.79412078081657]
--- value
[4503599627370496,4503599627370498,4503599627370496,4503599627370498,2251799813685248,2251799813685249,1125899906842624,1125899906842624.5,1125899906842625.5,9007199254740992,0.30000000000000004,859.7941207808166]

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
[parseInt("z".repeat(20), 36), parseInt("2".repeat(40), 3), parseInt("zzzzzzzzzzzzz", 36), parseInt("123456789abcdefghij", 20), parseInt("6".repeat(30), 7), parseInt("0".repeat(30) + "9876543210zyxwvuts", 36), parseInt("9876543210zyxwvuts", 36), parseInt("4".repeat(100), 5), parseInt("z".repeat(250), 36), parseInt("a".repeat(25), 11)]
--- value
[1.3367494538843734e+31,12157665459056929000,170581728179578200000,2.9046426592797785e+23,2.2539340290692256e+25,2.643860723857985e+27,2.643860723857985e+27,7.888609052210118e+69,Infinity,1.0834705943388373e+26]

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

# The Number and Math constants are names the compiler turns into number
# constants.
=== the Number constants
[Number.EPSILON, Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER, Number.MAX_VALUE, Number.MIN_VALUE, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NaN]
--- value
[2.220446049250313e-16,9007199254740991,-9007199254740991,1.7976931348623157e+308,5e-324,Infinity,-Infinity,NaN]

=== the Math constants
[Math.PI, Math.E, Math.LN2, Math.LN10, Math.LOG2E, Math.LOG10E, Math.SQRT2, Math.SQRT1_2]
--- value
[3.141592653589793,2.718281828459045,0.6931471805599453,2.302585092994046,1.4426950408889634,0.4342944819032518,1.4142135623730951,0.7071067811865476]

=== the constants in arithmetic
[1 + Number.EPSILON > 1, 1 + Number.EPSILON / 2 === 1, Number.MAX_SAFE_INTEGER + 2, Number.MIN_VALUE / 2, Number.MAX_VALUE * 2, -Number.MIN_VALUE, Number.isNaN(Number.NaN), Math.PI * 2, Math.SQRT2 * Math.SQRT1_2, Math.LN2 * Math.LOG2E, Math.LN10 * Math.LOG10E]
--- value
[true,true,9007199254740992,0,Infinity,-5e-324,true,6.283185307179586,1.0000000000000002,1,1]

=== the constants as callback results and keys
[[1, 2].map(x => Math.PI * x), {[Number.MAX_SAFE_INTEGER]: 1}, [Number.NEGATIVE_INFINITY, Number.POSITIVE_INFINITY].map(x => -x)]
--- value
[[3.141592653589793,6.283185307179586],{"9007199254740991":1},[Infinity,-Infinity]]

=== an unknown Number member is refused
Number.MAX_INTEGER
--- error unsupported does not have this built-in

=== an unknown Math member is refused
Math.TAU
--- error unsupported does not have this built-in

=== a Number member staticjs leaves out is refused
Number.prototype
--- error unsupported does not have this built-in

# Math functions IEEE 754 makes exact: the same bits on every target.
=== Math.sqrt
[Math.sqrt(4), Math.sqrt(2), Math.sqrt(0), Math.sqrt(-0), Math.sqrt(-1), Math.sqrt(Infinity), Math.sqrt(-Infinity), Math.sqrt(NaN), Math.sqrt(5e-324), Math.sqrt(Number.MAX_VALUE), Math.sqrt(0.01), Math.sqrt(1e-310), Math.sqrt(), Math.sqrt(null), Math.sqrt(true), Math.sqrt(undefined)]
--- value
[2,1.4142135623730951,0,-0,NaN,Infinity,NaN,NaN,2.2227587494850775e-162,1.3407807929942596e+154,0.1,9.999999999999986e-156,NaN,0,1,NaN]

=== Math.fround
[Math.fround(1.1), Math.fround(0.1), Math.fround(-0), Math.fround(NaN), Math.fround(Infinity), Math.fround(3.4028234663852886e38), Math.fround(3.4028235677973366e38), Math.fround(3.4028235677973362e38), Math.fround(1e39), Math.fround(-1e39), Math.fround(1.401298464324817e-45), Math.fround(7.006492321624085e-46), Math.fround(7.006492321624087e-46), Math.fround(1e-46), Math.fround(-1e-46), Math.fround(16777217), Math.fround(16777219), Math.fround(5e-324), Math.fround(), Math.fround(true)]
--- value
[1.100000023841858,0.10000000149011612,-0,NaN,Infinity,3.4028234663852886e+38,Infinity,3.4028234663852886e+38,Infinity,-Infinity,1.401298464324817e-45,0,1.401298464324817e-45,0,-0,16777216,16777220,0,NaN,1]

=== Math.clz32
[Math.clz32(0), Math.clz32(1), Math.clz32(-1), Math.clz32(0.5), Math.clz32(-0), Math.clz32(NaN), Math.clz32(Infinity), Math.clz32(-Infinity), Math.clz32(2147483648), Math.clz32(4294967295), Math.clz32(4294967296), Math.clz32(4294967297), Math.clz32(-2147483649), Math.clz32(1e20), Math.clz32(3.9), Math.clz32(), Math.clz32(true), Math.clz32(null), Math.clz32(9007199254740991), Math.clz32(1.7976931348623157e308), Math.clz32(-4294967295.5)]
--- value
[32,31,0,32,32,32,32,32,0,0,32,31,1,1,30,32,31,32,0,32,31]

=== Math.imul
[Math.imul(2, 3), Math.imul(-5, 12), Math.imul(0xffffffff, 5), Math.imul(0xfffffffe, 5), Math.imul(2147483647, 2), Math.imul(65536, 65536), Math.imul(65537, 65537), Math.imul(1.9, 2.9), Math.imul(-1.9, 3), Math.imul(NaN, 3), Math.imul(Infinity, 3), Math.imul(3), Math.imul(), Math.imul(true, 7), Math.imul(null, 7), Math.imul(-0, 5), Math.imul(-1, 0), Math.imul(4294967296 + 3, 5), Math.imul(1e20, 3), Math.imul(9007199254740991, 9007199254740991)]
--- value
[6,-60,-5,-10,-2,0,131073,2,-3,0,0,0,0,7,0,0,0,15,691011584,1]

=== Math.hypot
[Math.hypot(), Math.hypot(3, 4), Math.hypot(-3, -4), Math.hypot(5), Math.hypot(-5), Math.hypot(-0), Math.hypot(0, -0), Math.hypot(1, 1, 1), Math.hypot(3, 4, 12), Math.hypot(1e200, 1e200), Math.hypot(1e-200, 1e-200), Math.hypot(5e-324, 5e-324), Math.hypot(Number.MAX_VALUE, Number.MAX_VALUE), Math.hypot(0.1, 0.2, 0.3, 0.4, 0.5), Math.hypot(1, 2, 3, 4, 5, 6, 7, 8, 9, 10), Math.hypot(null, 3, 4), Math.hypot(true, true), Math.hypot(...[6, 8])]
--- value
[0,5,5,5,5,0,0,1.7320508075688772,13,1.414213562373095e+200,1.414213562373095e-200,5e-324,Infinity,0.7416198487095663,19.621416870348586,5,1.4142135623730951,10]

=== Math.hypot with an Infinity and a NaN
[Math.hypot(NaN), Math.hypot(1, NaN), Math.hypot(NaN, Infinity), Math.hypot(Infinity, NaN), Math.hypot(-Infinity, NaN, 1), Math.hypot(undefined), Math.hypot(undefined, -Infinity), Math.hypot(1, undefined), Math.hypot(0, NaN)]
--- value
[NaN,NaN,Infinity,Infinity,Infinity,NaN,Infinity,NaN,NaN]

=== Math functions as callbacks
[[1, 4, 9, -1].map(Math.sqrt), [1.1, 2.2].map(Math.fround), [1, 2, 3].map(Math.clz32), [[3, 4], [5, 12]].map(([a, b]) => Math.hypot(a, b)), [2, 3].map(Math.imul)]
--- value
[[1,2,3,NaN],[1.100000023841858,2.200000047683716],[31,30,30],[5,13],[0,3]]

=== Math.sqrt of a string faults
Math.sqrt("4")
--- fault converts only numbers

=== Math.hypot of a string faults
Math.hypot(3, "4")
--- fault converts only numbers

=== Math.hypot converts every argument before it looks at any
Math.hypot(Infinity, [1])
--- fault converts only numbers

=== Math.fround of an array faults
Math.fround([1])
--- fault converts only numbers

=== Math.clz32 of an object faults
Math.clz32({})
--- fault converts only numbers

=== Math.imul of a string faults
Math.imul(2, "3")
--- fault converts only numbers

# x ** y and Math.pow(x, y): V8's fdlibm pow (src/math.c), bit for bit
# (values generated by Node 22). JavaScript's 1 ** Infinity is NaN, not C's 1.

=== ** of integers
[2 ** 10, 2 ** -1, 3 ** 3, (-2) ** 3, (-2) ** 4, (-2) ** -3, 10 ** 21, 10 ** 22, 10 ** 23, 10 ** -5, 10 ** -7, 10 ** 308, 10 ** -323, 2 ** 1023, 2 ** -1022, 2 ** -1074, 7 ** 19, 3 ** 40, 5 ** -3, 2 ** 53 + 1, 0 ** 0, (-7) ** 1, 12 ** 12, (-3) ** 33]
--- value
[1024,0.5,27,-8,16,-0.125,1e+21,1e+22,1e+23,0.000009999999999999999,1e-7,1e+308,1e-323,8.98846567431158e+307,2.2250738585072014e-308,5e-324,11398895185373144,12157665459056929000,0.008,9007199254740992,1,-7,8916100448256,-5559060566555523]

=== ** of integers that overflow or underflow
[10 ** 309, 2 ** 1024, 10 ** -324, 2 ** -1075, (-2) ** 1025, (-2) ** 1024, (-2) ** -1075, (-10) ** 400, (-10) ** 401].map(String)
--- value
["Infinity","Infinity","0","0","-Infinity","Infinity","0","Infinity","-Infinity"]

=== ** with halves and fractions
[4 ** 0.5, 2 ** 0.5, 2 ** -0.5, 8 ** (1 / 3), 27 ** (1 / 3), 10 ** 0.1, 1.1 ** 1.5, 0.5 ** 2.5, Math.E ** Math.PI, 1e300 ** 0.5, 5e-324 ** 0.5, 2 ** 1023.5, 2 ** -1074.5, 0.1 ** 0.1, 123456.789 ** 2.5, (-8) ** 3.0]
--- value
[2,1.4142135623730951,0.7071067811865475,2,3,1.2589254117941673,1.153689732987167,0.17677669529663687,23.140692632779263,1e+150,2.2227587494850775e-162,1.2711610061536462e+308,5e-324,0.7943282347242815,5355344863124.801,-512]

=== ** of a negative base and a non-integer exponent is NaN
[(-8) ** (1 / 3), (-4) ** 0.5, (-2) ** 1.5, (-1) ** 0.5, (-1e300) ** 0.5, (-5e-324) ** 2.5].map(String)
--- value
["NaN","NaN","NaN","NaN","NaN","NaN"]

=== ** with NaN, Infinity and 1
[2 ** NaN, NaN ** 0, NaN ** -0, NaN ** 1, 1 ** NaN, 1 ** Infinity, 1 ** -Infinity, (-1) ** Infinity, (-1) ** -Infinity, 0.5 ** Infinity, 0.5 ** -Infinity, 2 ** Infinity, 2 ** -Infinity, (-2) ** Infinity, Infinity ** 0, Infinity ** 0.5, Infinity ** -1, (-Infinity) ** 3, (-Infinity) ** 2, (-Infinity) ** 0.5, (-Infinity) ** -3, (-Infinity) ** -2, 1 ** 1e308, (-1) ** 9007199254740991].map(String)
--- value
["NaN","1","1","NaN","NaN","NaN","NaN","NaN","NaN","0","Infinity","Infinity","0","Infinity","1","Infinity","0","-Infinity","Infinity","Infinity","0","0","1","-1"]

=== ** with zeros: signs
[0 ** 3, 0 ** -3, 0 ** -0.5, (-0) ** 0.5, (-0) ** 2, (-0) ** -2, (-0) ** -3, 5 ** -0, (-0) ** -0].map(String).concat([1 / ((-0) ** 3) === -Infinity, 1 / ((-0) ** 0.5) === Infinity, 1 / ((-Infinity) ** -3) === -Infinity, 1 / ((-2) ** -1075) === -Infinity, 1 / ((-0.5) ** 1075) === -Infinity, 1 / (2 ** -Infinity) === Infinity])
--- value
["0","Infinity","Infinity","0","0","Infinity","-Infinity","1","1",true,true,true,true,true,true]

=== ** next to overflow and underflow, and subnormal
[1.0000001 ** 1e10, (1 + 2 ** -52) ** (2 ** 52), 0.9999999999999999 ** 1e17, 0.9999999999999999 ** 1e16, 1.0000000000000002 ** 4294967296, (-1.0000000000000002) ** 9007199254740991, 1.0000000000000002 ** 18446744073709552000, 0.5 ** 1074, 0.5 ** 1075, 0.5 ** 1073.5, 1.5 ** 2000, (-1.5) ** 2001, 1e-300 ** 2, 1.9999999999999998 ** 1024, 1.9999999999999998 ** 1023, 2.0000000000000004 ** -1074, 10 ** -310, 3 ** -678].map(String)
--- value
["Infinity","2.718281828459045","0.000015078657172714957","0.3294854695069476","1.0000009536747712","-7.389056098930647","Infinity","5e-324","0","5e-324","Infinity","-Infinity","0","1.7976931348621115e+308","8.988465674310559e+307","5e-324","1e-310","5e-324"]

=== Math.pow is **
[Math.pow(2, 10), Math.pow(2, 0.5), Math.pow(-2, 3), Math.pow(10, -2), Math.pow(1.1, 1.5), Math.pow(2, 1023.5), Math.pow(0.5, 1074), Math.pow(4, 0.5) === 4 ** 0.5, [1, 2, 3].map(x => Math.pow(x, 3)), [2, 3].map(Math.pow)]
--- value
[1024,1.4142135623730951,-8,0.01,1.153689732987167,1.2711610061536462e+308,5e-324,true,[1,8,27],[1,3]]

=== Math.pow converts primitives as JavaScript does
[Math.pow(), Math.pow(2), Math.pow(null, 0), Math.pow(true, 10), Math.pow(undefined, 0), Math.pow(2, null), Math.pow(2, true), Math.pow(2, undefined), Math.pow(2, 10, 99), Math.pow(1, Infinity), Math.pow(-1, -Infinity), Math.pow(NaN, 0)].map(String).concat([1 / Math.pow(-0, 3) === -Infinity, 1 / Math.pow(-0, 0.5) === Infinity])
--- value
["NaN","NaN","1","1","1","1","2","NaN","1024","NaN","NaN","1",true,true]

=== ** is right-associative and binds tighter than *
[2 ** 3 ** 2, (2 ** 3) ** 2, 2 * 3 ** 2, -(2 ** 2), (-2) ** 2, 2 ** -(2 ** 2), 4 ** 0.5 ** 2]
--- value
[512,64,18,-4,4,0.0625,1.414213562373095]

=== ** in a function, a callback and a variable
(() => { const square = x => x ** 2; const p = 1.5; const qs = [1, 2, 3, 4].map(n => p ** n); return [square(3), square(-0.5), qs, [0.5, 2].map(b => b ** -1074).map(String)]; })()
--- value
[9,0.25,[1.5,2.25,3.375,5.0625],["Infinity","5e-324"]]

=== ** of a string faults, where JavaScript converts it
"2" ** 2
--- fault arithmetic in staticjs is on numbers

=== ** with a string exponent faults
2 ** "2"
--- fault arithmetic in staticjs is on numbers

=== ** of null faults, where JavaScript converts it
null ** 2
--- fault arithmetic in staticjs is on numbers

=== ** of an array faults
[2] ** 2
--- fault arithmetic in staticjs is on numbers

=== Math.pow of a string faults
Math.pow("2", 2)
--- fault converts only numbers

=== Math.pow converts its base before its exponent
Math.pow([2], "2")
--- fault converts only numbers

=== Math.pow of a BigInt faults
Math.pow(2n, 2)
--- fault BigInt

# Spot-check cases for number conversion, constants and Math (values generated by Node 22).

# Math.exp, expm1, log, log1p, log2 and log10: V8's fdlibm (src/math.c),
# bit for bit (values generated by Node 22). Some inputs are ones where
# glibc's last bit differs from V8's.

=== Math.exp
[Math.exp(0), Math.exp(1), Math.exp(-1), Math.exp(2), Math.exp(0.5), Math.exp(10), Math.exp(-10), Math.exp(100), Math.exp(709), Math.exp(709.782712893384), Math.exp(-708), Math.exp(-709), Math.exp(-745), Math.exp(1e-10), Math.exp(-1e-300), Math.exp(0.34657359027997264), Math.exp(1.0397207708399179)]
--- value
[1,2.718281828459045,0.36787944117144233,7.38905609893065,1.6487212707001282,22026.465794806718,0.00004539992976248485,2.6881171418161356e+43,8.218407461554972e+307,1.7976931348622732e+308,3.307553003638408e-308,1.216780750623423e-308,5e-324,1.0000000001,1,1.414213562373095,2.82842712474619]

=== Math.exp where glibc's last bit differs
[Math.exp(125.10776284998587), Math.exp(-17.051337266330457), Math.exp(35.178902681469836), Math.exp(213.96424573960428), Math.exp(202.85560714971467), Math.exp(0.36982602375742113)]
--- value
[2.1558128139882823e+54,3.9327678975983314e-8,1896720394473149,8.384771837837137e+92,1.2562347660679553e+88,1.447482765143366]

=== Math.exp at its specials and limits
[Math.exp(NaN), Math.exp(Infinity), Math.exp(-Infinity), Math.exp(709.7827128933841), Math.exp(710), Math.exp(-745.1332191019412), Math.exp(-746), Math.exp(1e300), Math.exp(-1e300), Math.exp(5e-324), Math.exp(-0)].map(String)
--- value
["NaN","Infinity","0","Infinity","Infinity","0","0","Infinity","0","1","1"]

=== Math.expm1
[Math.expm1(0), Math.expm1(1), Math.expm1(-1), Math.expm1(1e-10), Math.expm1(-1e-10), Math.expm1(1e-17), Math.expm1(0.5), Math.expm1(-0.5), Math.expm1(10), Math.expm1(-10), Math.expm1(38.8), Math.expm1(-38.9), Math.expm1(-40), Math.expm1(100), Math.expm1(709), Math.expm1(0.3465733528137212), Math.expm1(-0.34657337914781683), Math.expm1(-1.0397207333294944), Math.expm1(514)]
--- value
[0,1.718281828459045,-0.6321205588285577,1.00000000005e-10,-9.999999999500001e-11,1e-17,0.6487212707001282,-0.3934693402873666,22025.465794806718,-0.9999546000702375,70896679940719430,-1,-1,2.6881171418161356e+43,8.218407461554972e+307,0.41421322654514153,-0.29289306952045757,-0.6464465961447886,1.687966014410163e+223]

=== Math.expm1 at its specials and limits
[Math.expm1(NaN), Math.expm1(Infinity), Math.expm1(-Infinity), Math.expm1(710), Math.expm1(-1e300), Math.expm1(5e-324)].map(String).concat([1 / Math.expm1(-0) === -Infinity, 1 / Math.expm1(-5e-324) === -Infinity, Math.expm1(-5e-324) === -5e-324])
--- value
["NaN","Infinity","-1","Infinity","-1","5e-324",true,true,true]

=== Math.log
[Math.log(1), Math.log(2), Math.log(Math.E), Math.log(10), Math.log(0.5), Math.log(1e-300), Math.log(1e300), Math.log(5e-324), Math.log(2.2250738585072014e-308), Math.log(1.7976931348623157e308), Math.log(1.0000000000000002), Math.log(0.9999999999999999), Math.log(1.4142135623730951), Math.log(0.7071067811865476), Math.log(850), Math.log(0.79289436340332), Math.log(1.585788655771302), Math.log(1.3799991607666025)]
--- value
[0,0.6931471805599453,1,2.302585092994046,-0.6931471805599453,-690.7755278982137,690.7755278982137,-744.4400719213812,-708.3964185322641,709.782712893384,2.2204460492503128e-16,-1.1102230246251565e-16,0.3465735902799727,-0.3465735902799726,6.745236349484362,-0.23206527756582818,0.46108185819915903,0.3220828910287853]

=== Math.log at its specials
[Math.log(0), Math.log(-0), Math.log(-1), Math.log(-5e-324), Math.log(NaN), Math.log(Infinity), Math.log(-Infinity)].map(String).concat([1 / Math.log(1) === Infinity])
--- value
["-Infinity","-Infinity","NaN","NaN","NaN","Infinity","NaN",true]

=== Math.log1p
[Math.log1p(0), Math.log1p(1), Math.log1p(-0.5), Math.log1p(1e-10), Math.log1p(-1e-10), Math.log1p(1e-17), Math.log1p(0.41421356237309503), Math.log1p(-0.2928932188134524), Math.log1p(-0.29289340972900396), Math.log1p(0.45792136197263167), Math.log1p(-0.999999999999), Math.log1p(1e15), Math.log1p(1e300), Math.log1p(9007199254740992), Math.log1p(5e-324)]
--- value
[0,0.6931471805599453,-0.6931471805599453,9.999999999500001e-11,-1.00000000005e-10,1e-17,0.34657359027997264,-0.3465735902799726,-0.3465738602753713,0.3770116965850006,-27.63104323789336,34.538776394910684,690.7755278982137,36.7368005696771,5e-324]

=== Math.log1p at its specials
[Math.log1p(-1), Math.log1p(-1.0000000000000002), Math.log1p(-2), Math.log1p(NaN), Math.log1p(Infinity), Math.log1p(-Infinity)].map(String).concat([1 / Math.log1p(-0) === -Infinity, Math.log1p(-5e-324) === -5e-324])
--- value
["-Infinity","NaN","NaN","NaN","Infinity","NaN",true,true]

=== Math.log2
[Math.log2(1), Math.log2(2), Math.log2(8), Math.log2(1024), Math.log2(0.5), Math.log2(3), Math.log2(10), Math.log2(5e-324), Math.log2(2.2250738585072014e-308), Math.log2(1.7976931348623157e308), Math.log2(1.4142135623730951), Math.log2(0.7071067811865476), Math.log2(1.0000000000000002), Math.log2(1.3799991607666007), Math.log2(0.7071068821875267), Math.log2(0.79289436340333)]
--- value
[0,1,3,10,-1,1.584962500721156,3.321928094887362,-1074,-1022,1024,0.5000000000000001,-0.4999999999999999,3.203426503814917e-16,0.464667389642407,-0.4999997939298487,-0.3347994251067229]

=== Math.log2 at its specials
[Math.log2(0), Math.log2(-0), Math.log2(-2), Math.log2(NaN), Math.log2(Infinity), Math.log2(-Infinity)].map(String).concat([1 / Math.log2(1) === Infinity])
--- value
["-Infinity","-Infinity","NaN","NaN","Infinity","NaN",true]

=== Math.log10
[Math.log10(1), Math.log10(10), Math.log10(100), Math.log10(1000), Math.log10(1e22), Math.log10(1e23), Math.log10(1e300), Math.log10(1e-300), Math.log10(0.1), Math.log10(2), Math.log10(5e-324), Math.log10(1.7976931348623157e308), Math.log10(0.79289436340332), Math.log10(820.5), Math.log10(0.7071068555205817), Math.log10(1.585788726806641)]
--- value
[0,1,2,3,22,23,300,-300,-1,0.3010299956639812,-323.3062153431158,308.25471555991675,-0.10078466948818568,2.9140785853891122,-0.15051495217713334,0.20024532617579585]

=== Math.log10 at its specials
[Math.log10(0), Math.log10(-0), Math.log10(-10), Math.log10(NaN), Math.log10(Infinity), Math.log10(-Infinity)].map(String).concat([1 / Math.log10(1) === Infinity])
--- value
["-Infinity","-Infinity","NaN","NaN","Infinity","NaN",true]

=== exp and log convert primitives as JavaScript does
[Math.exp(), Math.exp(null), Math.exp(true), Math.exp(undefined), Math.expm1(null), Math.expm1(true), Math.log(), Math.log(null), Math.log(true), Math.log(false), Math.log1p(true), Math.log1p(undefined), Math.log2(true), Math.log2(null), Math.log10(true), Math.log10(undefined), Math.exp(1, 99)].map(String)
--- value
["NaN","1","2.718281828459045","NaN","0","1.718281828459045","NaN","-Infinity","0","-Infinity","0.6931471805599453","NaN","0","-Infinity","0","NaN","2.718281828459045"]

=== exp and log as values
[[0, 1, 2].map(Math.exp), [1, 2, 4].map(Math.log2), [1, 10, 100].map(Math.log10), [0, 1].map(Math.expm1), [0, 1].map(Math.log1p), [1, 2].map(Math.log), [0.5, 2].map(x => Math.log(Math.exp(x)))]
--- value
[[1,2.718281828459045,7.38905609893065],[0,1,2],[0,1,2],[0,1.718281828459045],[0,0.6931471805599453],[0,0.6931471805599453],[0.5,2]]

=== Math.exp of a string faults
Math.exp("1")
--- fault converts only numbers

=== Math.log of an array faults
Math.log([10])
--- fault converts only numbers

=== Math.log10 of an object faults
Math.log10({})
--- fault converts only numbers

=== Math.expm1 of a BigInt faults
Math.expm1(1n)
--- fault BigInt

# Math.sin, cos, tan, asin, acos, atan and atan2: V8's fdlibm (src/math.c),
# bit for bit (values generated by Node 22); sin and cos are fdlibm's too,
# not glibc's. Some inputs are ones where glibc's last bit differs from V8's;
# 5.319372648326541e+255 is 6381956970095103 * 2^797, the double closest to
# a multiple of pi/2.

=== Math.sin
[Math.sin(0), Math.sin(1), Math.sin(-1), Math.sin(0.5), Math.sin(Math.PI), Math.sin(Math.PI / 2), Math.sin(Math.PI / 4), Math.sin(3 * Math.PI / 4), Math.sin(2 * Math.PI), Math.sin(10), Math.sin(100), Math.sin(1e6), Math.sin(823549.6), Math.sin(1e10), Math.sin(1e22), Math.sin(1e300), Math.sin(1.7976931348623157e308), Math.sin(5.319372648326541e+255), Math.sin(-5.319372648326541e+255), Math.sin(1e-10), Math.sin(7.450580596923828e-9)]
--- value
[0,0.8414709848078965,-0.8414709848078965,0.479425538604203,1.2246467991473532e-16,1,0.7071067811865475,0.7071067811865476,-2.4492935982947064e-16,-0.5440211108893698,-0.5063656411097588,-0.34999350217129294,-0.06453775732851301,-0.4875060250875107,-0.8522008497671888,-0.8178819121159085,0.004961954789184062,1,-1,1e-10,7.450580596923828e-9]

=== Math.sin where glibc's last bit differs
[Math.sin(18), Math.sin(24), Math.sin(2.5), Math.sin(4.4), Math.sin(1.39), Math.sin(0.51), Math.sin(62)]
--- value
[-0.7509872467716762,-0.9055783620066238,0.5984721441039564,-0.9516020738895161,0.9837008148112765,0.48817724688290753,-0.7391806966492229]

=== Math.cos
[Math.cos(0), Math.cos(1), Math.cos(-1), Math.cos(0.5), Math.cos(Math.PI), Math.cos(Math.PI / 2), Math.cos(Math.PI / 4), Math.cos(3 * Math.PI / 4), Math.cos(2 * Math.PI), Math.cos(0.3), Math.cos(0.78125), Math.cos(10), Math.cos(1e6), Math.cos(1e22), Math.cos(1e300), Math.cos(1.7976931348623157e308), Math.cos(5.319372648326541e+255), Math.cos(1e-10)]
--- value
[1,0.5403023058681398,0.5403023058681398,0.8775825618903728,-1,6.123233995736766e-17,0.7071067811865476,-0.7071067811865475,1,0.955336489125606,0.7100338835660797,-0.8390715290764524,0.9367521275331447,0.523214785395139,-0.5753861119575491,-0.9999876894265599,-4.687165924254628e-19,1]

=== Math.cos where glibc's last bit differs
[Math.cos(0.1), Math.cos(6), Math.cos(2.28), Math.cos(29), Math.cos(0.44), Math.cos(0.77), Math.cos(56)]
--- value
[0.9950041652780257,0.9601702866503661,-0.6512296605275456,-0.7480575296890004,0.9047516632199635,0.7179106696109434,0.853220107722584]

=== Math.tan
[Math.tan(0), Math.tan(1), Math.tan(-1), Math.tan(0.5), Math.tan(Math.PI), Math.tan(Math.PI / 2), Math.tan(-Math.PI / 2), Math.tan(Math.PI / 4), Math.tan(0.6744), Math.tan(0.6745), Math.tan(10), Math.tan(1e6), Math.tan(1e21), Math.tan(1e300), Math.tan(1.7976931348623157e308), Math.tan(5.319372648326541e+255), Math.tan(1e-10), Math.tan(3e-9)]
--- value
[0,1.5574077246549023,-1.5574077246549023,0.5463024898437905,-1.2246467991473532e-16,16331239353195370,-16331239353195370,0.9999999999999999,0.799441007199113,0.7996049308967912,0.6483608274590866,-0.373624453987599,-0.8955232925542654,1.4214488238747245,-0.004962015874444895,-2133485385753704000,1e-10,3e-9]

=== Math.tan where glibc's last bit differs
[Math.tan(4), Math.tan(7), Math.tan(15), Math.tan(2.18), Math.tan(0.36), Math.tan(0.53), Math.tan(37)]
--- value
[1.1578212823495777,0.8714479827243187,-0.8559934009085188,-1.4332105287001913,0.3764028516420269,0.585917012598471,-0.8407712554027598]

=== sin, cos and tan at their specials
[Math.sin(NaN), Math.sin(Infinity), Math.sin(-Infinity), Math.cos(NaN), Math.cos(Infinity), Math.cos(-Infinity), Math.tan(NaN), Math.tan(Infinity), Math.tan(-Infinity), Math.cos(0), Math.cos(-0), Math.sin(5e-324), Math.tan(-5e-324), Math.cos(5e-324)].map(String).concat([1 / Math.sin(-0) === -Infinity, 1 / Math.tan(-0) === -Infinity, 1 / Math.sin(0) === Infinity, Math.sin(-5e-324) === -5e-324])
--- value
["NaN","NaN","NaN","NaN","NaN","NaN","NaN","NaN","NaN","1","1","5e-324","-5e-324","1",true,true,true,true]

=== Math.asin
[Math.asin(0), Math.asin(1), Math.asin(-1), Math.asin(0.5), Math.asin(-0.5), Math.asin(0.25), Math.asin(0.9), Math.asin(0.975), Math.asin(0.9999999999999999), Math.asin(-0.9999999999999999), Math.asin(1e-10), Math.asin(7.450580596923828e-9), Math.asin(0.49999999999999994)]
--- value
[0,1.5707963267948966,-1.5707963267948966,0.5235987755982989,-0.5235987755982989,0.25268025514207865,1.1197695149986342,1.3467210414930773,1.5707963118937354,-1.5707963118937354,1e-10,7.450580596923828e-9,0.5235987755982988]

=== Math.asin where glibc's last bit differs
[Math.asin(-0.995), Math.asin(-0.9938), Math.asin(-0.9911), Math.asin(-0.987), Math.asin(-0.9832)]
--- value
[-1.4707546131833564,-1.4593834255654796,-1.4372805368033108,-1.409375976646468,-1.3872356998400246]

=== Math.acos
[Math.acos(0), Math.acos(1), Math.acos(-1), Math.acos(0.5), Math.acos(-0.5), Math.acos(0.25), Math.acos(-0.25), Math.acos(0.9), Math.acos(-0.9), Math.acos(0.9999999999999999), Math.acos(-0.9999999999999999), Math.acos(1e-10), Math.acos(1e-20), Math.acos(-0)]
--- value
[1.5707963267948966,0,3.141592653589793,1.0471975511965979,2.0943951023931957,1.318116071652818,1.8234765819369754,0.45102681179626236,2.6905658417935308,1.4901161193847656e-8,3.141592638688632,1.5707963266948965,1.5707963267948966,1.5707963267948966]

=== Math.acos where glibc's last bit differs
[Math.acos(-0.9927), Math.acos(-0.9919), Math.acos(-0.9918), Math.acos(-0.9905), Math.acos(-0.9793)]
--- value
[3.0206885676622646,3.014227362546488,3.0134424976266514,3.0036428084360223,2.937770123661588]

=== asin and acos at their specials
[Math.asin(NaN), Math.asin(1.0000000000000002), Math.asin(-2), Math.asin(Infinity), Math.acos(NaN), Math.acos(1.0000000000000002), Math.acos(-2), Math.acos(-Infinity), Math.asin(5e-324)].map(String).concat([1 / Math.asin(-0) === -Infinity, 1 / Math.acos(1) === Infinity])
--- value
["NaN","NaN","NaN","NaN","NaN","NaN","NaN","NaN","5e-324",true,true]

=== Math.atan
[Math.atan(0), Math.atan(1), Math.atan(-1), Math.atan(0.5), Math.atan(0.4375), Math.atan(0.6875), Math.atan(1.1875), Math.atan(2.4375), Math.atan(10), Math.atan(-10), Math.atan(1e10), Math.atan(1e20), Math.atan(73786976294838210000), Math.atan(1e300), Math.atan(1e-10), Math.atan(7.450580596923828e-9)]
--- value
[0,0.7853981633974483,-0.7853981633974483,0.4636476090008061,0.4124104415973873,0.6022873461349642,0.870903457075653,1.1814796049617557,1.4711276743037347,-1.4711276743037347,1.5707963266948965,1.5707963267948966,1.5707963267948966,1.5707963267948966,1e-10,7.450580596923828e-9]

=== Math.atan where glibc's last bit differs
[Math.atan(0.23), Math.atan(2.4), Math.atan(28), Math.atan(0.58), Math.atan(4.4), Math.atan(1.01)]
--- value
[0.2260683879938839,1.1760052070951352,1.5350972141155728,0.5255837935516101,1.3473197256542635,0.7903732467283024]

=== atan at its specials
[Math.atan(NaN), Math.atan(Infinity), Math.atan(-Infinity), Math.atan(5e-324)].map(String).concat([1 / Math.atan(-0) === -Infinity])
--- value
["NaN","1.5707963267948966","-1.5707963267948966","5e-324",true]

=== Math.atan2
[Math.atan2(1, 1), Math.atan2(1, -1), Math.atan2(-1, -1), Math.atan2(-1, 1), Math.atan2(1, 2), Math.atan2(2, 1), Math.atan2(3, -4), Math.atan2(-3, 4), Math.atan2(1, 1e-300), Math.atan2(1e-300, 1), Math.atan2(1e-300, -1), Math.atan2(1, 1e20), Math.atan2(1e20, 1), Math.atan2(1, -1e20), Math.atan2(0.5, 1), Math.atan2(1.7976931348623157e308, 5e-324), Math.atan2(5e-324, 1.7976931348623157e308), Math.atan2(5e-324, -1.7976931348623157e308)]
--- value
[0.7853981633974483,2.356194490192345,-2.356194490192345,-0.7853981633974483,0.4636476090008061,1.1071487177940904,2.498091544796509,-0.6435011087932844,1.5707963267948966,1e-300,3.141592653589793,1e-20,1.5707963267948966,3.141592653589793,0.4636476090008061,1.5707963267948966,0,3.141592653589793]

=== Math.atan2 where glibc's last bit differs
[Math.atan2(-15, -10), Math.atan2(-15, -9.5), Math.atan2(-15, -8.25), Math.atan2(-15, -6.75), Math.atan2(-15, -6)]
--- value
[-2.1587989303424644,-2.135365720462997,-2.073639537722758,-1.9936502529278375,-1.9513027039072617]

=== atan2 at zeros, infinities and NaN
[Math.atan2(0, 0), Math.atan2(0, -0), Math.atan2(-0, -0), Math.atan2(0, -1), Math.atan2(-0, -1), Math.atan2(1, 0), Math.atan2(-1, 0), Math.atan2(1, -0), Math.atan2(-1, -0), Math.atan2(Infinity, Infinity), Math.atan2(-Infinity, Infinity), Math.atan2(Infinity, -Infinity), Math.atan2(-Infinity, -Infinity), Math.atan2(1, Infinity), Math.atan2(1, -Infinity), Math.atan2(-1, -Infinity), Math.atan2(Infinity, 1), Math.atan2(-Infinity, -1), Math.atan2(Infinity, 0), Math.atan2(NaN, 1), Math.atan2(1, NaN), Math.atan2(NaN, NaN), Math.atan2(Infinity, NaN)].map(String).concat([1 / Math.atan2(-0, 0) === -Infinity, 1 / Math.atan2(-0, 1) === -Infinity, 1 / Math.atan2(-1, Infinity) === -Infinity, 1 / Math.atan2(0, 1) === Infinity, 1 / Math.atan2(-0, 1) === -Infinity])
--- value
["0","3.141592653589793","-3.141592653589793","3.141592653589793","-3.141592653589793","1.5707963267948966","-1.5707963267948966","1.5707963267948966","-1.5707963267948966","0.7853981633974483","-0.7853981633974483","2.356194490192345","-2.356194490192345","0","3.141592653589793","-3.141592653589793","1.5707963267948966","-1.5707963267948966","1.5707963267948966","NaN","NaN","NaN","NaN",true,true,true,true,true]

=== trigonometric functions convert primitives as JavaScript does
[Math.sin(), Math.sin(null), Math.sin(true), Math.cos(undefined), Math.cos(false), Math.tan(true), Math.asin(true), Math.asin(null), Math.acos(false), Math.acos(), Math.atan(true), Math.atan(null), Math.atan2(), Math.atan2(1), Math.atan2(null, true), Math.atan2(true, null), Math.atan2(undefined, 1), Math.atan2(false, -1), Math.sin(1, 99), Math.atan2(1, 2, 3)].map(String)
--- value
["NaN","0","0.8414709848078965","NaN","1","1.5574077246549023","1.5707963267948966","0","1.5707963267948966","NaN","0.7853981633974483","0","NaN","NaN","0","1.5707963267948966","NaN","3.141592653589793","0.8414709848078965","0.4636476090008061"]

=== trigonometric functions as values
[[0, 1].map(Math.sin), [0, 1].map(Math.cos), [0, 1].map(Math.tan), [0, 1].map(Math.asin), [0, 1].map(Math.acos), [0, 1].map(Math.atan), [1, 2].map(Math.atan2), [0.5, 1].map(x => Math.asin(Math.sin(x)))]
--- value
[[0,0.8414709848078965],[1,0.5403023058681398],[0,1.5574077246549023],[0,1.5707963267948966],[1.5707963267948966,0],[0,0.7853981633974483],[1.5707963267948966,1.1071487177940904],[0.5,1]]

=== Math.sin of a string faults
Math.sin("1")
--- fault converts only numbers

=== Math.acos of an array faults
Math.acos([0])
--- fault converts only numbers

=== Math.atan2 of an object faults
Math.atan2({}, 1)
--- fault converts only numbers

=== Math.atan2 of a string second faults
Math.atan2(1, "2")
--- fault converts only numbers

=== Math.tan of a BigInt faults
Math.tan(1n)
--- fault BigInt

=== Math.atan2 of a BigInt faults
Math.atan2(1, 1n)
--- fault BigInt

# Math.sinh, cosh, tanh, asinh, acosh, atanh and cbrt: V8's fdlibm
# (src/math.c), bit for bit (values generated by Node 22). Some inputs are
# ones where glibc's last bit differs from V8's; 710.4758600739439 is the
# overflow threshold of sinh and cosh (0x408633CE 8FB9F87D), 709.7822265625
# their log(MAX) (0x40862E42 00000000).

=== Math.sinh
[Math.sinh(0), Math.sinh(1), Math.sinh(-1), Math.sinh(0.5), Math.sinh(0.9999999999999999), Math.sinh(2), Math.sinh(10), Math.sinh(21.999999999999996), Math.sinh(22), Math.sinh(22.5), Math.sinh(100), Math.sinh(709), Math.sinh(709.7822265625), Math.sinh(709.782712893384), Math.sinh(710), Math.sinh(710.4758600739439), Math.sinh(-710.4758600739439), Math.sinh(1e-10), Math.sinh(3.725290298461914e-9), Math.sinh(3.7252902984619136e-9)]
--- value
[0,1.1752011936438014,-1.1752011936438014,0.5210953054937474,1.1752011936438014,3.626860407847019,11013.232874703393,1792456423.0657895,1792456423.065796,2955261031.5116453,1.3440585709080678e+43,4.109203730777486e+307,8.984095368647863e+307,8.988465674311366e+307,1.1169973830808557e+308,1.7976931348621744e+308,-1.7976931348621744e+308,1e-10,3.725290298461914e-9,3.7252902984619136e-9]

=== Math.sinh where glibc's last bit differs
[Math.sinh(3.939), Math.sinh(-3.939), Math.sinh(22.08), Math.sinh(22.11), Math.sinh(22.2)]
--- value
[25.673870363635967,-25.673870363635967,1941744862.4780874,2000879797.3214955,2189311219.014474]

=== Math.cosh
[Math.cosh(0), Math.cosh(1), Math.cosh(-1), Math.cosh(0.34657359027997264), Math.cosh(0.3465735902799727), Math.cosh(0.5), Math.cosh(2), Math.cosh(21.999999999999996), Math.cosh(22), Math.cosh(30), Math.cosh(709.7822265625), Math.cosh(710), Math.cosh(710.4758600739439), Math.cosh(-710.4758600739439), Math.cosh(1e-10), Math.cosh(2e-17), Math.cosh(-5)]
--- value
[1,1.5430806348152437,1.5430806348152437,1.0606601717798212,1.0606601717798214,1.1276259652063807,3.7621956910836314,1792456423.0657895,1792456423.065796,5343237290762.231,8.984095368647863e+307,1.1169973830808557e+308,1.7976931348621744e+308,1.7976931348621744e+308,1,1,74.20994852478785]

=== Math.cosh where glibc's last bit differs
[Math.cosh(0.42), Math.cosh(0.92), Math.cosh(0.93), Math.cosh(0.98), Math.cosh(1.02), Math.cosh(1.08), Math.cosh(-19.86)]
--- value
[1.0895041877168454,1.453904715510406,1.464531443994728,1.5198836703904084,1.5668948520686883,1.6421375383552315,210891179.0791859]

=== Math.tanh
[Math.tanh(0), Math.tanh(1), Math.tanh(-1), Math.tanh(0.5), Math.tanh(-0.5), Math.tanh(0.9999999999999999), Math.tanh(2), Math.tanh(10), Math.tanh(21.999999999999996), Math.tanh(22), Math.tanh(30), Math.tanh(1e-10), Math.tanh(3.725290298461914e-9), Math.tanh(3.7252902984619136e-9), Math.tanh(1e-300)]
--- value
[0,0.7615941559557649,-0.7615941559557649,0.46211715726000974,-0.46211715726000974,0.7615941559557649,0.9640275800758169,0.9999999958776927,1,1,1,1e-10,3.725290298461914e-9,3.7252902984619136e-9,1e-300]

=== Math.tanh where glibc's last bit differs
[Math.tanh(0.155), Math.tanh(-0.155)]
--- value
[0.15377052226409268,-0.15377052226409268]

=== Math.asinh
[Math.asinh(0), Math.asinh(1), Math.asinh(-1), Math.asinh(0.5), Math.asinh(2), Math.asinh(2.0000000000000004), Math.asinh(10), Math.asinh(1e8), Math.asinh(268435456), Math.asinh(268435456.00000006), Math.asinh(1e300), Math.asinh(1.7976931348623157e308), Math.asinh(-1.7976931348623157e308), Math.asinh(1e-10), Math.asinh(3.725290298461914e-9), Math.asinh(3.7252902984619136e-9)]
--- value
[0,0.881373587019543,-0.881373587019543,0.48121182505960347,1.4436354751788103,1.4436354751788105,2.99822295029797,19.11382792451231,20.101268236238415,20.101268236238415,691.4686750787736,710.4758600739439,-710.4758600739439,1e-10,3.725290298461914e-9,3.7252902984619136e-9]

=== Math.asinh where glibc's last bit differs
[Math.asinh(2.02), Math.asinh(2.27), Math.asinh(2.34), Math.asinh(2.46), Math.asinh(-19.88)]
--- value
[1.45254413624738,1.558250713691684,1.5861119704218916,1.6322721925270534,-3.6834933504612826]

=== Math.acosh
[Math.acosh(1), Math.acosh(1.0000000000000002), Math.acosh(1.5), Math.acosh(2), Math.acosh(2.0000000000000004), Math.acosh(3), Math.acosh(10), Math.acosh(268435456), Math.acosh(268435456.00000006), Math.acosh(1e300), Math.acosh(1.7976931348623157e308)]
--- value
[0,2.1073424255447017e-8,0.9624236501192069,1.3169578969248166,1.316957896924817,1.7627471740390859,2.993222846126381,20.10126823623841,20.10126823623841,691.4686750787736,710.4758600739439]

=== Math.acosh where glibc's last bit differs
[Math.acosh(2.22), Math.acosh(2.3), Math.acosh(2.66), Math.acosh(2.9), Math.acosh(3.13)]
--- value
[1.4355651651111732,1.475044781241425,1.6341057875266878,1.7267108656228922,1.8076257389499508]

=== Math.atanh
[Math.atanh(0), Math.atanh(0.5), Math.atanh(-0.5), Math.atanh(0.49999999999999994), Math.atanh(0.25), Math.atanh(0.9), Math.atanh(0.99), Math.atanh(0.9999999999999999), Math.atanh(-0.9999999999999999), Math.atanh(1e-10), Math.atanh(3.725290298461914e-9), Math.atanh(3.7252902984619136e-9)]
--- value
[0,0.5493061443340548,-0.5493061443340548,0.5493061443340548,0.25541281188299536,1.4722194895832204,2.6466524123622457,18.714973875118524,-18.714973875118524,1e-10,3.725290298461914e-9,3.7252902984619136e-9]

=== Math.atanh where glibc's last bit differs
[Math.atanh(0.039), Math.atanh(-0.039), Math.atanh(0.9172), Math.atanh(-0.9172)]
--- value
[0.03901979106446745,-0.03901979106446745,1.5710965029471313,-1.5710965029471313]

=== Math.cbrt
[Math.cbrt(0), Math.cbrt(1), Math.cbrt(-1), Math.cbrt(8), Math.cbrt(-8), Math.cbrt(27), Math.cbrt(64), Math.cbrt(1000), Math.cbrt(1e9), Math.cbrt(2), Math.cbrt(3), Math.cbrt(0.001), Math.cbrt(1e-300), Math.cbrt(1e300), Math.cbrt(1.7976931348623157e308), Math.cbrt(-1.7976931348623157e308), Math.cbrt(2.2250738585072014e-308), Math.cbrt(2.225073858507201e-308), Math.cbrt(1e-310), Math.cbrt(5e-324), Math.cbrt(-5e-324)]
--- value
[0,1,-1,2,-2,3,4,10,1000,1.2599210498948732,1.4422495703074083,0.1,1e-100,1e+100,5.643803094122362e+102,-5.643803094122362e+102,2.812644285236262e-103,2.8126442852362615e-103,4.641588833612774e-104,1.7031839360032603e-108,-1.7031839360032603e-108]

=== Math.cbrt where glibc's last bit differs
[Math.cbrt(0.01), Math.cbrt(0.02), Math.cbrt(0.1), Math.cbrt(0.11), Math.cbrt(-19.99)]
--- value
[0.21544346900318836,0.27144176165949063,0.4641588833612779,0.4791419857062784,-2.7139651382373664]

=== sinh, cosh and tanh at their specials
[Math.sinh(NaN), Math.sinh(Infinity), Math.sinh(-Infinity), Math.sinh(710.475860073944), Math.sinh(-710.475860073944), Math.sinh(1e300), Math.cosh(NaN), Math.cosh(Infinity), Math.cosh(-Infinity), Math.cosh(710.475860073944), Math.cosh(-1e300), Math.tanh(NaN), Math.tanh(Infinity), Math.tanh(-Infinity), Math.tanh(1e300), Math.sinh(5e-324), Math.tanh(-5e-324), Math.cosh(5e-324)].map(String).concat([1 / Math.sinh(-0) === -Infinity, 1 / Math.tanh(-0) === -Infinity, 1 / Math.sinh(0) === Infinity, Math.cosh(-0) === 1, Math.sinh(-5e-324) === -5e-324])
--- value
["NaN","Infinity","-Infinity","Infinity","-Infinity","Infinity","NaN","Infinity","Infinity","Infinity","Infinity","NaN","1","-1","1","5e-324","-5e-324","1",true,true,true,true,true]

=== asinh, acosh and atanh at their specials
[Math.asinh(NaN), Math.asinh(Infinity), Math.asinh(-Infinity), Math.acosh(NaN), Math.acosh(Infinity), Math.acosh(-Infinity), Math.acosh(0.9999999999999999), Math.acosh(0.5), Math.acosh(0), Math.acosh(-1), Math.atanh(NaN), Math.atanh(1), Math.atanh(-1), Math.atanh(1.0000000000000002), Math.atanh(-2), Math.atanh(Infinity), Math.asinh(5e-324), Math.atanh(-5e-324)].map(String).concat([1 / Math.asinh(-0) === -Infinity, 1 / Math.atanh(-0) === -Infinity, 1 / Math.acosh(1) === Infinity, 1 / Math.asinh(0) === Infinity])
--- value
["NaN","Infinity","-Infinity","NaN","Infinity","NaN","NaN","NaN","NaN","NaN","NaN","Infinity","-Infinity","NaN","NaN","NaN","5e-324","-5e-324",true,true,true,true]

=== cbrt at its specials
[Math.cbrt(NaN), Math.cbrt(Infinity), Math.cbrt(-Infinity), Math.cbrt(4.9406564584124654e-324), Math.cbrt(-1e-320)].map(String).concat([1 / Math.cbrt(-0) === -Infinity, 1 / Math.cbrt(0) === Infinity, Math.cbrt(-27) === -3, Math.cbrt(0.125) === 0.5])
--- value
["NaN","Infinity","-Infinity","1.7031839360032603e-108","-2.1544266950262728e-107",true,true,true,true]

=== hyperbolic functions and cbrt convert primitives as JavaScript does
[Math.sinh(), Math.sinh(null), Math.sinh(true), Math.cosh(undefined), Math.cosh(false), Math.tanh(true), Math.asinh(true), Math.asinh(null), Math.acosh(true), Math.acosh(null), Math.acosh(), Math.atanh(false), Math.atanh(true), Math.atanh(null), Math.cbrt(true), Math.cbrt(undefined), Math.cbrt(null), Math.cbrt(8, 99), Math.sinh(1, 2)].map(String)
--- value
["NaN","0","1.1752011936438014","NaN","1","0.7615941559557649","0.881373587019543","0","0","NaN","NaN","0","Infinity","0","1","NaN","0","2","1.1752011936438014"]

=== hyperbolic functions and cbrt as values
[[0, 1].map(Math.sinh), [0, 1].map(Math.cosh), [0, 1].map(Math.tanh), [0, 1].map(Math.asinh), [1, 2].map(Math.acosh), [0, 0.5].map(Math.atanh), [8, 27].map(Math.cbrt), [0.5, 2].map(x => Math.asinh(Math.sinh(x)))]
--- value
[[0,1.1752011936438014],[1,1.5430806348152437],[0,0.7615941559557649],[0,0.881373587019543],[0,1.3169578969248166],[0,0.5493061443340548],[2,3],[0.5,2]]

=== Math.sinh of a string faults
Math.sinh("1")
--- fault converts only numbers

=== Math.cosh of an array faults
Math.cosh([0])
--- fault converts only numbers

=== Math.tanh of an object faults
Math.tanh({})
--- fault converts only numbers

=== Math.asinh of a string faults
Math.asinh("2")
--- fault converts only numbers

=== Math.acosh of an array faults
Math.acosh([2])
--- fault converts only numbers

=== Math.atanh of an object faults
Math.atanh({ x: 0.5 })
--- fault converts only numbers

=== Math.cbrt of a string faults
Math.cbrt("8")
--- fault converts only numbers

=== Math.cbrt of a BigInt faults
Math.cbrt(8n)
--- fault BigInt

=== Math.atanh of a BigInt faults
Math.atanh(0n)
--- fault BigInt

=== Number on strings
[" 12 ", "0x1F", "0b101", "0o17", "", "1e3", "-Infinity", "1_000", "12px", ".5", "5."].map(s => String(Number(s)))
--- value
["12","31","5","15","0","1000","-Infinity","NaN","NaN","0.5","5"]

=== Number on other primitives
[Number(true), Number(null), String(Number(undefined)), Number(), 1 / Number("-0") === -Infinity]
--- value
[1,0,"NaN",0,true]

=== parseInt
[parseInt("  -42px"), parseInt("0x1A"), parseInt("1A", 16), parseInt("z", 36), String(parseInt("9", 8)), parseInt("12345678901234567890123"), parseInt("101", 2), String(parseInt(""))]
--- value
[-42,26,26,35,"NaN",1.2345678901234568e+22,5,"NaN"]

=== parseFloat
["3.14abc", "-.5e2x", "1e300", "123456789012345678901234567890", "2.2250738585072011e-308", "Infinityx", "0.1e-400", "  7"].map(s => String(parseFloat(s)))
--- value
["3.14","-50","1e+300","1.2345678901234568e+29","2.225073858507201e-308","Infinity","0","7"]

=== exact literals
[1e300, 5e-324, 0.1 + 0.2, 9007199254740993, 1.7976931348623157e308].map(x => String(x))
--- value
["1e+300","5e-324","0.30000000000000004","9007199254740992","1.7976931348623157e+308"]

=== constants
[Number.EPSILON, Number.MAX_SAFE_INTEGER, Number.MIN_VALUE, Math.PI, Math.E, Math.SQRT1_2].map(x => String(x))
--- value
["2.220446049250313e-16","9007199254740991","5e-324","3.141592653589793","2.718281828459045","0.7071067811865476"]

=== sqrt, fround, clz32, imul
[Math.sqrt(2), Math.sqrt(-1), Math.fround(5.5), Math.fround(0.1), Math.clz32(1), Math.clz32(0), Math.imul(0xffffffff, 5), Math.imul(3, -4)].map(x => String(x))
--- value
["1.4142135623730951","NaN","5.5","0.10000000149011612","31","32","-5","-12"]

=== hypot
[Math.hypot(3, 4), Math.hypot(), Math.hypot(NaN, Infinity), Math.hypot(1e200, 1e200), Math.hypot(0.1, 0.2, 0.3), Math.hypot(-0)].map(x => String(x))
--- value
["5","0","Infinity","1.414213562373095e+200","0.37416573867739417","0"]

=== Number of an object
Number({})
--- fault

=== parseInt of an array
parseInt([1])
--- fault

=== isNaN of a string
isNaN("x")
--- fault

# A number's toFixed, toExponential and toPrecision: the exact decimal value
# of the double, rounded half up (the larger n of two as near). Faults where
# JavaScript throws a RangeError, and on an argument it would coerce from a
# string or an object.
=== toFixed rounds the exact value half up
[(1.005).toFixed(2), (1.045).toFixed(2), (1.255).toFixed(2), (8.345).toFixed(2), (10.235).toFixed(2), (9.995).toFixed(2), (99.995).toFixed(2), (4.35).toFixed(1), (0.125).toFixed(2), (0.375).toFixed(2), (2.5).toFixed(0), (-2.5).toFixed(0), (0.5).toFixed(0), (1.5).toFixed(0), (123.456).toFixed(1), (0.0005).toFixed(3), (0.00005).toFixed(4)]
--- value
["1.00","1.04","1.25","8.35","10.23","9.99","100.00","4.3","0.13","0.38","3","-3","1","2","123.5","0.001","0.0001"]

=== toFixed of zero, -0, small and large values
[(0).toFixed(2), (-0).toFixed(2), (-0.0001).toFixed(2), (-0.5).toFixed(0), (0.000001).toFixed(7), (1e-7).toFixed(6), (1e-7).toFixed(7), (1e21).toFixed(2), (-1e21).toFixed(3), (999999999999999900000).toFixed(2), (9007199254740993).toFixed(1), (1.7976931348623157e308).toFixed(2)]
--- value
["0.00","0.00","-0.00","-1","0.0000010","0.000000","0.0000001","1e+21","-1e+21","999999999999999868928.00","9007199254740992.0","1.7976931348623157e+308"]

=== toFixed with many digits
[(0.1).toFixed(20), (0.1).toFixed(100), (5e-324).toFixed(100), (1.5).toFixed(100), (123.456).toFixed(50)]
--- value
["0.10000000000000000555","0.1000000000000000055511151231257827021181583404541015625000000000000000000000000000000000000000000000","0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000","1.5000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000","123.45600000000000306954461848363280296325683593750000"]

=== toFixed of NaN and the infinities
[NaN.toFixed(2), Infinity.toFixed(0), (-Infinity).toFixed(100), NaN.toFixed()]
--- value
["NaN","Infinity","-Infinity","NaN"]

=== toFixed converts its argument without coercing an object
[(1.5).toFixed(), (1.5).toFixed(undefined), (1.5).toFixed(null), (1.5).toFixed(true), (1.5).toFixed(false), (1.5).toFixed(2.9), (1.5).toFixed(-0.5), (1.5).toFixed(-0), (1.5).toFixed(NaN), (1.25).toFixed(1.99)]
--- value
["2","2","2","1.5","2","1.50","2","2","2","1.3"]

=== toExponential with no digits gives the shortest digits
[(123.456).toExponential(), (0).toExponential(), (-0).toExponential(), (5e-324).toExponential(), (0.1 + 0.2).toExponential(), (1e21).toExponential(), (-1.7976931348623157e308).toExponential(), (100).toExponential(), (1.5).toExponential(undefined)]
--- value
["1.23456e+2","0e+0","0e+0","5e-324","3.0000000000000004e-1","1e+21","-1.7976931348623157e+308","1e+2","1.5e+0"]

=== toExponential rounds the exact value half up
[(123.456).toExponential(2), (-0).toExponential(2), (5e-324).toExponential(3), (1.7976931348623157e308).toExponential(20), (1.25).toExponential(1), (1.35).toExponential(1), (-1.5).toExponential(0), (9.5).toExponential(0), (99.5).toExponential(1), (0.000123).toExponential(1), (123.456).toExponential(100)]
--- value
["1.23e+2","0.00e+0","4.941e-324","1.79769313486231570815e+308","1.3e+0","1.4e+0","-2e+0","1e+1","1.0e+2","1.2e-4","1.2345600000000000306954461848363280296325683593750000000000000000000000000000000000000000000000000000e+2"]

=== toExponential of NaN and the infinities, whatever the digits
[NaN.toExponential(), Infinity.toExponential(-1), (-Infinity).toExponential(101), NaN.toExponential(1e9), Infinity.toExponential(-Infinity)]
--- value
["NaN","Infinity","-Infinity","NaN","Infinity"]

=== toExponential converts its argument without coercing an object
[(1.5).toExponential(null), (1.5).toExponential(true), (1.5).toExponential(NaN), (1.5).toExponential(2.7), (1.5).toExponential(-0.9)]
--- value
["2e+0","1.5e+0","2e+0","1.50e+0","2e+0"]

=== toPrecision chooses the fixed or the exponential form
[(123.456).toPrecision(), (123.456).toPrecision(1), (123.456).toPrecision(2), (123.456).toPrecision(4), (123.456).toPrecision(10), (0.000001234).toPrecision(2), (0.0000001234).toPrecision(2), (123456).toPrecision(2), (123456).toPrecision(6), (1e21).toPrecision(3), (1e21).toPrecision(22), (99.99).toPrecision(3), (999.5).toPrecision(3), (0.00001).toPrecision(1)]
--- value
["123.456","1e+2","1.2e+2","123.5","123.4560000","0.0000012","1.2e-7","1.2e+5","123456","1.00e+21","1000000000000000000000","100","1.00e+3","0.00001"]

=== toPrecision rounds the exact value half up
[(2.5).toPrecision(1), (-2.5).toPrecision(1), (1.005).toPrecision(3), (1.45).toPrecision(2), (5e-324).toPrecision(1), (5e-324).toPrecision(3), (1.7976931348623157e308).toPrecision(5), (0).toPrecision(3), (-0).toPrecision(1), (1.5).toPrecision(100), (0.1).toPrecision(21)]
--- value
["3","-3","1.00","1.4","5e-324","4.94e-324","1.7977e+308","0.00","0","1.500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000","0.100000000000000005551"]

=== toPrecision of NaN and the infinities, whatever the precision
[NaN.toPrecision(0), Infinity.toPrecision(101), (-Infinity).toPrecision(), NaN.toPrecision(), Infinity.toPrecision(null)]
--- value
["NaN","Infinity","-Infinity","NaN","Infinity"]

=== toPrecision converts its argument without coercing an object
[(1.5).toPrecision(undefined), (1.5).toPrecision(true), (1.5).toPrecision(2.9), (1.5).toPrecision(1.5)]
--- value
["1.5","2","1.5","2"]

=== the methods on numbers in variables and callbacks
[1.005, 2.5, -0.5, 1e21].map(x => [x.toFixed(1), x.toExponential(1), x.toPrecision(1)])
--- value
[["1.0","1.0e+0","1"],["2.5","2.5e+0","3"],["-0.5","-5.0e-1","-0.5"],["1e+21","1.0e+21","1e+21"]]

=== toFixed of more than 100 digits
(1).toFixed(101)
--- fault RangeError

=== toFixed of negative digits
(1).toFixed(-1)
--- fault RangeError

=== toFixed of Infinity digits
(1).toFixed(Infinity)
--- fault RangeError

=== toFixed checks its digits before NaN and the infinities
Infinity.toFixed(101)
--- fault RangeError

=== toFixed checks its digits before NaN
NaN.toFixed(-1)
--- fault RangeError

=== toExponential of negative digits
(1).toExponential(-1)
--- fault RangeError

=== toExponential of more than 100 digits
(-1).toExponential(101)
--- fault RangeError

=== toPrecision of 0
(1).toPrecision(0)
--- fault RangeError

=== toPrecision of more than 100
(1).toPrecision(101)
--- fault RangeError

=== toPrecision of null, which is 0
(1.5).toPrecision(null)
--- fault RangeError

=== toPrecision of NaN, which is 0
(1.5).toPrecision(NaN)
--- fault RangeError

=== toFixed of a string, which JavaScript converts
(1).toFixed("2")
--- fault converts only numbers

=== toExponential of a string, which JavaScript converts
(1).toExponential("2")
--- fault converts only numbers

=== toPrecision of a string, which JavaScript converts
(1).toPrecision("2")
--- fault converts only numbers

=== toExponential of Infinity with a string, which JavaScript converts
Infinity.toExponential("x")
--- fault converts only numbers

=== toFixed of an array, which JavaScript coerces
(1).toFixed([2])
--- fault converts only numbers

=== toFixed of an object
(1).toFixed({})
--- fault converts only numbers

=== toFixed of a BigInt
(1).toFixed(1n)
--- fault BigInt would become a number

=== toFixed read as a value
(1).toFixed
--- fault reads properties

=== toLocaleString of a number
(1.5).toLocaleString()
--- fault on numbers only those that format digits

=== toFixed of a string
"1.5".toFixed(2)
--- fault does not have this method

=== toFixed of a BigInt receiver
(1n).toFixed(2)
--- fault does not have this method

# Review of toFixed, toExponential and toPrecision: negative values that
# round to zero, the 1e21 boundary, values just around a half, subnormals,
# large integers, every kind of digit count, and the order of the checks.

=== toFixed of negative values that round to zero keep the sign
[(-0.0000001).toFixed(2), (-0.004).toFixed(2), (-0.005).toFixed(2), (-0.0050000000000000001).toFixed(2), (-1e-300).toFixed(0), (-5e-324).toFixed(100).length, (-0.49999999999999994).toFixed(0)]
--- value
["-0.00","-0.00","-0.01","-0.01","-0",103,"-0"]

=== toFixed, toPrecision and toExponential around 1e21
[(999999999999999868928).toFixed(2), (1e21 - 65536).toFixed(0), (1e21).toFixed(0), (1e21 + 131072).toFixed(2), (-999999999999999868928).toFixed(1), (999999999999999868928).toPrecision(21), (999999999999999868928).toPrecision(22), (1e21).toPrecision(21), (1e21).toExponential(20)]
--- value
["999999999999999868928.00","1e+21","1e+21","1.0000000000000001e+21","-999999999999999868928.0","999999999999999868928","999999999999999868928.0","1.00000000000000000000e+21","1.00000000000000000000e+21"]

=== toFixed of a value just above and just below a half
[(0.5000000000000001).toFixed(0), (0.49999999999999994).toFixed(0), (1.0000000000000002).toFixed(15), (0.9999999999999999).toFixed(15), (0.9999999999999999).toFixed(16), (0.0004999999999999999).toFixed(3), (0.095).toFixed(2), (0.0095).toFixed(3)]
--- value
["1","0","1.000000000000000","1.000000000000000","0.9999999999999999","0.000","0.10","0.009"]

=== the three methods of subnormals and the smallest normal
[(2.2250738585072014e-308).toExponential(5), (2.225073858507201e-308).toPrecision(17), (4.94e-322).toExponential(), (5e-324).toPrecision(100).length, (2.2250738585072014e-308).toFixed(100)]
--- value
["2.22507e-308","2.2250738585072009e-308","4.94e-322",106,"0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"]

=== the three methods of large integers
[(9007199254740993).toPrecision(17), (9007199254740992).toExponential(), (123456789012345680000).toFixed(2), (1000000000000000128).toFixed(0), (1000000000000000128).toPrecision(21), (6.02214076e23).toPrecision(3), (1e100).toFixed(2), (1.5e300).toPrecision(2)]
--- value
["9007199254740992.0","9.007199254740992e+15","123456789012345683968.00","1000000000000000128","1000000000000000128.00","6.02e+23","1e+100","1.5e+300"]

=== the three methods with every digit count from a fixed list
[0, 1, 2, 5, 17, 20, 50, 100].map((d) => [(1 / 3).toFixed(d), (2 / 3).toExponential(d), (123.456).toPrecision(d === 0 ? 1 : d)])
--- value
[["0","7e-1","1e+2"],["0.3","6.7e-1","1e+2"],["0.33","6.67e-1","1.2e+2"],["0.33333","6.66667e-1","123.46"],["0.33333333333333331","6.66666666666666630e-1","123.45600000000000"],["0.33333333333333331483","6.66666666666666629659e-1","123.45600000000000307"],["0.33333333333333331482961625624739099293947219848633","6.66666666666666629659232512494781985878944396972656e-1","123.45600000000000306954461848363280296325683593750"],["0.3333333333333333148296162562473909929394721984863281250000000000000000000000000000000000000000000000","6.6666666666666662965923251249478198587894439697265625000000000000000000000000000000000000000000000000e-1","123.4560000000000030695446184836328029632568359375000000000000000000000000000000000000000000000000000"]]

=== the three methods, with digits from a callback's counter
[1.23456, -2.5, 0.000123].map((x, i) => [x.toFixed(i), x.toExponential(i), x.toPrecision(i + 1)])
--- value
[["1","1e+0","1"],["-2.5","-2.5e+0","-2.5"],["0.00","1.23e-4","0.000123"]]

=== the methods' digits as booleans, null, -0 and fractions
[(2.5).toFixed(true), (2.5).toFixed(false), (2.5).toExponential(false), (2.5).toPrecision(true), (2.5).toFixed(0.9), (2.5).toFixed(-0.9), (2.5).toExponential(-0), (2.5).toPrecision(1.9), (2.5).toFixed(100.5).length]
--- value
["2.5","3","3e+0","3","3","3","3e+0","3",102]

=== toFixed of a BigInt digit count
(1.5).toFixed(1n)
--- fault JavaScript throws a TypeError where a

=== toPrecision of a BigInt precision
(1.5).toPrecision(1n)
--- fault JavaScript throws a TypeError where a

=== toExponential of NaN with a BigInt digit count
NaN.toExponential(1n)
--- fault JavaScript throws a TypeError where a

=== toPrecision of Infinity with a negative precision, which it never checks
[Infinity.toPrecision(-1), NaN.toPrecision(0), (-Infinity).toPrecision(1e9)]
--- value
["Infinity","NaN","-Infinity"]

=== toFixed of -Infinity digits
(1).toFixed(-Infinity)
--- fault JavaScript throws a RangeError for toFixed's

=== toExponential of Infinity digits on a finite value
(1).toExponential(Infinity)
--- fault JavaScript throws a RangeError for toExponential's

=== toPrecision of -0.5, which is 0
(1).toPrecision(-0.5)
--- fault JavaScript throws a RangeError for toPrecision's

=== toFixed of 100.9, which is 100
(1).toFixed(100.9).length
--- value
102

# A number's toString(radix): String(x) with no radix or radix 10; in any
# other radix 2 to 36 the digits V8 writes (DoubleToRadixCString): the
# integer part exactly, with zeros under the double's precision, and the
# fraction until what is left is under half the distance to the next
# double, rounded at the last digit. The radix is ToIntegerOrInfinity'd;
# outside 2 to 36 faults where JavaScript throws a RangeError, and a radix
# JavaScript would convert from a string or an object faults.

=== toString of a number with no radix is String(x)
[(1.5).toString(), (0).toString(), (-0).toString(), NaN.toString(), Infinity.toString(), (-Infinity).toString(), (1e21).toString(), (1e-7).toString(), (0.1 + 0.2).toString(), (5e-324).toString(), (123.456).toString(undefined), (255).toString(10), (-1.5e300).toString(10.5)]
--- value
["1.5","0","0","NaN","Infinity","-Infinity","1e+21","1e-7","0.30000000000000004","5e-324","123.456","255","-1.5e+300"]

=== toString of integers in a radix
[(255).toString(16), (255).toString(2), (-255).toString(36), (35).toString(36), (36).toString(36), (2147483648).toString(32), (4294967295).toString(16), (9007199254740992).toString(2), (9007199254740994).toString(3), (18446744073709551616).toString(16), (1e21).toString(36), (1e21).toString(7), (-9007199254740991).toString(36)]
--- value
["ff","11111111","-73","z","10","2000000","ffffffff","100000000000000000000000000000000000000000000000000000","1121202011211211122211100012101111","10000000000000000","5v1j4f4ds7c000","5135235413265003022600000","-2gosa7pa2gv"]

=== toString of large doubles: zeros for the digits under their precision
[(1e300).toString(36), Number.MAX_VALUE.toString(36), (-Number.MAX_VALUE).toString(16)]
--- value
["fhgyjdfcg7s00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000","1a1e4vngaiqo0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000","-fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"]

=== toString of the largest double in radix 2
Number.MAX_VALUE.toString(2)
--- value
"1111111111111111111111111111111111111111111111111111100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

=== toString of fractions in a radix
[(0.5).toString(2), (0.1).toString(2), (0.1).toString(3), (1 / 3).toString(3), (2 / 3).toString(3), (0.1 + 0.2).toString(16), (-0.5).toString(16), (123.456).toString(36), (1e-7).toString(36), (Math.PI).toString(16), (Math.E).toString(7), (255.5).toString(16), (-0.1).toString(5)]
--- value
["0.1","0.0001100110011001100110011001100110011001100110011001101","0.0022002200220022002200220022002201","0.1","0.2","0.4cccccccccccd","-0.8","3f.gez4w97ry","0.000061oezo085tl","3.243f6a8885a3","2.501241065422650434","ff.8","-0.0222222222222222222222222"]

=== toString whose last digit rounds up and carries
[(0.2).toString(3), (4 / 3).toString(21), (0.1).toString(18), (0.3).toString(15), (0.9999999999999999).toString(6), (575.9999999999999).toString(24), (6666666666.666666).toString(18)]
--- value
["0.01210121012101210121012101210121021","1.7","0.1e73ae73ae73af","0.47777777777778","0.555555555555555555553","nn.nnnnnnnnnh","ag02a7c6.bhhhh"]

=== toString of the smallest subnormal in radix 2
(5e-324).toString(2)
--- value
"0.000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001"

=== toString of subnormals and the smallest normal
[(5e-324).toString(36), (2.2250738585072014e-308).toString(36), (2.2250738585072014e-308).toString(32), (1e-300).toString(29)]
--- value
["0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003","0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000034lmua2oev","0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000008","0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000hrd81g2jd9e"]

=== toString of -0, NaN and the infinities in a radix
[(-0).toString(2), (-0).toString(36), NaN.toString(16), Infinity.toString(36), (-Infinity).toString(2), (0).toString(7)]
--- value
["0","0","NaN","Infinity","-Infinity","0"]

=== toString cuts a fractional radix toward zero
[(255).toString(16.9), (255).toString(2.5), (255).toString(-0 + 36), (255).toString(undefined), (255).toString(36.99)]
--- value
["ff","11111111","73","255","73"]

=== toString of a number in method chains
[(255).toString().length, (255).toString(16).toUpperCase(), [1, 2, 255].map(x => x.toString(2)), [10, 0.5, -3].map(x => x.toString()).join(","), (5).toString(2).padStart(8, "0"), (0.5).toString(2).slice(2)]
--- value
[3,"FF",["1","10","11111111"],"10,0.5,-3","00000101","1"]

=== a BigInt's toString beside a number's
[(0n).toString(), (255n).toString(16), (-255n).toString(2), (2n ** 64n).toString(36), (255).toString(16) === (255n).toString(16)]
--- value
["0","ff","-11111111","3w5e11264sgsg",true]

=== toString with radix 37
(255).toString(37)
--- fault RangeError

=== toString with radix 1
(255).toString(1)
--- fault RangeError

=== toString with radix 0
(255).toString(0)
--- fault RangeError

=== toString with radix -0
(255).toString(-0)
--- fault RangeError

=== toString with radix null, which is 0
(255).toString(null)
--- fault RangeError

=== toString with radix true, which is 1
(255).toString(true)
--- fault RangeError

=== toString with radix NaN, which is 0
(255).toString(NaN)
--- fault RangeError

=== toString with radix Infinity
(255).toString(Infinity)
--- fault RangeError

=== toString with a negative radix
(255).toString(-16)
--- fault RangeError

=== toString of NaN checks its radix
NaN.toString(37)
--- fault RangeError

=== toString of Infinity checks its radix
Infinity.toString(1)
--- fault RangeError

=== toString with a string radix, which JavaScript converts
(255).toString("16")
--- fault converts only numbers

=== toString with an array radix, which JavaScript coerces
(255).toString([16])
--- fault converts only numbers

=== toString with an object radix
(255).toString({})
--- fault converts only numbers

=== toString with a BigInt radix
(255).toString(16n)
--- fault BigInt would become a number

# toString(radix) beside every radix's half-digit ties, powers less an ulp
# and integers past 2^53 (from the batch's review).
=== toString in every radix: half-digit ties, a power of the radix less an ulp, and past 2^53
[[0.25, 2], [0.375, 2], [0.1875, 2], [127.99999999999999, 2], [128.5, 2], [-1048577, 2], [0.16666666666666666, 3], [0.16666666666666666, 3], [0.09259259259259259, 3], [2186.9999999999995, 3], [2187.5, 3], [-3486784402, 3], [0.125, 4], [0.09375, 4], [0.0546875, 4], [16383.999999999998, 4], [16384.5, 4], [-1099511627777, 4], [0.1, 5], [0.06, 5], [0.036, 5], [78124.99999999999, 5], [78125.5, 5], [-95367431640626, 5], [0.08333333333333333, 6], [0.041666666666666664, 6], [0.02546296296296296, 6], [279935.99999999994, 6], [279936.5, 6], [-3656158440062977, 6], [0.07142857142857142, 7], [0.030612244897959183, 7], [0.018950437317784258, 7], [823542.9999999999, 7], [823543.5, 7], [-79792266297612000, 7], [0.0625, 8], [0.0234375, 8], [0.0146484375, 8], [2097151.9999999998, 8], [2097152.5, 8], [-1152921504606847000, 8], [0.05555555555555555, 9], [0.018518518518518517, 9], [0.011659807956104253, 9], [4782968.999999999, 9], [4782969.5, 9], [-12157665459056929000, 9], [0.05, 10], [0.015, 10], [0.0095, 10], [9999999.999999998, 10], [10000000.5, 10], [-100000000000000000000, 10], [0.045454545454545456, 11], [0.012396694214876033, 11], [0.007888805409466567, 11], [19487170.999999996, 11], [19487171.5, 11], [-672749994932560000000, 11], [0.041666666666666664, 12], [0.010416666666666666, 12], [0.006655092592592593, 12], [35831807.99999999, 12], [35831808.5, 12], [-3.833759992447475e+21, 12], [0.038461538461538464, 13], [0.008875739644970414, 13], [0.005689576695493855, 13], [62748516.99999999, 13], [62748517.5, 13], [-1.90049637748808e+22, 13], [0.03571428571428571, 14], [0.007653061224489796, 14], [0.004919825072886298, 14], [105413503.99999999, 14], [105413504.5, 14], [-8.36682554252848e+22, 14], [0.03333333333333333, 15], [0.006666666666666667, 15], [0.004296296296296296, 15], [170859374.99999997, 15], [170859375.5, 15], [-3.3252567300796506e+23, 15], [0.03125, 16], [0.005859375, 16], [0.0037841796875, 16], [268435455.99999997, 16], [268435456.5, 16], [-1.2089258196146292e+24, 16], [0.029411764705882353, 17], [0.005190311418685121, 17], [0.0033584368003256666, 17], [410338672.99999994, 17], [410338673.5, 17], [-4.0642314066475725e+24, 17], [0.027777777777777776, 18], [0.004629629629629629, 18], [0.0030006858710562414, 18], [612220031.9999999, 18], [612220032.5, 18], [-1.2748236216396078e+25, 18], [0.02631578947368421, 19], [0.004155124653739612, 19], [0.002697186178743257, 19], [893871738.9999999, 19], [893871739.5, 19], [-3.758997345754596e+25, 19], [0.025, 20], [0.00375, 20], [0.0024375, 20], [1279999999.9999998, 20], [1280000000.5, 20], [-1.048576e+26, 20], [0.023809523809523808, 21], [0.003401360544217687, 21], [0.0022135838462369077, 21], [1801088540.9999998, 21], [1801088541.5, 21], [-2.7821842944695156e+26, 21], [0.022727272727272728, 22], [0.0030991735537190084, 22], [0.0020191585274229903, 22], [2494357887.9999995, 22], [2494357888.5, 22], [-7.05429498686404e+26, 22], [0.021739130434782608, 23], [0.002835538752362949, 23], [0.0018492644037149666, 23], [3404825446.9999995, 23], [3404825447.5, 23], [-1.7161558313345862e+27, 23], [0.020833333333333332, 24], [0.0026041666666666665, 24], [0.0016999421296296296, 24], [4586471423.999999, 24], [4586471424.5, 24], [-4.0199887178406037e+27, 24], [0.02, 25], [0.0024, 25], [0.001568, 25], [6103515624.999999, 25], [6103515625.5, 25], [-9.094947017729283e+27, 25], [0.019230769230769232, 26], [0.0022189349112426036, 26], [0.0014508420573509332, 26], [8031810175.999999, 26], [8031810176.5, 26], [-1.992814889520941e+28, 26], [0.018518518518518517, 27], [0.00205761316872428, 27], [0.0013463394807702077, 27], [10460353202.999998, 27], [10460353203.5, 27], [-4.23911582752162e+28, 27], [0.017857142857142856, 28], [0.001913265306122449, 28], [0.0012527332361516035, 28], [13492928511.999998, 28], [13492928512.5, 28], [-8.773252460082343e+28, 28], [0.017241379310344827, 29], [0.0017835909631391202, 29], [0.0011685595965394235, 29], [17249876308.999996, 29], [17249876309.5, 29], [-1.7699457615110974e+29, 29], [0.016666666666666666, 30], [0.0016666666666666668, 30], [0.0010925925925925925, 30], [21869999999.999996, 30], [21870000000.5, 30], [-3.486784401e+29, 30], [0.016129032258064516, 31], [0.0015608740894901144, 31], [0.0010237991339666342, 31], [27512614110.999996, 31], [27512614111.5, 31], [-6.717905288190824e+29, 31], [0.015625, 32], [0.00146484375, 32], [0.0009613037109375, 32], [34359738367.999996, 32], [34359738368.5, 32], [-1.2676506002282294e+30, 32], [0.015151515151515152, 33], [0.0013774104683195593, 33], [0.0009043604084926399, 33], [42618442976.99999, 33], [42618442977.5, 33], [-2.3457341881036794e+30, 33], [0.014705882352941176, 34], [0.0012975778546712802, 34], [0.0008523305515978017, 34], [52523350143.99999, 34], [52523350144.5, 34], [-4.261655511456885e+30, 34], [0.014285714285714285, 35], [0.0012244897959183673, 35], [0.0008046647230320699, 35], [64339296874.99999, 35], [64339296875.5, 35], [-7.609583501588059e+30, 35], [0.013888888888888888, 36], [0.0011574074074074073, 36], [0.0007608882030178326, 36], [78364164095.99998, 36], [78364164096.5, 36], [-1.3367494538843734e+31, 36]].map(([x, r]) => x.toString(r))
--- value
["0.01","0.011","0.0011","1111111.1111111111111111111111111111111111111111111111","10000000.1","-100000000000000000001","0.01111111111111111111111111111111112","0.01111111111111111111111111111111112","0.002111111111111111111111111111111112","2222222.22222222222222222222222222","10000000.11111111111111111111111112","-100000000000000000001","0.02","0.012","0.0032","3333333.33333333333333333332","10000000.2","-100000000000000000001","0.0222222222222222222222222","0.0122222222222222222222222","0.0042222222222222222222222","4444444.4444444444444443","10000000.2222222222222222","-100000000000000000001","0.03","0.013","0.00525555555555555555553","5555555.5555555555555","10000000.3","-100000000000000000001","0.03333333333333333334","0.013333333333333333334","0.006333333333333333334","6666666.666666666665","10000000.333333333334","-66666666666666666660","0.04","0.014","0.0074","7777777.77777777776","10000000.4","-100000000000000000000","0.044444444444444444","0.0144444444444444444","0.0084444444444444444","8888888.8888888886","10000000.4444444444","-88888888888888888000","0.05","0.015","0.0095","9999999.999999998","10000000.5","-100000000000000000000","0.05555555555555556","0.015555555555555556","0.00a555555555555556","aaaaaaa.aaaaaaaa","10000000.555555556","-aaaaaaaaaaaaaaaa0000","0.06","0.016","0.00b6","bbbbbbb.bbbbbbb9","10000000.6","-100000000000000000000","0.0666666666666666","0.0166666666666666","0.00c66666666666666","ccccccc.ccccccc7","10000000.66666666","-100000000000000300000","0.07","0.017","0.00d7000000000001","ddddddd.ddddddc","10000000.7","-dddddddddddddc000000","0.077777777777778","0.0177777777777778","0.00e7777777777778","eeeeeee.eeeeeea","10000000.7777778","-eeeeeeeeeeeeda000000","0.08","0.018","0.00f8","fffffff.ffffff8","10000000.8","-100000000000000000000","0.088888888888888","0.018888888888886","0.00g888888888888","ggggggg.gggggg","10000000.888888","-ggggggggggggg0000000","0.09","0.019","0.00h9","hhhhhhh.hhhhhe","10000000.9","-100000000000000000000","0.0999999999999a","0.0199999999999a","0.00i99999999999a","iiiiiii.iiiiid","10000000.99999a","-iiiiiiiiiiii50000000","0.0a","0.01a","0.00ja","jjjjjjj.jjjjj","10000000.a","-100000000000000000000","0.0aaaaaaaaaaaaa","0.01aaaaaaaaaaaa","0.00kaaaaaaaaaaa","kkkkkkk.kkkkk","10000000.aaaaaa","-kkkkkkkkkkkjg0000000","0.0b","0.01b","0.00lb0000000002","lllllll.llllk","10000000.b","-100000000000000000000","0.0bbbbbbbbbbbc","0.01bbbbbbbbbbbc","0.00mbbbbbbbbbb8","mmmmmmm.mmmmk","10000000.bbbbc","-mmmmmmmmmmli00000000","0.0c","0.01c","0.00nc","nnnnnnn.nnnng","10000000.c","-100000000000000000000","0.0cccccccccccc","0.01cccccccccccc","0.00occccccccccc","ooooooo.oooog","10000000.ccccc","-ooooooooooog00000000","0.0d","0.01d","0.00pd000000000d","ppppppp.ppppf","10000000.d","-100000000000000000000","0.0ddddddddddde","0.01dddddddddde","0.00qdddddddddd","qqqqqqq.qqqq","10000000.ddde","-qqqqqqqqqqqa00000000","0.0e","0.01e","0.00re","rrrrrrr.rrrr","10000000.e","-rrrrrrrrrrqo00000000","0.0eeeeeeeeeee","0.01eeeeeeeeeee","0.00seeeeeeeeee","sssssss.sssq","10000000.eeee","-ssssssssssq000000000","0.0f","0.01f","0.00tetttttttts","ttttttt.tttr","10000000.f","-tttttttttts000000000","0.0ffffffffffg","0.01ffffffffffg","0.00ufffffffffd","uuuuuuu.uuur","10000000.fffg","-uuuuuuuuuus000000000","0.0g","0.01g","0.00vg","vvvvvvv.vvvs","10000000.g","-100000000000000000000","0.0ggggggggggg","0.01ggggggggggg","0.00wgggggggggg","wwwwwww.wwwo","10000000.gggg","-100000000007000000000","0.0h","0.01gxxxxxxxxxj","0.00xh","xxxxxxx.xxxo","10000000.h","-100000000000000000000","0.0hhhhhhhhhhi","0.01hhhhhhhhhhi","0.00yhhhhhhhhhi","yyyyyyy.yyyo","10000000.hhhi","-100000000005000000000","0.0i","0.01i","0.00zi","zzzzzzz.zzz","10000000.i","-100000000000000000000"]
=== toString reads only its first argument
[(255).toString(16, 2), (255).toString(undefined, 2), (-0.5).toString(2, "x")]
--- value
["ff","255","-0.1"]

=== String of integers at the edges of the integer digits
[0, -0, 1, 9, 10, 99, 100, 4294967295, 4294967296, 4294967297, -1, -4294967295, -4294967296, 2**53, 2**53 + 2, 1e21, 1e20, 123456789012345680000, 4294967295.5, 0.5].map(x => String(x))
--- value
["0","0","1","9","10","99","100","4294967295","4294967296","4294967297","-1","-4294967295","-4294967296","9007199254740992","9007199254740994","1e+21","100000000000000000000","123456789012345680000","4294967295.5","0.5"]

# Integers formatted where sjs_number_format writes them, at the edges of
# its path for integers below 2^32; values from Node 22.

=== integers at the edges of 2^31, 2^32 and 2^53, and others, by String
[String(0), String(-0), String(1), String(-1), String(9), String(10), String(99), String(100), String(2147483647), String(2147483648), String(-2147483648), String(-2147483649), String(4294967294), String(4294967295), String(4294967296), String(4294967297), String(-4294967295), String(-4294967296), String(4294967295.5), String(4294967294.5), String(0.5), String(1e-7), String(123456789), String(1e9), String(1e10), String(9007199254740991), String(9007199254740992), String(9007199254740993), String(1e21), String(1e20), String(999999999999999999999), String(2**31), String(2**32-1), String(2**32), String(2**53), String(-(2**53)), String(5e-324), String(Number.MAX_VALUE), String(4294967295.0000005), String(4294967295.9999995), String(1.0000000000000002), String(0.1+0.2), String(NaN), String(Infinity), String(-Infinity)]
--- value
["0","0","1","-1","9","10","99","100","2147483647","2147483648","-2147483648","-2147483649","4294967294","4294967295","4294967296","4294967297","-4294967295","-4294967296","4294967295.5","4294967294.5","0.5","1e-7","123456789","1000000000","10000000000","9007199254740991","9007199254740992","9007199254740992","1e+21","100000000000000000000","1e+21","2147483648","4294967295","4294967296","9007199254740992","-9007199254740992","5e-324","1.7976931348623157e+308","4294967295.0000005","4294967295.9999995","1.0000000000000002","0.30000000000000004","NaN","Infinity","-Infinity"]

=== the same numbers in a template
[`${0}`, `${-0}`, `${1}`, `${-1}`, `${9}`, `${10}`, `${99}`, `${100}`, `${2147483647}`, `${2147483648}`, `${-2147483648}`, `${-2147483649}`, `${4294967294}`, `${4294967295}`, `${4294967296}`, `${4294967297}`, `${-4294967295}`, `${-4294967296}`, `${4294967295.5}`, `${4294967294.5}`, `${0.5}`, `${1e-7}`, `${123456789}`, `${1e9}`, `${1e10}`, `${9007199254740991}`, `${9007199254740992}`, `${9007199254740993}`, `${1e21}`, `${1e20}`, `${999999999999999999999}`, `${2**31}`, `${2**32-1}`, `${2**32}`, `${2**53}`, `${-(2**53)}`, `${5e-324}`, `${Number.MAX_VALUE}`, `${4294967295.0000005}`, `${4294967295.9999995}`, `${1.0000000000000002}`, `${0.1+0.2}`, `${NaN}`, `${Infinity}`, `${-Infinity}`]
--- value
["0","0","1","-1","9","10","99","100","2147483647","2147483648","-2147483648","-2147483649","4294967294","4294967295","4294967296","4294967297","-4294967295","-4294967296","4294967295.5","4294967294.5","0.5","1e-7","123456789","1000000000","10000000000","9007199254740991","9007199254740992","9007199254740992","1e+21","100000000000000000000","1e+21","2147483648","4294967295","4294967296","9007199254740992","-9007199254740992","5e-324","1.7976931348623157e+308","4294967295.0000005","4294967295.9999995","1.0000000000000002","0.30000000000000004","NaN","Infinity","-Infinity"]

=== the same numbers as computed keys
[Object.keys({ [0]: 1 })[0], Object.keys({ [-0]: 1 })[0], Object.keys({ [1]: 1 })[0], Object.keys({ [-1]: 1 })[0], Object.keys({ [9]: 1 })[0], Object.keys({ [10]: 1 })[0], Object.keys({ [99]: 1 })[0], Object.keys({ [100]: 1 })[0], Object.keys({ [2147483647]: 1 })[0], Object.keys({ [2147483648]: 1 })[0], Object.keys({ [-2147483648]: 1 })[0], Object.keys({ [-2147483649]: 1 })[0], Object.keys({ [4294967294]: 1 })[0], Object.keys({ [4294967295]: 1 })[0], Object.keys({ [4294967296]: 1 })[0], Object.keys({ [4294967297]: 1 })[0], Object.keys({ [-4294967295]: 1 })[0], Object.keys({ [-4294967296]: 1 })[0], Object.keys({ [4294967295.5]: 1 })[0], Object.keys({ [4294967294.5]: 1 })[0], Object.keys({ [0.5]: 1 })[0], Object.keys({ [1e-7]: 1 })[0], Object.keys({ [123456789]: 1 })[0], Object.keys({ [1e9]: 1 })[0], Object.keys({ [1e10]: 1 })[0], Object.keys({ [9007199254740991]: 1 })[0], Object.keys({ [9007199254740992]: 1 })[0], Object.keys({ [9007199254740993]: 1 })[0], Object.keys({ [1e21]: 1 })[0], Object.keys({ [1e20]: 1 })[0], Object.keys({ [999999999999999999999]: 1 })[0], Object.keys({ [2**31]: 1 })[0], Object.keys({ [2**32-1]: 1 })[0], Object.keys({ [2**32]: 1 })[0], Object.keys({ [2**53]: 1 })[0], Object.keys({ [-(2**53)]: 1 })[0], Object.keys({ [5e-324]: 1 })[0], Object.keys({ [Number.MAX_VALUE]: 1 })[0], Object.keys({ [4294967295.0000005]: 1 })[0], Object.keys({ [4294967295.9999995]: 1 })[0], Object.keys({ [1.0000000000000002]: 1 })[0], Object.keys({ [0.1+0.2]: 1 })[0], Object.keys({ [NaN]: 1 })[0], Object.keys({ [Infinity]: 1 })[0], Object.keys({ [-Infinity]: 1 })[0]]
--- value
["0","0","1","-1","9","10","99","100","2147483647","2147483648","-2147483648","-2147483649","4294967294","4294967295","4294967296","4294967297","-4294967295","-4294967296","4294967295.5","4294967294.5","0.5","1e-7","123456789","1000000000","10000000000","9007199254740991","9007199254740992","9007199254740992","1e+21","100000000000000000000","1e+21","2147483648","4294967295","4294967296","9007199254740992","-9007199254740992","5e-324","1.7976931348623157e+308","4294967295.0000005","4294967295.9999995","1.0000000000000002","0.30000000000000004","NaN","Infinity","-Infinity"]

=== the same numbers joined
[[0, 1].join(), [-0, 1].join(), [1, 1].join(), [-1, 1].join(), [9, 1].join(), [10, 1].join(), [99, 1].join(), [100, 1].join(), [2147483647, 1].join(), [2147483648, 1].join(), [-2147483648, 1].join(), [-2147483649, 1].join(), [4294967294, 1].join(), [4294967295, 1].join(), [4294967296, 1].join(), [4294967297, 1].join(), [-4294967295, 1].join(), [-4294967296, 1].join(), [4294967295.5, 1].join(), [4294967294.5, 1].join(), [0.5, 1].join(), [1e-7, 1].join(), [123456789, 1].join(), [1e9, 1].join(), [1e10, 1].join(), [9007199254740991, 1].join(), [9007199254740992, 1].join(), [9007199254740993, 1].join(), [1e21, 1].join(), [1e20, 1].join(), [999999999999999999999, 1].join(), [2**31, 1].join(), [2**32-1, 1].join(), [2**32, 1].join(), [2**53, 1].join(), [-(2**53), 1].join(), [5e-324, 1].join(), [Number.MAX_VALUE, 1].join(), [4294967295.0000005, 1].join(), [4294967295.9999995, 1].join(), [1.0000000000000002, 1].join(), [0.1+0.2, 1].join(), [NaN, 1].join(), [Infinity, 1].join(), [-Infinity, 1].join()]
--- value
["0,1","0,1","1,1","-1,1","9,1","10,1","99,1","100,1","2147483647,1","2147483648,1","-2147483648,1","-2147483649,1","4294967294,1","4294967295,1","4294967296,1","4294967297,1","-4294967295,1","-4294967296,1","4294967295.5,1","4294967294.5,1","0.5,1","1e-7,1","123456789,1","1000000000,1","10000000000,1","9007199254740991,1","9007199254740992,1","9007199254740992,1","1e+21,1","100000000000000000000,1","1e+21,1","2147483648,1","4294967295,1","4294967296,1","9007199254740992,1","-9007199254740992,1","5e-324,1","1.7976931348623157e+308,1","4294967295.0000005,1","4294967295.9999995,1","1.0000000000000002,1","0.30000000000000004,1","NaN,1","Infinity,1","-Infinity,1"]

=== the same numbers written
[0, 1, -1, 9, 10, 99, 100, 2147483647, 2147483648, -2147483648, -2147483649, 4294967294, 4294967295, 4294967296, 4294967297, -4294967295, -4294967296, 4294967295.5, 4294967294.5, 0.5, 1e-7, 123456789, 1e9, 1e10, 9007199254740991, 9007199254740992, 9007199254740993, 1e21, 1e20, 999999999999999999999, 2**31, 2**32-1, 2**32, 2**53, -(2**53), 5e-324, Number.MAX_VALUE, 4294967295.0000005, 4294967295.9999995, 1.0000000000000002, 0.1+0.2]
--- value
[0,1,-1,9,10,99,100,2147483647,2147483648,-2147483648,-2147483649,4294967294,4294967295,4294967296,4294967297,-4294967295,-4294967296,4294967295.5,4294967294.5,0.5,1e-7,123456789,1000000000,10000000000,9007199254740991,9007199254740992,9007199254740992,1e+21,100000000000000000000,1e+21,2147483648,4294967295,4294967296,9007199254740992,-9007199254740992,5e-324,1.7976931348623157e+308,4294967295.0000005,4294967295.9999995,1.0000000000000002,0.30000000000000004]

=== numbers about 2^32 as keys, written by JSON.stringify
JSON.stringify({ [4294967295]: 1, [4294967296]: 2, [1]: 3, [2**53]: 4, [1e21]: 5, [0.5]: 6 })
--- value
"{\"1\":3,\"4294967295\":1,\"4294967296\":2,\"9007199254740992\":4,\"1e+21\":5,\"0.5\":6}"
