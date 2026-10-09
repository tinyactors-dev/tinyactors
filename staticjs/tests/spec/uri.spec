# encodeURI, encodeURIComponent, decodeURI and decodeURIComponent.
# Every expected value was printed by Node 22. Where JavaScript throws a
# URIError, staticjs faults.

=== the unreserved characters stay as they are in both encoders
[encodeURIComponent("AZaz09-_.!~*'()"), encodeURI("AZaz09-_.!~*'()")]
--- value
["AZaz09-_.!~*'()","AZaz09-_.!~*'()"]

=== the reserved characters: encodeURIComponent escapes them, encodeURI keeps them
[encodeURIComponent(";/?:@&=+$,#"), encodeURI(";/?:@&=+$,#")]
--- value
["%3B%2F%3F%3A%40%26%3D%2B%24%2C%23",";/?:@&=+$,#"]

=== the other ASCII characters are escaped by both
[encodeURIComponent(" \"%<>[]\\^`{|}"), encodeURI(" \"%<>[]\\^`{|}")]
--- value
["%20%22%25%3C%3E%5B%5D%5C%5E%60%7B%7C%7D","%20%22%25%3C%3E%5B%5D%5C%5E%60%7B%7C%7D"]

=== control characters are escaped, in upper-case hex
[encodeURIComponent("\t\n\u007f\u0000"), encodeURI("\r")]
--- value
["%09%0A%7F%00","%0D"]

=== non-ASCII text is escaped as its UTF-8 bytes
[encodeURIComponent("é😀日"), encodeURI("é😀日"), encodeURIComponent("\u0080߿ࠀ￿")]
--- value
["%C3%A9%F0%9F%98%80%E6%97%A5","%C3%A9%F0%9F%98%80%E6%97%A5","%C2%80%DF%BF%E0%A0%80%EF%BF%BF"]

=== encodeURI keeps the structure of a whole URI
encodeURI("https://例え.jp/a b?q=x&y=é#frag")
--- value
"https://%E4%BE%8B%E3%81%88.jp/a%20b?q=x&y=%C3%A9#frag"

=== the empty string
[encodeURIComponent(""), encodeURI("")]
--- value
["",""]

=== a primitive is encoded as String gives it
[encodeURIComponent(1.5), encodeURIComponent(-0), encodeURIComponent(1e21), encodeURIComponent(true), encodeURIComponent(null), encodeURIComponent(undefined), encodeURIComponent(), encodeURI(10n), encodeURI(NaN)]
--- value
["1.5","0","1e%2B21","true","null","undefined","undefined","10","NaN"]

=== encodeURIComponent of an object faults, as String of one does
encodeURIComponent({})
--- fault only primitives

=== encodeURI of an array faults, as String of one does
encodeURI([1, 2])
--- fault only primitives

=== encodeURIComponent of a lone surrogate made at run time faults (a URIError in JavaScript)
encodeURIComponent(String.fromCharCode(0xD800))
--- fault lone surrogate

=== encodeURI of a lone surrogate made at run time faults (a URIError in JavaScript)
encodeURI("a" + String.fromCharCode(0xDC00) + "b")
--- fault lone surrogate

=== a lone surrogate in a literal is refused when compiled (a URIError in JavaScript)
encodeURIComponent("\uD800")
--- error unsupported lone surrogate

=== a surrogate pair written as two escapes is one character
encodeURIComponent("😀")
--- value
"%F0%9F%98%80"

=== decodeURIComponent decodes the reserved characters, decodeURI keeps them escaped
[decodeURIComponent("%3B%2F%3F%3A%40%26%3D%2B%24%2C%23"), decodeURI("%3B%2F%3F%3A%40%26%3D%2B%24%2C%23")]
--- value
[";/?:@&=+$,#","%3B%2F%3F%3A%40%26%3D%2B%24%2C%23"]

