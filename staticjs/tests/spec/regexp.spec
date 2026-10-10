# Regular expressions (docs/regex.md): literals, their values, and what is
# refused when compiled. Every expected value was generated in Node 22.

# ---------------------------------------------------------------------------
# Literals as values: source, flags, the flags' getters, lastIndex.

=== a literal is written as its source and flags
/ab+c/
--- value
/ab+c/

=== flags are written in the order RegExp.prototype.flags gives them
/a/yusmigd
--- value
/a/dgimsuy

=== the source as written, escapes and a slash in a class kept
/a\/b[/]\d/.source
--- value
"a\\/b[/]\\d"

=== the flags in their order
/a/yusmigd.flags
--- value
"dgimsuy"

=== no flags
/a/.flags
--- value
""

=== the flags' getters
[/a/g.global, /a/i.ignoreCase, /a/m.multiline, /a/s.dotAll, /a/u.unicode, /a/y.sticky, /a/d.hasIndices, /a/.unicodeSets]
--- value
[true,true,true,true,true,true,true,false]

=== the flags' getters, without the flags
[/a/.global, /a/.ignoreCase, /a/.multiline, /a/.dotAll, /a/.unicode, /a/.sticky, /a/.hasIndices]
--- value
[false,false,false,false,false,false,false]

=== lastIndex is 0
[/a/.lastIndex, /a/g.lastIndex, /a/y.lastIndex]
--- value
[0,0,0]

=== another property is undefined
/a/.x
--- value
undefined

=== typeof a regular expression
typeof /a/
--- value
"object"

=== JSON.stringify of a regular expression
JSON.stringify(/a/g)
--- value
"{}"

=== JSON.stringify of a regular expression in an object
JSON.stringify({ r: /a/, n: 1 })
--- value
"{\"r\":{},\"n\":1}"

=== JSON.stringify with a list of keys a regular expression lacks
JSON.stringify(/a/, ["x"])
--- value
"{}"

=== spreading a regular expression into an object
({ .../a/g })
--- value
{}

=== a property destructured from a regular expression
(({ source, flags }) => source + flags)(/a.b/gi)
--- value
"a.bgi"

=== the empty class and its negation
[/[]/.source, /[^]/.source]
--- value
["[]","[^]"]

=== a regular expression in an array and an object
[/a/, { r: /b/m }]
--- value
[/a/,{"r":/b/m}]

=== text that is not ASCII in a literal
/é😀[ü]/u.source
--- value
"é😀[ü]"

=== a regular expression after an operator
(x => x ? /a/.source : /b/.source)(true)
--- value
"a"

=== a regular expression after ! and typeof
[!/a/, typeof /a/]
--- value
[false,"object"]

=== a regular expression as an arrow function's body
(() => /a/g)().flags
--- value
"g"

=== a regular expression as an argument
((a, b) => a.source + b.source)(/x/, /y/)
--- value
"xy"

=== a regular expression in a template
`${/a+/.source}!`
--- value
"a+!"

=== a regular expression that begins with =, read from the token /=
/=a/.source
--- value
"=a"

=== a regular expression that begins with =, after an operator
(x => x || /==/.source)(false)
--- value
"=="

=== a regular expression after a return
(() => { return /r/.source; })()
--- value
"r"

=== a regular expression after a block statement in a function
(() => { { } return /b/.source; })()
--- value
"b"

=== a regular expression after an if's condition in a function
(x => { if (x) return /i/.source; return /e/.source; })(true)
--- value
"i"

=== a regular expression in a default value, after a division, read ahead
((a = 6 / 3, b = /x/) => a + b.source)()
--- value
"2x"

=== a regular expression in a computed key's default, read ahead
(({ [/k/.source]: v = /d/ }) => v.source)({})
--- value
"d"

=== a division after a regular expression, read ahead
((a = /x/.source.length / 2) => a)()
--- value
0.5

