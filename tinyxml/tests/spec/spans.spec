# Spans: where a node ends (ADR 0023).
#
# A span is [start, end): `end` is one past the node's last byte, as a
# line:column and a byte offset. An element ends after its end tag (or after
# the `/>` of an empty one); text ends where the next markup begins; a comment,
# PI, declaration, CDATA section or DOCTYPE ends after its closing delimiter;
# the document node ends at the end of the input. Offsets count raw input
# bytes, so references and line ends are inside the span at their written
# length, not their decoded one. Columns count bytes.
#
# @attribute_spans rows are `node.attribute name=<l>:<c>..<l>:<c> value=…
# end=<l>:<c> offset=<start>..<end>`: the name, the bytes inside the quotes, and
# the position one past the closing quote. `-` marks an attribute that has no
# source text.

@case an empty element ends after its slash and bracket
@xml
<a/>
@spans
0 1:1 - 1:5 offset=0..4
1 1:1 - 1:5 offset=0..4
@end

@case an element ends after its end tag
@xml
<a>hi</a>
@spans
0 1:1 - 1:10 offset=0..9
1 1:1 - 1:10 offset=0..9
2 1:4 - 1:6 offset=3..5
@end

@case a start tag and an end tag with no content
@xml
<a></a>
@spans
0 1:1 - 1:8 offset=0..7
1 1:1 - 1:8 offset=0..7
@end

@case whitespace inside the end tag belongs to the element
@xml
<a>x</a  >
@spans
0 1:1 - 1:11 offset=0..10
1 1:1 - 1:11 offset=0..10
2 1:4 - 1:5 offset=3..4
@end

@case nested elements and text across lines
@xml
<a>
  <b/>
</a>
@spans
0 1:1 - 3:5 offset=0..15
1 1:1 - 3:5 offset=0..15
2 1:4 - 2:3 offset=3..6
3 2:3 - 2:7 offset=6..10
4 2:7 - 3:1 offset=10..11
@end

@case a lone carriage return does not start a line
@xml
<a>\r<b/></a>
@spans
0 1:1 - 1:13 offset=0..12
1 1:1 - 1:13 offset=0..12
2 1:4 - 1:5 offset=3..4
3 1:5 - 1:9 offset=4..8
@end

@case a carriage return and line feed is one line end
@xml
<a>\r\n<b/></a>
@spans
0 1:1 - 2:9 offset=0..13
1 1:1 - 2:9 offset=0..13
2 1:4 - 2:1 offset=3..5
3 2:1 - 2:5 offset=5..9
@end

@case a text run with references spans the written bytes
@xml
<a>x&amp;y&#65;</a>
@spans
0 1:1 - 1:20 offset=0..19
1 1:1 - 1:20 offset=0..19
2 1:4 - 1:16 offset=3..15
@end

@case a comment, a PI and a CDATA section end after their delimiters
@xml
<a><!--c--><?t d?><![CDATA[x]]></a>
@spans
0 1:1 - 1:36 offset=0..35
1 1:1 - 1:36 offset=0..35
2 1:4 - 1:12 offset=3..11
3 1:12 - 1:19 offset=11..18
4 1:19 - 1:32 offset=18..31
@end

@case a PI with no data
@xml
<a><?t?></a>
@spans
0 1:1 - 1:13 offset=0..12
1 1:1 - 1:13 offset=0..12
2 1:4 - 1:9 offset=3..8
@end

@case the XML declaration and the byte order mark
@xml
\xef\xbb\xbf<?xml version="1.0"?><a/>
@spans
0 1:1 - 1:26 offset=0..28
1 1:1 - 1:22 offset=3..24
2 1:22 - 1:26 offset=24..28
@end

@case a DOCTYPE with an internal subset ends after its closing bracket
@xml
<!DOCTYPE a [<!ENTITY e "a>b">]>
<a/>
@spans
0 1:1 - 2:5 offset=0..37
1 1:1 - 1:33 offset=0..32
2 1:33 - 2:1 offset=32..33
3 2:1 - 2:5 offset=33..37
@end

@case attributes carry their own spans
@xml
<a x="1"  y = 'two' xmlns:p="u"/>
@attribute_spans
1.0 name=1:4..1:5 value=1:7..1:8 end=1:9 offset=3..8
1.1 name=1:11..1:12 value=1:16..1:19 end=1:20 offset=10..19
1.2 name=1:21..1:28 value=1:30..1:31 end=1:32 offset=20..31
@end

@case an attribute value spanning lines and holding a reference
@xml
<a x="1&amp;
2"/>
@attribute_spans
1.0 name=1:4..1:5 value=1:7..2:2 end=2:3 offset=3..15
@end

@case a prefixed attribute's name extent includes the prefix
@xml
<a xmlns:p="u" p:k="v"/>
@attribute_spans
1.0 name=1:4..1:11 value=1:13..1:14 end=1:15 offset=3..14
1.1 name=1:16..1:19 value=1:21..1:22 end=1:23 offset=15..22
@end

@case a defaulted attribute has no source
@xml
<!DOCTYPE a [<!ATTLIST a d CDATA "x">]><a/>
@attribute_spans
2.0 -
@end

@case the declaration's pseudo-attributes have no source
@xml
<?xml version="1.0"?><a b="c"/>
@attribute_spans
1.0 -
2.0 name=1:25..1:26 value=1:28..1:29 end=1:30 offset=24..29
@end

@case spans do not depend on how the input is cut
@chunk 1
@xml
<?xml version="1.0"?>
<a x="1&amp;2">
  <!--c-->t&lt;<![CDATA[d]]><?p q?>
</a>
@spans
0 1:1 - 4:5 offset=0..78
1 1:1 - 1:22 offset=0..21
2 1:22 - 2:1 offset=21..22
3 2:1 - 4:5 offset=22..78
4 2:16 - 3:3 offset=37..40
5 3:3 - 3:11 offset=40..48
6 3:11 - 3:16 offset=48..53
7 3:16 - 3:29 offset=53..66
8 3:29 - 3:36 offset=66..73
9 3:36 - 4:1 offset=73..74
@end
