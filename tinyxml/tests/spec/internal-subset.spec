# The internal DTD subset, which is always read (ADR 0017).
#
# Two things are taken from it: general entity declarations and attribute
# defaults. Element declarations are ignored, because txml does not validate.
# Nothing external is ever fetched, so an external DTD is recorded in the
# doctype node and never read, and an entity it would have declared stays
# undeclared.
#
# A doctype is a node whose text is the bytes between `<!DOCTYPE` and its
# matching `>`, verbatim — the same rule a comment's text follows. The cases
# below are written on one line so their node tables are about entities and
# defaults; the whitespace a real document puts between the doctype and the
# document element is a node too, which the last case pins.

@case a declared entity expands in content
@xml
<!DOCTYPE a [<!ENTITY greeting "hello">]><a>&greeting; world</a>
@nodes
0 document - end=4 parent=- depth=0
1 doctype " a [<!ENTITY greeting \"hello\">]" end=2 parent=0 depth=1
2 element {}a end=4 parent=0 depth=1 attributes=0
3 text "hello world" end=4 parent=2 depth=2
@names
0 {}a
@end

@case a declared entity expands in an attribute value
@xml
<!DOCTYPE a [<!ENTITY e "v">]><a k="&e;"/>
@nodes
0 document - end=3 parent=- depth=0
1 doctype " a [<!ENTITY e \"v\">]" end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
2.0 {}k "v"
@names
0 {}a
1 {}k
@end

@case an attribute default is supplied and flagged
@xml
<!DOCTYPE a [<!ATTLIST a k CDATA "v">]><a/>
@nodes
0 document - end=3 parent=- depth=0
1 doctype " a [<!ATTLIST a k CDATA \"v\">]" end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
2.0 {}k "v" flags=DEFAULTED
@names
0 {}a
1 {}k
@end

@case a written attribute overrides its default
@xml
<!DOCTYPE a [<!ATTLIST a k CDATA "v">]><a k="w"/>
@nodes
0 document - end=3 parent=- depth=0
1 doctype " a [<!ATTLIST a k CDATA \"v\">]" end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
2.0 {}k "w"
@names
0 {}a
1 {}k
@end

@case a default applies to every element of that name
@xml
<!DOCTYPE r [<!ATTLIST b k CDATA "v">]><r><b/><b k="w"/></r>
@nodes
0 document - end=5 parent=- depth=0
1 doctype " r [<!ATTLIST b k CDATA \"v\">]" end=2 parent=0 depth=1
2 element {}r end=5 parent=0 depth=1 attributes=0
3 element {}b end=4 parent=2 depth=2 attributes=1 flags=EMPTY
4 element {}b end=5 parent=2 depth=2 attributes=1 flags=EMPTY
@attributes
3.0 {}k "v" flags=DEFAULTED
4.0 {}k "w"
@names
0 {}r
1 {}b
2 {}k
@end

@case an element declaration is kept in the doctype text and otherwise ignored
@xml
<!DOCTYPE a [<!ELEMENT a EMPTY>]><a><b/></a>
@nodes
0 document - end=4 parent=- depth=0
1 doctype " a [<!ELEMENT a EMPTY>]" end=2 parent=0 depth=1
2 element {}a end=4 parent=0 depth=1 attributes=0
3 element {}b end=4 parent=2 depth=2 attributes=0 flags=EMPTY
@names
0 {}a
1 {}b
@end

@case an external DTD is recorded and never fetched
@xml
<!DOCTYPE a SYSTEM "a.dtd"><a/>
@nodes
0 document - end=3 parent=- depth=0
1 doctype " a SYSTEM \"a.dtd\"" end=2 parent=0 depth=1
2 element {}a end=3 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}a
@end

@case an entity an unfetched external DTD would declare is undeclared
@xml
<!DOCTYPE a SYSTEM "a.dtd"><a>&x;</a>
@error
UNDECLARED_ENTITY 1:31 offset=30
@end

@case the whitespace between a doctype and the document element is a node
@xml
<!DOCTYPE a [<!ELEMENT a EMPTY>]>
<a/>
@nodes
0 document - end=4 parent=- depth=0
1 doctype " a [<!ELEMENT a EMPTY>]" end=2 parent=0 depth=1
2 text "\n" end=3 parent=0 depth=1
3 element {}a end=4 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}a
@end
