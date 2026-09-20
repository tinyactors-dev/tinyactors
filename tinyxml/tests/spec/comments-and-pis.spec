# Comments, processing instructions, and the XML declaration.
#
# Every construct in the input is a node (ADR 0017): comments and PIs are
# always kept, and their names are always interned. Comments and PIs outside
# the document element are children of the document node. The XML declaration
# is its own kind rather than a PI, and carries version, encoding and
# standalone as ordinary attributes in no namespace.

@case comments and PIs are nodes
@xml
<a><!-- c --><?p d?></a>
@nodes
0 document - end=4 parent=- depth=0
1 element {}a end=4 parent=0 depth=1 attributes=0
2 comment " c " end=3 parent=1 depth=2
3 pi {}p "d" end=4 parent=1 depth=2
@names
0 {}a
1 {}p
@end

@case prolog and epilog comments belong to the document node
@xml
<!--x--><a/><!--y-->
@nodes
0 document - end=4 parent=- depth=0
1 comment "x" end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
3 comment "y" end=4 parent=0 depth=1
@names
0 {}a
@end

@case the XML declaration is a node, not a processing instruction
@xml
<?xml version="1.0" encoding="UTF-8"?><a/>
@nodes
0 document - end=3 parent=- depth=0
1 declaration - end=2 parent=0 depth=1 attributes=2
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@attributes
1.0 {}version "1.0"
1.1 {}encoding "UTF-8"
@names
0 {}version
1 {}encoding
2 {}a
@end

@case a declaration carries standalone like any other pseudo-attribute
@xml
<?xml version="1.0" standalone="yes"?><a/>
@nodes
0 document - end=3 parent=- depth=0
1 declaration - end=2 parent=0 depth=1 attributes=2
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@attributes
1.0 {}version "1.0"
1.1 {}standalone "yes"
@names
0 {}version
1 {}standalone
2 {}a
@end

@case a declaration with only a version
@chunk 3
@xml
<?xml version="1.0"?><a/>
@nodes
0 document - end=3 parent=- depth=0
1 declaration - end=2 parent=0 depth=1 attributes=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@attributes
1.0 {}version "1.0"
@names
0 {}version
1 {}a
@end

@case a target that merely begins with xml is a PI
@xml
<?xml-stylesheet href="x"?><a/>
@nodes
0 document - end=3 parent=- depth=0
1 pi {}xml-stylesheet "href=\"x\"" end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}xml-stylesheet
1 {}a
@end

@case a PI may have no data
@xml
<a><?p?></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 pi {}p "" end=3 parent=1 depth=2
@names
0 {}a
1 {}p
@end

@case markup inside a comment is text
@chunk 3
@xml
<a><!-- <b/> & --></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 comment " <b/> & " end=3 parent=1 depth=2
@names
0 {}a
@end

@case a PI in the prolog
@xml
<?stylesheet href="x"?><a/>
@nodes
0 document - end=3 parent=- depth=0
1 pi {}stylesheet "href=\"x\"" end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}stylesheet
1 {}a
@end
