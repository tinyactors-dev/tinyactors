# Well-formedness failures.
#
# A document error is data, not a C result: the parse ends with
# TXML_STEP_FAILED and txml_parser_error() describes it. An error is reported
# at the first byte of the offending construct — the `<` of a tag, the first
# byte of a name, the `&` of a reference, the first byte of a bad character
# sequence.
#
# These cases check only @error: whatever partial document exists is an
# implementation detail and is deliberately left unchecked.

@case an end tag must match the open element
@xml
<a></b>
@error
TAG_MISMATCH 1:4 offset=3
@end

@case a mismatch reports the line it is on
@xml
<a>
  </b>
</a>
@error
TAG_MISMATCH 2:3 offset=6
@end

@case an attribute may not be repeated
@xml
<a x="1" x="2"/>
@error
DUPLICATE_ATTRIBUTE 1:10 offset=9
@end

@case a name may not start with a digit
@xml
<1a/>
@error
NAME 1:2 offset=1
@end

@case input may not end inside a tag
@xml
<a
@error
UNEXPECTED_EOF 1:3 offset=2
@end

@case input may not end with an element open
@xml
<a>
@error
UNEXPECTED_EOF 1:4 offset=3
@end

@case an empty document has no document element
@xml
@error
NO_ROOT 1:1 offset=0
@end

@case a prolog alone has no document element
@flags KEEP_COMMENTS
@xml
<!--x-->
@error
NO_ROOT 1:9 offset=8
@end

@case there is only one document element
@xml
<a/><b/>
@error
TRAILING_CONTENT 1:5 offset=4
@end

@case text after the document element is not allowed
@xml
<a/>tail
@error
TRAILING_CONTENT 1:5 offset=4
@end

@case input must be well-formed UTF-8
@xml
<a>\xff</a>
@error
ENCODING 1:4 offset=3
@end

@case a truncated UTF-8 sequence is rejected
@xml
<a>\xe2\x98</a>
@error
ENCODING 1:4 offset=3
@end

@case control characters are not XML characters
@xml
<a>\x01</a>
@error
ENCODING 1:4 offset=3
@end

@case an undeclared entity is not well-formed
@xml
<a>&foo;</a>
@error
UNDECLARED_ENTITY 1:4 offset=3
@end

@case an attribute value must be quoted
@xml
<a x=1/>
@error
SYNTAX 1:6 offset=5
@end

@case an attribute value may not contain a raw <
@xml
<a x="<"/>
@error
SYNTAX 1:7 offset=6
@end

@case a comment may not contain --
@flags KEEP_COMMENTS
@xml
<a><!-- a -- b --></a>
@error
SYNTAX 1:11 offset=10
@end

@case a stray < in content is markup, not text
@xml
<a>1 < 2</a>
@error
SYNTAX 1:6 offset=5
@end