=== decodeURI keeps a reserved escape as written, in lower case too
[decodeURI("%2f%3a"), decodeURIComponent("%2f%3a"), decodeURI("%41%25%20"), decodeURIComponent("%41%25%20")]
--- value
["%2f%3a","/:","A% ","A% "]

=== decoding UTF-8 escapes, in either case, and text that is not escaped
[decodeURIComponent("%C3%A9%F0%9F%98%80%E6%97%A5"), decodeURI("%c3%a9%f0%9f%98%80"), decodeURI("é日 x"), decodeURIComponent("a+b")]
--- value
["é😀日","é😀","é日 x","a+b"]

=== decoding a NUL, a DEL, and a character before a reserved escape
[decodeURIComponent("%00").length, decodeURIComponent("%7F") === "\u007f", decodeURI("%E2%82%AC%2F")]
--- value
[1,true,"€%2F"]

=== a primitive is decoded as String gives it
[decodeURIComponent(""), decodeURIComponent(12), decodeURIComponent(true), decodeURIComponent(null), decodeURIComponent(undefined), decodeURIComponent(), decodeURI(25n)]
--- value
["","12","true","null","undefined","undefined","25"]

=== decoding undoes encoding
[decodeURIComponent(encodeURIComponent("é😀 ;/?#%")), decodeURI(encodeURI("https://例え.jp/a b?q=é#f"))]
--- value
["é😀 ;/?#%","https://例え.jp/a b?q=é#f"]

=== decodeURIComponent of an object faults, as String of one does
decodeURIComponent({})
--- fault only primitives

=== a percent sign at the end faults (a URIError in JavaScript)
decodeURIComponent("%")
--- fault malformed URI

=== a percent sign with one hex digit faults (a URIError in JavaScript)
decodeURIComponent("%4")
--- fault malformed URI

=== a percent sign before a character that is not hex faults (a URIError in JavaScript)
decodeURI("a%G0")
--- fault malformed URI

=== a lead byte with no continuation faults (a URIError in JavaScript)
decodeURIComponent("%E9")
--- fault malformed URI

=== a lead byte at the end faults (a URIError in JavaScript)
decodeURI("%C3")
--- fault malformed URI

=== a lead byte followed by a byte that does not continue it faults (a URIError in JavaScript)
decodeURIComponent("%C3%28")
--- fault malformed URI

=== a continuation escape cut short faults (a URIError in JavaScript)
decodeURIComponent("%C3%A")
--- fault malformed URI

=== a continuation that is not escaped faults (a URIError in JavaScript)
decodeURIComponent("%C3A9")
--- fault malformed URI

=== a continuation byte alone faults (a URIError in JavaScript)
decodeURI("%80")
--- fault malformed URI

=== an overlong two-byte encoding faults (a URIError in JavaScript)
decodeURIComponent("%C0%80")
--- fault malformed URI

=== an overlong three-byte encoding faults (a URIError in JavaScript)
decodeURIComponent("%E0%80%80")
--- fault malformed URI

=== an encoded surrogate faults (a URIError in JavaScript)
decodeURI("%ED%A0%80")
--- fault malformed URI

=== a code point past 0x10FFFF faults (a URIError in JavaScript)
decodeURIComponent("%F4%90%80%80")
--- fault malformed URI

=== a five-byte lead faults (a URIError in JavaScript)
decodeURIComponent("%F8%88%80%80%80")
--- fault malformed URI

=== the byte 0xFF faults (a URIError in JavaScript)
decodeURIComponent("%FF")
--- fault malformed URI

=== the functions as callbacks and as values
[["a b", "c/d", "é"].map(encodeURIComponent), ["a%20b", "%2F"].map(decodeURI)]
--- value
[["a%20b","c%2Fd","%C3%A9"],["a b","%2F"]]

=== the functions' types
[typeof encodeURI, typeof encodeURIComponent, typeof decodeURI, typeof decodeURIComponent]
--- value
["function","function","function","function"]
