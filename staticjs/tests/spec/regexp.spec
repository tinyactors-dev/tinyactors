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
