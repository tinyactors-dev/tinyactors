# Elements: document order, subtree ends, depth, and the EMPTY flag.
#
# Node indices are document order. `end` is one past the last descendant, so a
# subtree is [index, end). The document node is index 0 at depth 0.

@case single empty element
@xml
<a/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}a
@end

@case an element pair is not EMPTY
@xml
<a></a>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0
@names
0 {}a
@end

@case element with text
@xml
<a>hi</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "hi" end=3 parent=1 depth=2
@names
0 {}a
@end

@case nested elements and text
@chunk 3
@xml
<a><b>hi</b></a>
@nodes
0 document - end=4 parent=- depth=0
1 element {}a end=4 parent=0 depth=1 attributes=0
2 element {}b end=4 parent=1 depth=2 attributes=0
3 text "hi" end=4 parent=2 depth=3
@names
0 {}a
1 {}b
@end

@case siblings each end where the next begins
@xml
<r><a/><b/><c/></r>
@nodes
0 document - end=5 parent=- depth=0
1 element {}r end=5 parent=0 depth=1 attributes=0
2 element {}a end=3 parent=1 depth=2 attributes=0 flags=EMPTY
3 element {}b end=4 parent=1 depth=2 attributes=0 flags=EMPTY
4 element {}c end=5 parent=1 depth=2 attributes=0 flags=EMPTY
@names
0 {}r
1 {}a
2 {}b
3 {}c
@end

@case mixed content
@xml
<p>one<b>two</b>three</p>
@nodes
0 document - end=6 parent=- depth=0
1 element {}p end=6 parent=0 depth=1 attributes=0
2 text "one" end=3 parent=1 depth=2
3 element {}b end=5 parent=1 depth=2 attributes=0
4 text "two" end=5 parent=3 depth=3
5 text "three" end=6 parent=1 depth=2
@names
0 {}p
1 {}b
@end

@case repeated names share one name id
@xml
<a><a/><a/></a>
@nodes
0 document - end=4 parent=- depth=0
1 element {}a end=4 parent=0 depth=1 attributes=0
2 element {}a end=3 parent=1 depth=2 attributes=0 flags=EMPTY
3 element {}a end=4 parent=1 depth=2 attributes=0 flags=EMPTY
@names
0 {}a
@end

@case deep nesting
@chunk 2
@xml
<a><b><c><d/></c></b></a>
@nodes
0 document - end=5 parent=- depth=0
1 element {}a end=5 parent=0 depth=1 attributes=0
2 element {}b end=5 parent=1 depth=2 attributes=0
3 element {}c end=5 parent=2 depth=3 attributes=0
4 element {}d end=5 parent=3 depth=4 attributes=0 flags=EMPTY
@names
0 {}a
1 {}b
2 {}c
3 {}d
@end

@case whitespace around the document element is a text node
@xml
  <a/>
@nodes
0 document - end=3 parent=- depth=0
1 text "  " end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}a
@end
