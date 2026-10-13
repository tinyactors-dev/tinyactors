# Regular expressions: edges an adversarial review checked (docs/regex.md).
# Every expected value was generated in Node 22.

# ---------------------------------------------------------------------------
# Long inputs: each match pauses on fuel and resumes with the same answer.

=== a nested quantifier on a long input
/(a+)+b/.exec("a".repeat(5000) + "b")[1].length
--- value
5000

=== two alternatives of one letter under a star, on a long input
/(a|a)*b/.exec("a".repeat(5000) + "b")[1]
--- value
"a"

=== alternatives under a plus, anchored, on a long input
/^(a|b)+$/.test("ab".repeat(2500))
--- value
true

=== a star of a star, with its capture, on a long input
/(a*)*b/.exec("a".repeat(5000) + "b")[1].length
--- value
5000

=== twenty copies of .*a on a long input
/(.*a){20}/.exec("a".repeat(5000)).index
--- value
0

=== replace with g on a long input
"a".repeat(5000).replace(/a/g, "bc").length
--- value
10000

=== split with a capture on a long input
"ab".repeat(3000).split(/(b)/).length
--- value
6001

=== matchAll with named groups on a long input
[..."ab".repeat(2000).matchAll(/(?<x>a)(b)?/g)].length
--- value
2000

=== replace with a function on a long input
"x".repeat(5000).replace(/x+$/, (m) => m.length)
--- value
"5000"

=== match with g and empty matches on a long input
"ab".repeat(2000).match(/a*/g).length
--- value
4001

=== search on a long input that does not match
"a".repeat(5000).search(/a*?b/)
--- value
-1

# ---------------------------------------------------------------------------
# Empty iterations and captures reset at each iteration.

=== a star of an optional group
/(a?)*/.exec("aa")
--- value
Object.assign(["aa","a"],{"index":0,"input":"aa","groups":undefined})

=== a star over an empty capture before b
/(?:a|())*b/.exec("aab")
--- value
Object.assign(["aab",undefined],{"index":0,"input":"aab","groups":undefined})

=== the first alternative that lets the rest match
/(a|ab)(c|bcd)(d*)/.exec("abcd")
--- value
Object.assign(["abcd","a","bcd",""],{"index":0,"input":"abcd","groups":undefined})

=== an inner group unset by a later iteration
/((a)|b)+/.exec("ab")
--- value
Object.assign(["ab","b",undefined],{"index":0,"input":"ab","groups":undefined})

=== two optional groups under a star
/((a?)|(b?))*/.exec("ab")
--- value
Object.assign(["ab","b",undefined,"b"],{"index":0,"input":"ab","groups":undefined})

=== three alternatives, one empty, under a star
/(?:(a)|(b)|())*c/.exec("abc")
--- value
Object.assign(["abc",undefined,"b",undefined],{"index":0,"input":"abc","groups":undefined})

=== nested groups reset in each iteration
/(z)((a+)?(b+)?(c))*/.exec("zaacbbbcac")
--- value
Object.assign(["zaacbbbcac","z","ac","a",undefined,"c"],{"index":0,"input":"zaacbbbcac","groups":undefined})

=== a lazy count of an optional group
/(a?){0,3}?b/.exec("aab")
--- value
Object.assign(["aab","a"],{"index":0,"input":"aab","groups":undefined})

=== an empty first alternative under a plus
/(|a)+/.exec("aa")
--- value
Object.assign(["aa","a"],{"index":0,"input":"aa","groups":undefined})

=== an empty capture in a count past the input
/(?:a|()){2,3}/.exec("a")
--- value
Object.assign(["a",""],{"index":0,"input":"a","groups":undefined})

=== a capture inside one alternative of a repetition
/(?:x(a?)|y)+/.exec("xayxa")
--- value
Object.assign(["xayxa","a"],{"index":0,"input":"xayxa","groups":undefined})

