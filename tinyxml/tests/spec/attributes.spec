# Attributes: document order, value normalization, and name interning.
#
# Name IDs are assigned in first-appearance order: an element's own name
# before its attributes, attributes in document order. An attribute whose
# expanded name equals an element's shares that element's name ID.

@case attribute order is preserved and names are shared
@xml
<a z="1" m="2" a="3"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=3 flags=EMPTY
@attributes
1.0 {}z "1"
1.1 {}m "2"
1.2 {}a "3"
@names
0 {}a
1 {}z
2 {}m
@end

@case both quoting styles, with entities in values
@chunk 3
@xml
<a x='sq' y="dq" z="a&amp;b&lt;c"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=3 flags=EMPTY
@attributes
1.0 {}x "sq"
1.1 {}y "dq"
1.2 {}z "a&b<c"
@names
0 {}a
1 {}x
2 {}y
3 {}z
@end

@case literal whitespace in a value normalizes to spaces
@xml
<a k="one\ttwo\nthree\r\nfour"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {}k "one two three four"
@names
0 {}a
1 {}k
@end

@case a character reference in a value is not whitespace-normalized
@xml
<a k="one&#9;two&#10;three"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {}k "one\ttwo\nthree"
@names
0 {}a
1 {}k
@end

@case empty value
@xml
<a k=""/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {}k ""
@names
0 {}a
1 {}k
@end

@case attributes are numbered within their element
@xml
<a x="1"><b y="2" z="3"/></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=1
2 element {}b end=3 parent=1 depth=2 attributes=2 flags=EMPTY
@attributes
1.0 {}x "1"
2.0 {}y "2"
2.1 {}z "3"
@names
0 {}a
1 {}x
2 {}b
3 {}y
4 {}z
@end

@case markup characters survive inside values
@xml
<a k="&lt;b&gt;&amp;&quot;'"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {}k "<b>&\"'"
@names
0 {}a
1 {}k
@end
