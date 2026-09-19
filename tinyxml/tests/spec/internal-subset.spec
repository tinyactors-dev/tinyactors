# The internal DTD subset, read only under TXML_INTERNAL_SUBSET.
#
# Two things are taken from it: general entity declarations and attribute
# defaults. Nothing external is ever fetched. With the flag off the subset is
# skipped, which makes its entities undeclared.

@case a declared entity expands in content
@flags INTERNAL_SUBSET
@xml
<!DOCTYPE a [<!ENTITY greeting "hello">]>
<a>&greeting; world</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "hello world" end=3 parent=1 depth=2
@names
0 {}a
@end

@case a declared entity expands in an attribute value
@flags INTERNAL_SUBSET
@xml
<!DOCTYPE a [<!ENTITY e "v">]>
<a k="&e;"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {}k "v"
@names
0 {}a
1 {}k
@end

@case an attribute default is supplied and flagged
@flags INTERNAL_SUBSET
@xml
<!DOCTYPE a [<!ATTLIST a k CDATA "v">]>
<a/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {}k "v" flags=DEFAULTED
@names
0 {}a
1 {}k
@end

@case a written attribute overrides its default
@flags INTERNAL_SUBSET
@xml
<!DOCTYPE a [<!ATTLIST a k CDATA "v">]>
<a k="w"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {}k "w"
@names
0 {}a
1 {}k
@end

@case a default applies to every element of that name
@flags INTERNAL_SUBSET
@xml
<!DOCTYPE r [<!ATTLIST b k CDATA "v">]>
<r><b/><b k="w"/></r>
@nodes
0 document - end=4 parent=- depth=0
1 element {}r end=4 parent=0 depth=1 attributes=0
2 element {}b end=3 parent=1 depth=2 attributes=1 flags=EMPTY
3 element {}b end=4 parent=1 depth=2 attributes=1 flags=EMPTY
@attributes
2.0 {}k "v" flags=DEFAULTED
3.0 {}k "w"
@names
0 {}r
1 {}b
2 {}k
@end

@case without the flag the subset is skipped and its entities are undeclared
@xml
<!DOCTYPE a [<!ENTITY greeting "hello">]>
<a>&greeting;</a>
@error
UNDECLARED_ENTITY 2:4 offset=45
@end

@case without the flag a doctype alone is still skipped cleanly
@xml
<!DOCTYPE a [<!ELEMENT a EMPTY>]>
<a/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}a
@end