=== a lazy star of a lazy star
/(a*?)+/.exec("aa")
--- value
Object.assign(["aa","a"],{"index":0,"input":"aa","groups":undefined})

=== a count over a count that can match empty
/(a{0,2}){0,2}/.exec("aaaaa")
--- value
Object.assign(["aaaa","aa"],{"index":0,"input":"aaaaa","groups":undefined})

# ---------------------------------------------------------------------------
# Annex B forms that mean what the standard grammar means.

=== \c and a letter of either case
/\cA\ca/.test("\x01\x01")
--- value
true

=== a class escape and a dash at the end of a class
"- \t-x".match(/[\s-]+/)[0]
--- value
"- \t-"

=== a dash and a class escape at the start of a class
"a-1-2".match(/[-\d]+/)[0]
--- value
"-1-2"

=== an escaped slash
"a/b".split(/\//)
--- value
["a","b"]

# ---------------------------------------------------------------------------
# lastIndex: the calls that read or write none.

=== search with y matches only at 0
[ "xaba".search(/a/y), "abab".search(/a/y) ]
--- value
[-1,0]

=== replace with g and y stops where a match does not follow
"aaba".replace(/a/gy, "x")
--- value
"xxba"

=== matchAll with g and y
[..."abab".matchAll(/a(b)?/gy)].map((m) => m.index)
--- value
[0,2]

=== replaceAll with g and y
"abxab".replaceAll(/ab/gy, "-")
--- value
"-xab"

=== lastIndex after match with g
((r, s) => [s.match(r), r.lastIndex])(/a/g, "aa")
--- value
[["a","a"],0]

=== lastIndex after split and search with y
((r) => ["aba".split(r), "aba".search(r), r.lastIndex])(/a/y)
--- value
[["","b",""],0,0]

# ---------------------------------------------------------------------------
# Surrogate pairs without u: what is whole again agrees.

=== a group of half a pair put back where it was
"😀".replace(/(.)/, "$1")
--- value
"😀"

=== two halves put back in order
"😀".replace(/(.)(.)/, "$1$2")
--- value
"😀"

=== each half replaced
"😀".replace(/[^a]/g, "x")
--- value
"xx"

=== split on each half leaves empty pieces
"😀".split(/[^a]/)
--- value
["","",""]

=== a match across two pairs, removed
"😀😀".replace(/\ude00\ud83d/, "")
--- value
"😀"

=== search for the second half
"😀".search(/\ude00/)
--- value
1

=== split on any code unit
"a😀".split(/./)
--- value
["","","",""]

=== split with a limit of 0 between the halves
"😀".split(/(?:)/, 0)
--- value
[]

=== test of half a pair
/\ud83d/.test("😀")
--- value
true

# ---------------------------------------------------------------------------
# split's limit, as ToUint32 gives it.

=== 2^32 + 1 is 1
"abc".split(/b/, 4294967297)
--- value
["a"]

=== 2^32 is 0
"abc".split(/b/, 4294967296)
--- value
[]

=== -1 is 2^32 - 1
"abc".split(/b/, -1)
--- value
["a","c"]

=== NaN is 0
"abc".split(/b/, NaN)
--- value
[]

=== 1e100 is 0
"abc".split(/b/, 1e100)
--- value
[]

=== Infinity is 0
"abc".split(/b/, Infinity)
--- value
[]

=== undefined is no limit
"abc".split(/b/, undefined)
--- value
["a","c"]

=== a limit that cuts the captures
"abc".split(/(b)/, 2)
--- value
["a","b"]

# ---------------------------------------------------------------------------
# Case-insensitive edges.

=== a negated class with i and u leaves out the folds of s
"Ssſx".match(/[^s]/giu)
--- value
["x"]

=== the Kelvin sign is a word character with i and u
["\u212a".match(/[^\w]/iu), "\u212a".match(/\w/iu)[0]]
--- value
[null,"K"]

=== [A-z] holds the six characters between Z and a
"[".match(/[A-z]/i)[0]
--- value
"["

=== _ is not a letter with i
"_".match(/[a-z]/i)
--- value
null

=== a negated range with i
"Z".match(/[^a-z]/i)
--- value
null

=== an escaped letter with i
"AB".match(/\u0061\u0062/i)[0]
--- value
"AB"

# ---------------------------------------------------------------------------
# A regular expression on the next line.

=== a division at the start of a line, after a name
((a) => a
/2/1)(4)
--- value
2

=== flags, then a member on the next line
/a/i
.flags
--- value
"i"

# ---------------------------------------------------------------------------
# Match results read in more ways.

=== in on indices
["index" in /(?<x>a)(b)?(?<y>c)?/d.exec("zab").indices, "groups" in /(?<x>a)(b)?(?<y>c)?/d.exec("zab").indices, 3 in /(?<x>a)(b)?(?<y>c)?/d.exec("zab").indices, 4 in /(?<x>a)(b)?(?<y>c)?/d.exec("zab").indices]
--- value
[false,true,true,false]

=== a group of indices that did not match
/(?<x>a)(b)?(?<y>c)?/d.exec("zab").indices.groups.y
--- value
undefined

=== at, join and includes
[/(?<x>a)(b)?(?<y>c)?/d.exec("zab").at(-1), /(?<x>a)(b)?(?<y>c)?/d.exec("zab").join("|"), /(?<x>a)(b)?(?<y>c)?/d.exec("zab").includes(undefined)]
--- value
[undefined,"ab|a|b|",true]

=== toReversed and with are plain arrays
[/(?<x>a)(b)?(?<y>c)?/d.exec("zab").toReversed(), /(?<x>a)(b)?(?<y>c)?/d.exec("zab").with(0, "q")]
--- value
[[undefined,"b","a","ab"],["q","a","b",undefined]]

=== flat of match results
[/(?<x>a)(b)?(?<y>c)?/d.exec("zab"), /a/.exec("a")].flat()
--- value
["ab","a","b",undefined,"a"]

=== a match result in a map
new Map([[1, /(?<x>a)/.exec("a")]])
--- value
new Map([[1,Object.assign(["a","a"],{"index":0,"input":"a","groups":{"x":"a"}})]])

=== filter and reduce of a match result
[/(?<x>a)(b)?(?<y>c)?/d.exec("zab").filter((x) => x), /(?<x>a)(b)?(?<y>c)?/d.exec("zab").reduce((a, x) => a + x, "")]
--- value
[["ab","a","b"],"ababundefined"]

# ---------------------------------------------------------------------------
# A long test() runs from the cache of its automaton's steps (docs/regex.md,
# 15): classes of characters that could be split wrongly, more classes,
# states or instructions than the cache takes, and inputs made by methods.
# spec_test runs each as loops paused after every unit, with the cache and
# without, for the same values, fuel and pauses. Values from Node 22.

=== a long test: thirteen letters and the rest make 16 classes, fourteen 17
[/abcdefghijklm/.test("abcdefghijkl ".repeat(300) + "abcdefghijklm"), /abcdefghijklmn/.test("abcdefghijklm ".repeat(300) + "abcdefghijklmn"), /abcdefghijklm/.test("abcdefghijkl ".repeat(300)), /abcdefghijklmn/.test("abcdefghijklm ".repeat(300))]
--- value
[true,true,false,false]

=== a long test: 29 letters and the rest make 32 classes, the most the cache takes, and 30 make 33
[/abcdefghijklmnopqrstuvwxyzABC/.test("abcdefghijklmnopqrstuvwxyzAB ".repeat(60) + "abcdefghijklmnopqrstuvwxyzABC"), /abcdefghijklmnopqrstuvwxyzABCD/.test("abcdefghijklmnopqrstuvwxyzABC ".repeat(60) + "abcdefghijklmnopqrstuvwxyzABCD"), /abcdefghijklmnopqrstuvwxyzABC/.test("abcdefghijklmnopqrstuvwxyzAB ".repeat(60)), /abcdefghijklmnopqrstuvwxyzABCD/.test("abcdefghijklmnopqrstuvwxyzABC ".repeat(60))]
--- value
[true,true,false,false]

=== a long test: more than 16 classes, the character after an edge's in fewer (word, line terminator, the skip's)
[/,r(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|0)\d{4}5,/.test("r000124,".repeat(40)), /,r(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|0)\d{4}5,/.test("r000124,r000124,rq12345," + "r000124,".repeat(40)), /,r(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|0)\d{4}5,/.test("r000124,r000125," + "r000124,".repeat(40)), /,r(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|0)\d{4}5,/.test("r000125.r000125," + "r000124,".repeat(40)), /,r(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|0)\d{4}5,/.test(",r12345,".repeat(40))]
--- value
[false,true,true,false,false]

