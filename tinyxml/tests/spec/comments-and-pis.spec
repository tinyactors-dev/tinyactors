# Comments and processing instructions.
#
# Both are dropped unless their keep flag is set, and a dropped node's names
# are never interned. Comments and PIs outside the document element are
# children of the document node. The XML declaration is not a PI.

@case comments and PIs are kept on request
@flags KEEP_COMMENTS KEEP_PIS
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

@case comments and PIs are dropped by default
@xml
<a><!-- c --><?p d?></a>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0
@names
0 {}a
@end

@case only comments are kept
@flags KEEP_COMMENTS
@xml
<a><!-- c --><?p d?></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 comment " c " end=3 parent=1 depth=2
@names
0 {}a
@end

@case prolog and epilog comments belong to the document node
@flags KEEP_COMMENTS
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

@case the XML declaration is not a processing instruction
@flags KEEP_PIS
@xml
<?xml version="1.0" encoding="UTF-8"?><a/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}a
@end

@case a PI may have no data
@flags KEEP_PIS
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
@flags KEEP_COMMENTS
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
@flags KEEP_PIS
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
