# Positions, recorded only under KEEP_POSITIONS.
#
# A position is the first byte of the construct that produced the node: the
# `<` of a start tag, the first byte of a text run. Lines and columns count
# from 1 and columns count bytes, not characters. The document node sits at
# the start of the input.

@case positions across lines
@flags KEEP_POSITIONS
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
@positions
0 1:1 offset=0
1 1:1 offset=0
2 1:4 offset=3
3 2:3 offset=6
4 2:7 offset=10
@names
0 {}a
1 {}b
@end

@case positions on one line
@flags KEEP_POSITIONS
@chunk 2
@xml
<a k="v">hi</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=1
2 text "hi" end=3 parent=1 depth=2
@attributes
1.0 {}k "v"
@positions
0 1:1 offset=0
1 1:1 offset=0
2 1:10 offset=9
@names
0 {}a
1 {}k
@end

@case a column counts bytes, not characters
@flags KEEP_POSITIONS
@xml
<a>é<b/></a>
@nodes
0 document - end=4 parent=- depth=0
1 element {}a end=4 parent=0 depth=1 attributes=0
2 text "é" end=3 parent=1 depth=2
3 element {}b end=4 parent=1 depth=2 attributes=0 flags=EMPTY
@positions
0 1:1 offset=0
1 1:1 offset=0
2 1:4 offset=3
3 1:6 offset=5
@names
0 {}a
1 {}b
@end

@case without the flag no position is recorded
@xml
<a/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@positions
@names
0 {}a
@end