=== a long test: more than 16 classes, with \b, \B and $ with m
[/\b(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q)x\b/.test("ax_ bxa qx1 ".repeat(100)), /\b(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q)x\b/.test("ax_ -gx-" + "ax_ bxa qx1 ".repeat(100)), /(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q)z$/m.test("az1\n".repeat(300)), /(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q)z$/m.test("az1\nqz\r\n" + "az1\n".repeat(300)), /\B(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q)\B/.test(" a b- c_ ".repeat(200)), /\B(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q)\B/.test(" a b- c_ 1q2" + " a b- c_ ".repeat(200))]
--- value
[false,true,false,true,false,true]

=== a long test: case-insensitive classes
[/[a-f]+x/i.test("AbCdEf-".repeat(400) + "FX"), /K/i.test("j".repeat(2000) + "k"), /[^a-z]q/i.test("Aq".repeat(800)), /[^a-z]q/i.test("Aq".repeat(800) + "1Q")]
--- value
[true,true,false,true]

=== a long test: \s, \d and \w against their neighbours
[/\s\s\s/.test("a\t\va\f\r".repeat(400)), /\s\s\s/.test("a\t\va\f\r".repeat(400) + "\n \v"), /\d+\.\d/.test("12.a".repeat(500)), /\d+\.\d/.test("12.a".repeat(500) + "3.4"), /\w\W\w/.test("ab".repeat(900) + "_-9")]
--- value
[false,true,false,true,true]

