# Text: entity and character references, line-end normalization, CDATA, and
# whitespace.
#
# Normalization happens while the bytes are copied into the document's pool,
# so a text node is always one contiguous, already-normalized string: entity
# references never split it, and CRLF and lone CR have become LF.
#
# Under ADR 0017 no text is ever dropped. Whitespace between elements is a
# node, whitespace in the prolog and epilog is a node on the document, and a
# CDATA section is always its own node rather than merging with the text
# beside it.

@case predefined entities
@xml
<a>&lt;&gt;&amp;&quot;&apos;</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "<>&\"'" end=3 parent=1 depth=2
@names
0 {}a
@end

@case character references, decimal and hexadecimal
@xml
<a>&#65;&#x263A;&#10;</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "A☺\n" end=3 parent=1 depth=2
@names
0 {}a
@end

@case references do not split a text node
@chunk 3
@xml
<a>x&amp;y&#38;z</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "x&y&z" end=3 parent=1 depth=2
@names
0 {}a
@end

@case CRLF and lone CR normalize to LF
@xml
<a>one\r\ntwo\rthree\nfour</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "one\ntwo\nthree\nfour" end=3 parent=1 depth=2
@names
0 {}a
@end

@case a CDATA section is always its own node
@xml
<a>p<![CDATA[q<r]]>s</a>
@nodes
0 document - end=5 parent=- depth=0
1 element {}a end=5 parent=0 depth=1 attributes=0
2 text "p" end=3 parent=1 depth=2
3 text "q<r" end=4 parent=1 depth=2 flags=CDATA
4 text "s" end=5 parent=1 depth=2
@names
0 {}a
@end

@case an empty CDATA section is still a node
@xml
<a><![CDATA[]]></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "" end=3 parent=1 depth=2 flags=CDATA
@names
0 {}a
@end

@case whitespace between elements is text
@xml
<a>
  <b/>
</a>
@nodes
0 document - end=5 parent=- depth=0
1 element {}a end=5 parent=0 depth=1 attributes=0
2 text "\n  " end=3 parent=1 depth=2
3 element {}b end=4 parent=1 depth=2 attributes=0 flags=EMPTY
4 text "\n" end=5 parent=1 depth=2
@names
0 {}a
1 {}b
@end

@case text and elements interleave
@xml
<a>x <b/></a>
@nodes
0 document - end=4 parent=- depth=0
1 element {}a end=4 parent=0 depth=1 attributes=0
2 text "x " end=3 parent=1 depth=2
3 element {}b end=4 parent=1 depth=2 attributes=0 flags=EMPTY
@names
0 {}a
1 {}b
@end

@case whitespace in the prolog and epilog belongs to the document node
@xml
\n<a/>\n
@nodes
0 document - end=4 parent=- depth=0
1 text "\n" end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
3 text "\n" end=4 parent=0 depth=1
@names
0 {}a
@end

@case a tab is ordinary text
@xml
<a>x\ty</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "x\ty" end=3 parent=1 depth=2
@names
0 {}a
@end

@case multi-byte UTF-8 passes through unchanged
@chunk 1
@xml
<a>héllo ☺</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "héllo ☺" end=3 parent=1 depth=2
@names
0 {}a
@end
