# Namespaces: resolution, scoping, and how declarations are stored.
#
# A namespace declaration is kept as an attribute flagged NS_DECL whose
# expanded name is in the xmlns namespace: {http://www.w3.org/2000/xmlns/}xmlns
# for a default declaration and {http://www.w3.org/2000/xmlns/}<prefix> for a
# prefixed one. A prefix is recorded on the element but is not part of a name's
# identity, so it renders separately.

@case default namespace applies to element names
@xml
<a xmlns="urn:x"><b/></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {urn:x}a end=3 parent=0 depth=1 attributes=1
2 element {urn:x}b end=3 parent=1 depth=2 attributes=0 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}xmlns "urn:x" flags=NS_DECL
@names
0 {urn:x}a
1 {http://www.w3.org/2000/xmlns/}xmlns
2 {urn:x}b
@end

@case prefixed names keep their prefix
@chunk 3
@xml
<p:a xmlns:p="urn:x"><p:b/></p:a>
@nodes
0 document - end=3 parent=- depth=0
1 element {urn:x}a prefix=p end=3 parent=0 depth=1 attributes=1
2 element {urn:x}b prefix=p end=3 parent=1 depth=2 attributes=0 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}p "urn:x" flags=NS_DECL
@names
0 {urn:x}a
1 {http://www.w3.org/2000/xmlns/}p
2 {urn:x}b
@end

@case a redeclaration is scoped to its element
@xml
<a xmlns="urn:1"><b xmlns="urn:2"><c/></b><d/></a>
@nodes
0 document - end=5 parent=- depth=0
1 element {urn:1}a end=5 parent=0 depth=1 attributes=1
2 element {urn:2}b end=4 parent=1 depth=2 attributes=1
3 element {urn:2}c end=4 parent=2 depth=3 attributes=0 flags=EMPTY
4 element {urn:1}d end=5 parent=1 depth=2 attributes=0 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}xmlns "urn:1" flags=NS_DECL
2.0 {http://www.w3.org/2000/xmlns/}xmlns "urn:2" flags=NS_DECL
@names
0 {urn:1}a
1 {http://www.w3.org/2000/xmlns/}xmlns
2 {urn:2}b
3 {urn:2}c
4 {urn:1}d
@end

@case an unprefixed attribute has no namespace
@xml
<a xmlns="urn:x" k="v"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {urn:x}a end=2 parent=0 depth=1 attributes=2 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}xmlns "urn:x" flags=NS_DECL
1.1 {}k "v"
@names
0 {urn:x}a
1 {http://www.w3.org/2000/xmlns/}xmlns
2 {}k
@end

@case a prefixed attribute is in its prefix's namespace
@xml
<a xmlns:p="urn:x" p:k="v"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=2 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}p "urn:x" flags=NS_DECL
1.1 {urn:x}k "v"
@names
0 {}a
1 {http://www.w3.org/2000/xmlns/}p
2 {urn:x}k
@end

@case a declaration is visible on the element that carries it
@xml
<p:a xmlns:p="urn:x" p:k="v"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {urn:x}a prefix=p end=2 parent=0 depth=1 attributes=2 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}p "urn:x" flags=NS_DECL
1.1 {urn:x}k "v"
@names
0 {urn:x}a
1 {http://www.w3.org/2000/xmlns/}p
2 {urn:x}k
@end

@case the default namespace can be undeclared
@xml
<a xmlns="urn:x"><b xmlns=""><c/></b></a>
@nodes
0 document - end=4 parent=- depth=0
1 element {urn:x}a end=4 parent=0 depth=1 attributes=1
2 element {}b end=4 parent=1 depth=2 attributes=1
3 element {}c end=4 parent=2 depth=3 attributes=0 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}xmlns "urn:x" flags=NS_DECL
2.0 {http://www.w3.org/2000/xmlns/}xmlns "" flags=NS_DECL
@names
0 {urn:x}a
1 {http://www.w3.org/2000/xmlns/}xmlns
2 {}b
3 {}c
@end

@case two prefixes may bind the same namespace
@xml
<a xmlns:p="urn:x" xmlns:q="urn:x"><p:b/><q:b/></a>
@nodes
0 document - end=4 parent=- depth=0
1 element {}a end=4 parent=0 depth=1 attributes=2
2 element {urn:x}b prefix=p end=3 parent=1 depth=2 attributes=0 flags=EMPTY
3 element {urn:x}b prefix=q end=4 parent=1 depth=2 attributes=0 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}p "urn:x" flags=NS_DECL
1.1 {http://www.w3.org/2000/xmlns/}q "urn:x" flags=NS_DECL
@names
0 {}a
1 {http://www.w3.org/2000/xmlns/}p
2 {http://www.w3.org/2000/xmlns/}q
3 {urn:x}b
@end

@case an undeclared prefix is not well-formed
@xml
<p:a/>
@error
UNDECLARED_PREFIX 1:2 offset=1
@end

@case an undeclared prefix on an attribute is not well-formed
@xml
<a p:k="v"/>
@error
UNDECLARED_PREFIX 1:4 offset=3
@end

# Reserved namespaces (Namespaces in XML 1.0 §3). The xmlns prefix is never
# declared. The xml prefix may be declared, but only as its own namespace.
# Neither reserved namespace may be bound to any other prefix or to the
# default. Rebinding a reserved prefix is UNDECLARED_PREFIX; binding a reserved
# URI to the wrong name is SYNTAX. The error is at the offending attribute, or
# for an ATTLIST default at the start tag that receives it.

@case the xml prefix may be declared as its own namespace
@xml
<a xmlns:xml="http://www.w3.org/XML/1998/namespace" xml:lang="en"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=2 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}xml "http://www.w3.org/XML/1998/namespace" flags=NS_DECL
1.1 {http://www.w3.org/XML/1998/namespace}lang "en"
@names
0 {}a
1 {http://www.w3.org/2000/xmlns/}xml
2 {http://www.w3.org/XML/1998/namespace}lang
@end

@case the xml prefix needs no declaration
@xml
<a xml:lang="en"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {http://www.w3.org/XML/1998/namespace}lang "en"
@end

@case the xmlns prefix cannot be declared
@xml
<a xmlns:xmlns="urn:x"/>
@error
UNDECLARED_PREFIX 1:4 offset=3
@end

@case the xmlns prefix cannot be declared even as its own namespace
@xml
<a xmlns:xmlns="http://www.w3.org/2000/xmlns/"/>
@error
UNDECLARED_PREFIX 1:4 offset=3
@end

@case the xml prefix cannot be bound to another namespace
@xml
<a xmlns:xml="urn:x"/>
@error
UNDECLARED_PREFIX 1:4 offset=3
@end

@case the xml prefix cannot be bound to the xmlns namespace
@xml
<a xmlns:xml="http://www.w3.org/2000/xmlns/"/>
@error
UNDECLARED_PREFIX 1:4 offset=3
@end

@case no other prefix may be bound to the XML namespace
@xml
<a xmlns:p="http://www.w3.org/XML/1998/namespace"/>
@error
SYNTAX 1:4 offset=3
@end

@case the default namespace may not be the XML namespace
@xml
<a xmlns="http://www.w3.org/XML/1998/namespace"/>
@error
SYNTAX 1:4 offset=3
@end

@case no prefix may be bound to the xmlns namespace
@xml
<a xmlns:p="http://www.w3.org/2000/xmlns/"/>
@error
SYNTAX 1:4 offset=3
@end

@case the default namespace may not be the xmlns namespace
@xml
<a xmlns="http://www.w3.org/2000/xmlns/"/>
@error
SYNTAX 1:4 offset=3
@end

@case a reserved namespace on an inner element is refused there
@xml
<a><b xmlns:p="http://www.w3.org/XML/1998/namespace"/></a>
@error
SYNTAX 1:7 offset=6
@end

@case an ATTLIST default that declares the xml prefix correctly is fine
@xml
<!DOCTYPE a [<!ATTLIST a xmlns:xml CDATA "http://www.w3.org/XML/1998/namespace">]><a xml:lang="en"/>
@error
@end

@case an ATTLIST default cannot bind a reserved namespace to another prefix
@xml
<!DOCTYPE a [<!ATTLIST a xmlns:p CDATA "http://www.w3.org/XML/1998/namespace">]><a/>
@error
SYNTAX 1:81 offset=80
@end

@case an ATTLIST default cannot declare the xmlns prefix
@xml
<!DOCTYPE a [<!ATTLIST a xmlns:xmlns CDATA "urn:x">]><a/>
@error
UNDECLARED_PREFIX 1:54 offset=53
@end