=== a long test: \b and \B where a class ends
[/\bfoo\b/.test("foox foo_ xfoo ".repeat(200)), /\bfoo\b/.test("foox foo_ xfoo ".repeat(200) + "foo"), /\Bo\B/.test("o o oa ao ".repeat(200)), /\Bo\B/.test("o o oa ao ".repeat(200) + "aoa"), /x\b-/.test("x_-".repeat(600))]
--- value
[false,true,false,true,false]

=== a long test: ^ and $ with m, and \r\n
[/^b$/m.test("a\r\n".repeat(500) + "b\r\nc"), /^b$/m.test("ab\r\n".repeat(500) + "bc"), /^$/m.test("a\r".repeat(700)), /^$/m.test("a\r".repeat(700) + "\r"), /a$/m.test("ab\n".repeat(600)), /a$/.test("a\n".repeat(600)), /^b/.test("\nb".repeat(600))]
--- value
[true,false,true,true,false,false,false]

=== a long test: . with and without s
[/x.y/.test("x\ny".repeat(500)), /x.y/s.test("x\ny".repeat(500)), /x.y/.test("x\ry x\u2028y".repeat(300)), /a.*b/.test("a\n".repeat(600) + "b")]
--- value
[false,true,false,false]

=== a long test: loops that can match empty
[/(?:a*)*c/.test("a".repeat(1500) + "c"), /(?:|a)+b/.test("a".repeat(1500) + "b"), /(?:\b)*a/.test(" ".repeat(1500) + "a"), /(?:x*)*y/.test("x".repeat(1500) + "y"), /(?:a*|b)*c/.test("ab".repeat(700) + "c")]
--- value
[true,true,true,true,true]