=== a division is not a regular expression
((a, b, c) => a / b / c)(8, 2, 2)
--- value
2

=== a division after a closing parenthesis
((a, b) => (a) / b / 1)(8, 2)
--- value
4

=== a member of a regular expression on the next line
(() => { const r = /m/
.source; return r; })()
--- value
"m"

# ---------------------------------------------------------------------------
# Faults

=== a method read as a value faults
/a/.exec
--- fault does not read them as values

=== toString read as a value faults
/a/.toString
--- fault does not read them as values

=== === on two regular expressions faults
/a/ === /a/
--- fault identity

=== a regular expression into a string faults, where JavaScript writes its source
"" + /a/
--- fault a string and a primitive

=== a template of a regular expression faults, where JavaScript writes its source
`${/a/}`
--- fault only primitives

=== Object.keys of a regular expression faults, where JavaScript gives []
Object.keys(/a/)
--- fault Object.keys

=== in on a regular expression faults
"lastIndex" in /a/
--- fault in needs

=== JSON.stringify with a list holding a flag's getter faults, where JavaScript writes it
JSON.stringify(/a/, ["source"])
--- fault prototype

=== JSON.stringify with a list holding lastIndex faults, where JavaScript writes it
JSON.stringify(/a/, ["lastIndex"])
--- fault prototype

=== a regular expression is not a set member
new Set([/a/])
--- fault primitives

# ---------------------------------------------------------------------------
# Refused when compiled: syntax errors, as JavaScript refuses them too

=== an unterminated literal
/abc
--- error syntax regular expression

=== an unterminated class
/[a/
--- error syntax regular expression

=== a line break in a literal
/a
/
--- error syntax lines

=== a line separator in a literal
/a /
--- error syntax lines

=== a flag that is not one
/a/x
--- error syntax flags

=== a flag written twice
/a/gg
--- error syntax flags

=== u and v together
/a/uv
--- error syntax flags

=== an escape in the flags
/a/\u0067
--- error syntax flags

=== a letter that is not ASCII after the flags
/a/gé
--- error syntax flags

=== a quantifier with nothing to repeat, after |
/a|*b/
--- error syntax Nothing to repeat

=== a braced quantifier with nothing to repeat
/{1}/
--- error syntax Nothing to repeat

=== a quantifier after a quantifier
/x{1}{2}/
--- error syntax Nothing to repeat

=== a star after a star
/a**/
--- error syntax Nothing to repeat

=== a quantifier after ^
/^*/
--- error syntax Nothing to repeat

=== a quantifier after \b
/\b+/
--- error syntax Nothing to repeat

=== a quantified lookbehind
/(?<=a)*/
--- error syntax Nothing to repeat

=== a quantified lookahead with u
/(?=a)*/u
--- error syntax Nothing to repeat

=== numbers out of order in a quantifier
/a{2,1}/
--- error syntax out of order

=== numbers out of order past V8's largest count
/a{4294967296,1}/
--- error syntax out of order

=== a range out of order
/[z-a]/
--- error syntax out of order

=== a range of the halves of a pair out of order, without u
/[😀-😁]/
--- error syntax out of order

=== an identity escape with u
/\-/u
--- error syntax Invalid escape

=== a letter escaped with u
/\a/u
--- error syntax Invalid escape

=== a reference to a group that does not exist, with u
/\1/u
--- error syntax Invalid escape

=== a legacy octal escape with u
/\00/u
--- error syntax decimal

=== \x without two digits, with u
/\x1/u
--- error syntax Invalid escape

=== \u{} past U+10FFFF
/\u{110000}/u
--- error syntax Unicode

=== \c without a letter, with u
/\c1/u
--- error syntax Unicode

=== a property that does not exist
/\p{Foo}/u
--- error syntax property

=== a property without braces
/\pL/u
--- error syntax property

=== a script name alone is not a property
/\p{Latin}/u
--- error syntax property

=== a property of strings with u
/\p{RGI_Emoji}/u
--- error syntax property

=== a property of strings negated, with v
/\P{RGI_Emoji}/v
--- error syntax property

=== \k without a name, with u
/\k/u
--- error syntax named reference

=== \k naming no group, with u
/\k<a>/u
--- error syntax named

=== \k naming no group, where a group has a name
/(?<a>x)\k<b>/
--- error syntax named

=== \k without a name, where a group has a name
/(?<a>x)\k/
--- error syntax named reference

=== two groups of one name
/(?<a>x)(?<a>y)/
--- error syntax Duplicate

=== two groups of one name in alternatives, which Node 22 refuses
/(?<a>x)|(?<a>y)/
--- error syntax Duplicate

=== a modifier, which Node 22 refuses
/(?i:a)/
--- error syntax Invalid group

=== a group of no known kind
/(?a)/
--- error syntax Invalid group

=== a group name that begins with a digit
/(?<1a>x)/
--- error syntax group name

=== an empty group name
/(?<>x)/
--- error syntax group name

=== an unmatched )
/a)/
--- error syntax Unmatched

