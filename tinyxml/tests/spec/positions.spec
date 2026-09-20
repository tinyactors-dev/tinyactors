# Positions, which every node has (ADR 0017).
#
# A position is the first byte of the construct that produced the node: the
# `<` of a start tag, comment, PI, declaration or doctype, and the first byte
# of a text run. Lines and columns count from 1 and columns count bytes, not
# characters. The document node sits at the start of the input.

@case positions across lines
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

@case every kind of node carries a position
@xml
<?xml version="1.0"?><!--c--><a><?p d?>x</a>
@nodes
0 document - end=6 parent=- depth=0
1 declaration - end=2 parent=0 depth=1 attributes=1
2 comment "c" end=3 parent=0 depth=1
3 element {}a end=6 parent=0 depth=1 attributes=0
4 pi {}p "d" end=5 parent=3 depth=2
5 text "x" end=6 parent=3 depth=2
@attributes
1.0 {}version "1.0"
@positions
0 1:1 offset=0
1 1:1 offset=0
2 1:22 offset=21
3 1:30 offset=29
4 1:33 offset=32
5 1:40 offset=39
@names
0 {}version
1 {}a
2 {}p
@end