=== a long test: alternatives whose threads meet
[/(?:a|ab)(?:c|bcd)d/.test("abc".repeat(500) + "abcdd"), /(?:a|ab)(?:c|bcd)d/.test("abc".repeat(500) + "abd"), /(?:a|b|ab)+c/.test("ab".repeat(800) + "c"), /(?:aa|a)+b$/.test("a".repeat(1500) + "b")]
--- value
[true,false,true,true]

=== a long test whose automaton has more states than the cache holds, 4 after the a
[/(a|b)*a(a|b){4}c/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join("")), /(a|b)*a(a|b){4}c/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join("") + "c"), /(?:a|b)*a(?:a|b){4}$/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join(""))]
--- value
[false,true,true]

=== a long test whose automaton has more states than the cache holds, 8 after the a
[/(a|b)*a(a|b){8}c/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join("")), /(a|b)*a(a|b){8}c/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join("") + "c"), /(?:a|b)*a(?:a|b){8}$/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join(""))]
--- value
[false,false,false]

=== a long test whose automaton has more states than the cache holds, 12 after the a
[/(a|b)*a(a|b){12}c/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join("")), /(a|b)*a(a|b){12}c/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join("") + "c"), /(?:a|b)*a(?:a|b){12}$/.test(Array.from({ length: 1200 }, (_, i) => (i * i * 7 + i * 3) % 11 < 5 ? "a" : "b").join(""))]
--- value
[false,true,true]

=== a long test: x{252}y is 256 instructions, the most the cache takes, and x{254}y is 258
[/x{248}y/.test("x".repeat(247) + "y" + "x".repeat(248) + "y"), /x{250}y/.test("x".repeat(249) + "y" + "x".repeat(250) + "y"), /x{252}y/.test("x".repeat(251) + "y" + "x".repeat(252) + "y"), /x{254}y/.test("x".repeat(253) + "y" + "x".repeat(254) + "y"), /x{256}y/.test("x".repeat(255) + "y" + "x".repeat(256) + "y"), /x{258}y/.test("x".repeat(257) + "y" + "x".repeat(258) + "y"), /x{260}y/.test("x".repeat(259) + "y" + "x".repeat(260) + "y")]
--- value
[true,true,true,true,true,true,true]

=== a long test on strings made by methods, ASCII or not
[/b+c$/.test("é".slice(1) + "ab".repeat(400) + "c"), /[^a-b]c/.test("ab".repeat(400) + "é".slice(0, 0) + "c"), /\W/.test(decodeURIComponent("%C3%A9" + "ab".repeat(300))), /.c/.test("ab".repeat(300).padEnd(1000, "éc")), /\w*c/.test("ab".repeat(300) + "\u{1F600}" + "c"), /^.*c$/.test(["é", "ab".repeat(400), "c"].join("").slice(1))]
--- value
[true,false,true,true,true,true]

=== a long test with u and i: the characters that fold to k and s are not ASCII
[/k/iu.test("j".repeat(1500) + "\u212A"), /\bk/iu.test("_".repeat(1500) + "K"), /[^k]/iu.test("kK".repeat(800)), /s/iu.test("S".repeat(5) + "x".repeat(1500))]
--- value
[true,false,false,true]

=== a regular expression tested on many long inputs
(() => { const p = /(a|b)*a(a|b){3}c/; const xs = ["ab".repeat(300) + "aabac", "ba".repeat(300), "c" + "a".repeat(500), "aaaac"]; return [xs.map((s) => p.test(s)), xs.filter((s) => p.test(s)).length, p.test(xs[0]), p.test(xs[1])]; })()
--- value
[[true,false,false,true],2,true,false]
