# The UTF-8 byte order mark. A leading EF BB BF is an encoding signature, not
# content: it is consumed without a node, counted in the byte offset, and does
# not move the column, so what follows it is at column 1 and offset 3. U+FEFF
# anywhere else is ordinary character data. UTF-16 marks are not UTF-8 and are
# encoding errors. The writer never emits a BOM (ADR 0017).
#
# The input is written with \x escapes because an editor would swallow a BOM.

@case a leading BOM produces no node
@xml
\xef\xbb\xbf<a/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@positions
0 1:1 offset=0
1 1:1 offset=3
@names
0 {}a
@roundtrip
<a/>
@end

@case a BOM split after its first byte
@chunk 1
@xml
\xef\xbb\xbf<a/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@positions
0 1:1 offset=0
1 1:1 offset=3
@end

@case a BOM split after its second byte
@chunk 2
@xml
\xef\xbb\xbf<a/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@positions
0 1:1 offset=0
1 1:1 offset=3
@end

@case the XML declaration may follow a BOM
@xml
\xef\xbb\xbf<?xml version="1.0"?><a/>
@nodes
0 document - end=3 parent=- depth=0
1 declaration - end=2 parent=0 depth=1 attributes=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@attributes
1.0 {}version "1.0"
@positions
0 1:1 offset=0
1 1:1 offset=3
2 1:22 offset=24
@end

@case whitespace after a BOM is a text node at column 1
@xml
\xef\xbb\xbf  <a/>
@nodes
0 document - end=3 parent=- depth=0
1 text "  " end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@positions
0 1:1 offset=0
1 1:1 offset=3
2 1:3 offset=5
@end

@case the XML declaration must be the first thing after a BOM
@xml
\xef\xbb\xbf <?xml version="1.0"?><a/>
@error
SYNTAX 1:2 offset=4
@end

@case text after a BOM is still text before the document element
@xml
\xef\xbb\xbfx<a/>
@error
SYNTAX 1:1 offset=3
@end

@case a second BOM is text before the document element
@xml
\xef\xbb\xbf\xef\xbb\xbf<a/>
@error
SYNTAX 1:1 offset=3
@end

# The expected text below holds U+FEFF, written literally and invisible.
@case a BOM inside content is character data
@xml
<a>\xef\xbb\xbfx</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "﻿x" end=3 parent=1 depth=2
@positions
0 1:1 offset=0
1 1:1 offset=0
2 1:4 offset=3
@end

@case a BOM alone is an empty document
@xml
\xef\xbb\xbf
@error
NO_ROOT 1:1 offset=3
@end

@case a BOM cut off by the end of the input is an encoding error
@xml
\xef\xbb
@error
ENCODING 1:1 offset=0
@end

@case a UTF-16 little-endian mark is an encoding error
@xml
\xff\xfe<\x00a\x00/\x00>\x00
@error
ENCODING 1:1 offset=0
@end

@case a UTF-16 big-endian mark is an encoding error
@xml
\xfe\xff\x00<\x00a\x00/\x00>
@error
ENCODING 1:1 offset=0
@end