=== an unterminated group
/(a/
--- error syntax Unterminated group

=== a lone ] with u
/]/u
--- error syntax Lone

=== a lone { with u
/{/u
--- error syntax Lone

=== an incomplete quantifier with u
/a{1/u
--- error syntax Incomplete

=== a class escape at a range's end, with u
/[\d-z]/u
--- error syntax Invalid character class

=== \B in a class, with u
/[\B]/u
--- error syntax Invalid escape

=== \k in a class, where a group has a name
/[\k](?<a>x)/
--- error syntax Invalid escape

=== a syntax character alone in a class, with v
/[(]/v
--- error syntax character class

=== three & in a class, with v
/[a&&&b]/v
--- error syntax character class

=== a range in an intersection, with v
/[a-z&&b]/v
--- error syntax set operation

=== a negated class that may hold strings, with v
/[^\q{ab}]/v
--- error syntax strings

=== a syntax error after an unsupported construct is still one
/(?=a)(/
--- error syntax Unterminated group

=== a syntax error in a later literal comes before an earlier refusal
[/(?=a)/, /(/]
--- error syntax Unterminated group

# ---------------------------------------------------------------------------
# Refused when compiled: JavaScript that staticjs leaves out

=== a backreference
/(a)\1/
--- error unsupported backreferences

=== a backreference with u
/(a)\1/u
--- error unsupported backreferences

=== a named backreference
/(?<n>a)\k<n>/
--- error unsupported backreferences

=== a named backreference before its group
/\k<n>(?<n>a)/
--- error unsupported backreferences

=== a lookahead
/a(?=b)/
--- error unsupported lookahead

=== a negative lookahead
/a(?!b)/
--- error unsupported lookahead

=== a lookbehind
/(?<=a)b/
--- error unsupported lookbehind

=== a negative lookbehind
/(?<!a)b/
--- error unsupported lookbehind

=== a quantified lookahead (Annex B)
/(?=a)*/
--- error unsupported lookahead

=== a property escape
/\p{L}/u
--- error unsupported Unicode property

=== a negated property escape of a script
/\P{Script=Greek}/u
--- error unsupported Unicode property

=== a property escape of a category
/\p{gc=Lu}/u
--- error unsupported Unicode property

=== a property escape in a class
/[\p{N}a]/u
--- error unsupported Unicode property

=== the v flag
/a/v
--- error unsupported v flag

=== a set operation with v
/[\p{L}--a]/v
--- error unsupported v flag

=== a letter that is not ASCII, with i
/é/i
--- error unsupported ASCII

=== a range that is not ASCII, with i
/[à-ÿ]/i
--- error unsupported ASCII

=== an escape of a letter that is not ASCII, with i
/\u00e9/i
--- error unsupported ASCII

=== a decimal escape without a group, a legacy octal escape (Annex B)
/\1/
--- error unsupported octal

=== \8 (Annex B)
/\8/
--- error unsupported octal

=== \0 and a digit (Annex B)
/\08/
--- error unsupported octal

=== a lone ] (Annex B)
/]/
--- error unsupported lone

=== a lone { (Annex B)
/a{/
--- error unsupported quantifier

=== a lone } (Annex B)
/}/
--- error unsupported lone

=== {,n} is no quantifier (Annex B)
/a{,5}/
--- error unsupported quantifier

=== \c and a digit (Annex B)
/\c1/
--- error unsupported \c

=== \c alone (Annex B)
/\c/
--- error unsupported \c

=== \c and _ in a class (Annex B)
/[\c_]/
--- error unsupported \c

=== an identity escape of - (Annex B)
/\-/
--- error unsupported identity

=== an identity escape of a letter (Annex B)
/\a/
--- error unsupported identity

=== \k where no group has a name (Annex B)
/\k<a>/
--- error unsupported \k

=== \u{41} without u is u forty-one times (Annex B)
/\u{41}/
--- error unsupported \u

=== \p{L} without u is the text p{L} (Annex B)
/\p{L}/
--- error unsupported identity

=== a class escape at a range's end (Annex B)
/[\d-z]/
--- error unsupported class escape

=== \x without two digits (Annex B)
/\x4/
--- error unsupported \x

=== \u without four digits (Annex B)
/\u12/
--- error unsupported \u

=== \B in a class (Annex B)
/[\B]/
--- error unsupported identity

=== a group name that is not ASCII
/(?<é>a)/
--- error unsupported group names

=== a group name with an escape
/(?<a\u0062>x)/
--- error unsupported group names

=== new RegExp
new RegExp("a")
--- error unsupported Set and Map

=== RegExp called
RegExp("a")
--- error unsupported RegExp

# ---------------------------------------------------------------------------
# Limits

=== a quantifier that copies past max_regex_states
/a{1000000}/
--- error limit max_regex_states

=== a group under a quantifier that copies past max_regex_states
/(a?){300000}/
--- error limit max_regex_states

# ---------------------------------------------------------------------------
# test, search, match with g, split, replace and replaceAll

=== test
[/ab+c/.test("xxabbbcx"), /ab+c/.test("ac"), /^$/.test(""), /a|b/.test("c")]
--- value
[true,false,true,false]

=== anchors, with and without m
[/^b/.test("a\nb"), /^b/m.test("a\nb"), /a$/.test("a\nb"), /a$/m.test("a\nb"), /a$/m.test("a\rb"), /a$/m.test("a b")]
--- value
[false,true,false,true,true,true]

=== the dot, with and without s
[/a.b/.test("a\nb"), /a.b/s.test("a\nb"), /a.b/.test("a\rb"), /a.b/.test("a b"), /^.$/.test("é")]
--- value
[false,true,false,false,true]

=== word boundaries
["a b".replace(/\b/g, "|"), "ab cd".replace(/\B/g, "|"), "_é".replace(/\b/g, "|")]
--- value
["|a| |b|","a|b c|d","|_|é"]

=== class escapes
["a1 _-é\t".replace(/\d/g, "D"), "a1 _-é\t".replace(/\w/g, "W"), "a1 _-é\t".replace(/\s/g, "S"), "a1 _-é\t".replace(/[\D]/g, "N")]
--- value
["aD _-é\t","WW W-é\t","a1S_-éS","N1NNNNN"]

=== white space is what trim removes
"\t\v\f \u00a0\ufeff\u1680\u2000\u200a\u2028\u2029\u202f\u205f\u3000x\u180e".replace(/\s/g, "")
--- value
"x᠎"

=== classes, ranges and negation
["a-z]B".replace(/[a-z]/g, "."), "a-z]B".replace(/[^a-z]/g, "."), "a-z]B".replace(/[-a]/g, "."), "a]b".replace(/[\]]/g, ".")]
--- value
[".-.]B","a.z..","..z]B","a.b"]

=== the empty class and its negation match nothing and anything
["ab".replace(/[]/g, "."), "a\nb".replace(/[^]/g, ".")]
--- value
["ab","..."]

=== escapes of characters
["\0\n\t\v\f\r".replace(/[\0][\n][\t][\v][\f][\r]/, "ok"), "\x01".replace(/\cA/, "ok"), "\b".replace(/[\b]/, "ok"), "a".replace(/\x61/, "ok"), "a".replace(/a/, "ok")]
--- value
["ok","ok","ok","ok","ok"]

=== syntax characters escaped
"^$\\.*+?()[]{}|/".replace(/\^\$\\\.\*\+\?\(\)\[\]\{\}\|\//, "ok")
--- value
"ok"

=== quantifiers, greedy and lazy
["aaaa".replace(/a{2,3}/, "<$&>"), "aaaa".replace(/a{2,3}?/, "<$&>"), "aaaa".replace(/a*?/, "<$&>"), "aaaa".replace(/a+?/, "<$&>"), "aaaa".replace(/a??/, "<$&>"), "aaaa".replace(/a{2,}/, "<$&>"), "aaaa".replace(/a{0}/, "<$&>")]
--- value
["<aaa>a","<aa>aa","<>aaaa","<a>aaa","<>aaaa","<aaaa>","<>aaaa"]

=== alternatives, in order
["abc".replace(/a|ab|abc/, "<$&>"), "abc".replace(/abc|ab|a/, "<$&>"), "abc".replace(/x||a/, "<$&>")]
--- value
["<a>bc","<abc>","<>abc"]

=== captures and their reset at each iteration
["ab".replace(/(?:(a)|b)+/, "[$1]"), "aba".replace(/(?:(a)|(b))+/, "[$1|$2]"), "aab".replace(/(a|(b))+/, "[$1|$2]")]
--- value
["[]","[a|]","[b|b]"]

=== an iteration that matches empty fails
["b".replace(/(a*)*/, "[$1]"), "b".replace(/(a*)+/, "[$1]"), "a".replace(/(?:a|())*?/, "[$1]"), "b".replace(/(a*)?/, "[$1]"), "b".replace(/(a*){0,3}/, "[$1]"), "b".replace(/(a*){1,2}/, "[$1]")]
--- value
["[]b","[]b","[]a","[]b","[]b","[]b"]

=== the minimum count is not checked for empty iterations
"ab".replace(/(a?){2,}/, "[$&|$1]")
--- value
"[a|]b"

=== a thread dropped only with the same registers at the position
"aa".replace(/(?:a*?|aa)*/, "[$&]")
--- value
"[aa]"

=== nested quantifiers that can match empty
["aaa".replace(/((a*)*)*/, "[$1|$2]"), "abab".replace(/(?:(a)|b|())*/, "[$&|$1|$2]"), "aa".replace(/(a?)*?$/, "[$&|$1]")]
--- value
["[aaa|aaa]","[abab||]","[aa|a]"]

=== a pattern that backtracks exponentially, matched in linear time
/(a+)+$/.test("a".repeat(20) + "b")
--- value
false

=== case-insensitive ASCII
[/abc/i.test("ABC"), /[a-c]+/i.test("BaC"), /[^a]/i.test("A"), "AbC".replace(/b/gi, "x")]
--- value
[true,true,false,"AxC"]

=== k and s with i, without u: U+212A and U+017F stay themselves
[/k/i.test("\u212a"), /s/i.test("\u017f"), /\w/i.test("\u017f"), /[a-z]/i.test("\u212a")]
--- value
[false,false,false,false]

=== k and s with i and u: U+212A folds to k, U+017F to s
[/k/iu.test("\u212a"), /K/iu.test("\u212a"), /s/iu.test("\u017f"), /[a-z]/iu.test("\u212a"), /[^k]/iu.test("\u212a"), /\w/iu.test("\u017f"), /\W/iu.test("\u017f"), /\W/iu.test("\u212a")]
--- value
[true,true,true,true,false,true,false,false]

=== \b with i and u follows \w
["\u017fa".replace(/\b/giu, "|"), "\u017fa".replace(/\b/gi, "|")]
--- value
["|ſa|","ſ|a|"]

=== without u, a character outside the BMP is two code units
[/^.{2}$/.test("😀"), /^.$/.test("😀"), /\uD83D/.test("😀"), /^\uD83D\uDE00$/.test("😀"), "😀x".search(/x/)]
--- value
[true,false,true,true,2]

=== with u, a code point
[/^.$/u.test("😀"), /^\u{1F600}$/u.test("😀"), /[\u{1F600}-\u{1F64F}]/u.test("😃"), /^[😀]$/u.test("😀"), /\uD83D\uDE00/u.test("😀")]
--- value
[true,true,true,true,true]

=== with u, V8 tries a search's start between the halves of a pair
["a😀".search(/\B/u), "😀".search(/\B$/u), "a😀".search(/\B./u)]
--- value
[2,2,-1]

=== search gives a UTF-16 index
["é😀ab".search(/a/), "é😀ab".search(/a/u), "abc".search(/x/)]
--- value
[3,3,-1]

=== search with g and y
["xab".search(/a/g), "xab".search(/a/y), "abc".search(/a/y)]
--- value
[1,-1,0]

=== match with g
["a1b22c333".match(/\d+/g), "abc".match(/x/g), "aaa".match(/(?:)/g), "aXbX".match(/(X)/g)]
--- value
[["1","22","333"],null,["","","",""],["X","X"]]

=== match with g and y matches only where the last match ended
["aab".match(/a/gy), "baa".match(/a/gy)]
--- value
[["a","a"],null]

=== match with g steps over a character outside the BMP with u
["😀".match(/(?:)/gu), "a😀".match(/\B/gu)]
--- value
[["",""],["",""]]

=== split on a pattern
["a, b,c ,d".split(/\s*,\s*/), "aXbXc".split(/(X)/), "abc".split(/(?:)/), "ab".split(/(a)?b/)]
--- value
[["a","b","c","d"],["a","X","b","X","c"],["a","b","c"],["","a",""]]

=== split's edges: an empty input, an empty match at the start, a limit
["".split(/a*/), "".split(/x/), "abc".split(/b/, 1), "abc".split(/(b)/, 2), "abc".split(/x/, 0), "abc".split(/a*?/), "abc".split(/$/), "abc".split(/^/)]
--- value
[[],[""],["a"],["a","b"],[],["a","b","c"],["abc"],["abc"]]

=== split ignores g and y
["a,b".split(/,/g), "a,b".split(/,/y)]
--- value
[["a","b"],["a","b"]]

=== split with u keeps a character outside the BMP whole
"a😀b".split(/(?:)/u)
--- value
["a","😀","b"]

=== replace with a pattern, once or with g
["a-b-c".replace(/-/, "+"), "a-b-c".replace(/-/g, "+"), "abc".replace(/x/, "y"), "abc".replace(/x/g, "y")]
--- value
["a+b-c","a+b+c","abc","abc"]

=== replace's $ patterns
["abc".replace(/b/, "[$$|$&|$`|$']"), "abc".replace(/(b)/, "[$0|$00|$01|$1|$10|$2|$9]"), "abc".replace(/b/, "$"), "abc".replace(/b/, "$x")]
--- value
["a[$|b|a|c]c","a[$0|$00|b|b|b0|$2|$9]c","a$c","a$xc"]

=== replace's $nn with many groups
"abcdefghijkl".replace(/(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)/, "$11-$10-$111")
--- value
"k-j-k1l"

=== replace's $<name>
["2020-01-02".replace(/(?<y>\d+)-(?<m>\d+)-(?<d>\d+)/, "$<d>/$<m>/$<y>"), "ab".replace(/(?<x>a)|(?<y>b)/g, "[$<x>|$<y>|$<z>]"), "ab".replace(/(?<x>a)/, "$<x"), "ab".replace(/(a)/, "$<x>")]
--- value
["02/01/2020","[a||][|b|]","$<xb","$<x>b"]

=== replace with g and empty matches
["aaa".replace(/a*?/g, "-"), "abc".replace(/(?:)/g, "-"), "😀".replace(/(?:)/gu, "-")]
--- value
["-a-a-a-","-a-b-c-","-😀-"]

=== replace's replacement is a primitive's text
["a".replace(/a/, 1), "a".replace(/a/, true), "a".replace(/a/, null), "a".replace(/a/)]
--- value
["1","true","null","undefined"]

=== replaceAll with g
["a.b.c".replaceAll(/\./g, "!"), "abc".replaceAll(/(b)/g, "[$1]")]
--- value
["a!b!c","a[b]c"]

=== replace with a string pattern
["a.b.c".replace(".", "$&$&"), "x".replace("", "-"), "abc".replace("z", "q"), "abc".replace("b", "[$`|$'|$1|$<x>]"), "😀".replace("", "-")]
--- value
["a..b.c","-x","abc","a[a|c|$1|$<x>]c","-😀"]

=== the input as it was when nothing matched, a character outside the BMP included
["😀".replace(/x/, "y"), "😀".replace(/x/g, "y"), "😀".split(/x/)]
--- value
["😀","😀",["😀"]]

# ---------------------------------------------------------------------------
# Faults where JavaScript gives a value

=== test with g faults: JavaScript changes lastIndex
/a/g.test("a")
--- fault lastIndex

=== test with y faults: JavaScript changes lastIndex
/a/y.test("a")
--- fault lastIndex

=== replace with y and without g faults: JavaScript changes lastIndex
"a".replace(/a/y, "b")
--- fault lastIndex

=== match with y and without g faults: JavaScript changes lastIndex
"a".match(/a/y)
--- fault lastIndex

=== replaceAll without g: JavaScript throws a TypeError
"a".replaceAll(/a/, "b")
--- fault TypeError

=== a match with a string pattern faults, where JavaScript makes a RegExp of it
"a.b".match(".")
--- fault regular expression literal

=== a search with a string pattern faults, where JavaScript makes a RegExp of it
"a.b".search(".")
--- fault regular expression literal

=== test of a number faults, where JavaScript converts it
/1/.test(1)
--- fault against a string

=== split with a limit that is a string faults, where JavaScript converts it
"a,b".split(/,/, "1")
--- fault limit

=== replace with a function faults
"a".replace(/a/, x => x)
--- fault function's result

=== replace with a string pattern and a function faults
"a".replace("a", x => x)
--- fault function's result

=== replace with a BigInt faults, where JavaScript writes it
"a".replace(/a/, 1n)
--- fault BigInt

=== replace with an object faults
"a".replace(/a/, {})
--- fault only primitives

=== a piece that would cut a pair in two faults, where JavaScript makes a lone surrogate
"😀".split(/(?:)/)
--- fault lone surrogate

=== a replacement between the halves of a pair faults
"😀".replace(/(?:)/g, "-")
--- fault lone surrogate

=== a match of half a pair faults, where JavaScript makes a lone surrogate
"😀".match(/./g)
--- fault lone surrogate

=== V8's empty match between the halves of a pair, with u, faults in a replacement
"a😀".replace(/\B/u, "-")
--- fault lone surrogate

=== the methods of a regular expression it does not have yet
/a/.exec("a")
--- fault does not have this method
