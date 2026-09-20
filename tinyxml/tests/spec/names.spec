# Names: XML 1.0 (fifth edition) NameStartChar and NameChar, which are code
# point ranges and not "any byte that is not ASCII". A character that cannot
# be part of a name is TXML_DOC_NAME at that character; columns count bytes.
#
# Legal non-ASCII names first, in every place a name can appear.

@case a name may start with a letter outside ASCII
@xml
<é/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}é end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}é
@end

@case a middle dot and combining marks may continue a name
@xml
<a·b/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a·b end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}a·b
@end

@case CJK element and attribute names
@xml
<日本 属性="値"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}日本 end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {}属性 "値"
@names
0 {}日本
1 {}属性
@end

@case a name may start outside the basic multilingual plane
@xml
<𐀀/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}𐀀 end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}𐀀
@end

@case an end tag with a non-ASCII name matches its start tag
@xml
<é></é>
@nodes
0 document - end=2 parent=- depth=0
1 element {}é end=2 parent=0 depth=1 attributes=0
@names
0 {}é
@end

@case a prefixed name with a non-ASCII local part
@xml
<p:é xmlns:p="urn:x"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {urn:x}é prefix=p end=2 parent=0 depth=1 attributes=1 flags=EMPTY
@attributes
1.0 {http://www.w3.org/2000/xmlns/}p "urn:x" flags=NS_DECL
@names
0 {urn:x}é
1 {http://www.w3.org/2000/xmlns/}p
@end

@case a processing instruction target with a non-ASCII name
@xml
<a><?é d?></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 pi {}é "d" end=3 parent=1 depth=2
@names
0 {}a
1 {}é
@end

@case an entity may have a non-ASCII name
@xml
<!DOCTYPE a [<!ENTITY é "v">]><a>&é;</a>
@nodes
0 document - end=4 parent=- depth=0
1 doctype " a [<!ENTITY é \"v\">]" end=2 parent=0 depth=1
2 element {}a end=4 parent=0 depth=1 attributes=0
3 text "v" end=4 parent=2 depth=2
@names
0 {}a
@end

# Names the old byte-wise rule accepted because their bytes are not ASCII.

@case a multiplication sign is not a name character
@xml
<a×b/>
@error
NAME 1:3 offset=2
@end

@case an em dash is not a name character
@xml
<a—b/>
@error
NAME 1:3 offset=2
@end

@case a middle dot cannot start a name
@xml
<·a/>
@error
NAME 1:2 offset=1
@end

@case a combining mark cannot start a name
@xml
<́a/>
@error
NAME 1:2 offset=1
@end

@case a character that is no name character at all cannot start one
@xml
<×a/>
@error
NAME 1:2 offset=1
@end

@case an illegal character in an attribute name
@xml
<a b×c="1"/>
@error
NAME 1:5 offset=4
@end

@case an attribute name cannot start with a middle dot
@xml
<a ·b="1"/>
@error
NAME 1:4 offset=3
@end

@case an illegal character in an end tag name
@xml
<a></a×>
@error
NAME 1:7 offset=6
@end

@case an illegal character in a processing instruction target
@xml
<?p×q d?><a/>
@error
NAME 1:4 offset=3
@end

@case a processing instruction target cannot start with a middle dot
@xml
<?·p?><a/>
@error
NAME 1:3 offset=2
@end

@case an illegal character in an entity reference name
@xml
<a>&a×b;</a>
@error
NAME 1:4 offset=3
@end

@case an entity reference name cannot start with a digit
@xml
<a>&1x;</a>
@error
NAME 1:4 offset=3
@end

@case an illegal character in the DOCTYPE name
@xml
<!DOCTYPE a×b><a/>
@error
NAME 1:12 offset=11
@end

@case an illegal character in an entity declaration name
@xml
<!DOCTYPE a [<!ENTITY a×b "v">]><a/>
@error
NAME 1:24 offset=23
@end

@case an illegal character in an ATTLIST attribute name
@xml
<!DOCTYPE a [<!ATTLIST a b×c CDATA "v">]><a/>
@error
NAME 1:27 offset=26
@end

@case an illegal character in an ATTLIST element name
@xml
<!DOCTYPE a [<!ATTLIST a×b c CDATA "v">]><a/>
@error
NAME 1:25 offset=24
@end

@case a name split across chunks is checked whole
@chunk 1
@xml
<日本×/>
@error
NAME 1:8 offset=7
@end
