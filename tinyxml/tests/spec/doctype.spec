# The DOCTYPE and its internal subset are checked against the XML 1.0
# grammar. A non-validating processor still rejects a malformed declaration:
# it is a well-formedness error, and an entity or attribute default is only
# safe to record if the declaration that carries it parsed. Names follow the
# name rules, references must be well formed, literals must be quoted and
# hold only what their production allows, and every declaration must end
# where the grammar says it does.
#
# Not checked, because it needs the whole DTD: that names are declared, that
# a content model is deterministic, that a group uses one separator kind.
# A case with no @error asserts that the parse succeeds.

@case an external identifier with a system literal
@xml
<!DOCTYPE a SYSTEM "a.dtd"><a/>
@end

@case an external identifier with public and system literals
@xml
<!DOCTYPE a PUBLIC "-//W3C//DTD X 1.0//EN" "http://x/y.dtd"><a/>
@end

@case a subset needs no whitespace before its bracket
@xml
<!DOCTYPE a[]><a/>
@end

@case a subset may follow an external identifier and be followed by whitespace
@xml
<!DOCTYPE a SYSTEM 'x' [ ]  ><a/>
@end

@case every entity declaration form
@xml
<!DOCTYPE a [<!ENTITY e "v"><!ENTITY % pe "v"><!ENTITY x SYSTEM "x.ent"><!ENTITY y PUBLIC "p" "x.ent"><!ENTITY z SYSTEM "x" NDATA gif>]><a/>
@end

@case an entity value may hold references to entities declared later or never
@xml
<!DOCTYPE a [<!ENTITY e "a &amp; &#65; &other; b">]><a/>
@end

@case every attribute type and default form
@xml
<!DOCTYPE a [<!ATTLIST a c CDATA #IMPLIED i ID #REQUIRED r IDREF #IMPLIED rs IDREFS #IMPLIED e ENTITY #IMPLIED es ENTITIES #IMPLIED n NMTOKEN #IMPLIED ns NMTOKENS #IMPLIED en (x|y|z) "x" f CDATA #FIXED "v" nt NOTATION (gif|png) #IMPLIED>]><a i="1"/>
@end

@case every element declaration form
@xml
<!DOCTYPE a [<!ELEMENT a (b,(c|d)*,e?)><!ELEMENT b EMPTY><!ELEMENT c ANY><!ELEMENT d (#PCDATA)><!ELEMENT e (#PCDATA|b|c)*><!ELEMENT f (#PCDATA)*><!ELEMENT g ((h|i)+,j)>]><a/>
@end

@case every notation declaration form
@xml
<!DOCTYPE a [<!NOTATION gif SYSTEM "x"><!NOTATION png PUBLIC "p"><!NOTATION jpg PUBLIC "p" "x">]><a/>
@end

@case a parameter-entity reference between declarations
@xml
<!DOCTYPE a [%pe;<!ELEMENT a EMPTY>]><a/>
@end

@case comments and processing instructions in the subset
@xml
<!DOCTYPE a [<!-- c - d --><?pi data?><?pi?><!---->]><a/>
@end

@case declaration names may be non-ASCII
@xml
<!DOCTYPE a [<!ENTITY \xc3\xa9 "v"><!ELEMENT \xe6\x97\xa5 EMPTY>]><a/>
@end

@case whitespace may be any run of blanks, tabs and newlines
@xml
<!DOCTYPE a [<!ENTITY\n  e\t"v"  \n>]><a/>
@end

@case an entity name may not contain a dollar sign
@xml
<!DOCTYPE a [<!ENTITY a$b "v">]><a/>
@error
SYNTAX 1:24 offset=23
@end

@case an entity value must be quoted
@xml
<!DOCTYPE a [<!ENTITY e v>]><a/>
@error
SYNTAX 1:25 offset=24
@end

@case nothing may follow an entity value
@xml
<!DOCTYPE a [<!ENTITY e "v" junk>]><a/>
@error
SYNTAX 1:29 offset=28
@end

@case a parameter-entity reference is not allowed in an internal entity value
@xml
<!DOCTYPE a [<!ENTITY e "a % b">]><a/>
@error
SYNTAX 1:28 offset=27
@end

@case an ampersand in an entity value must start a reference
@xml
<!DOCTYPE a [<!ENTITY e "a & b">]><a/>
@error
SYNTAX 1:28 offset=27
@end

@case a character reference in an entity value must be a legal character
@xml
<!DOCTYPE a [<!ENTITY e "&#0;">]><a/>
@error
ENCODING 1:26 offset=25
@end

@case an entity name may not start with a digit
@xml
<!DOCTYPE a [<!ENTITY 1e "v">]><a/>
@error
NAME 1:23 offset=22
@end

@case an entity name may not contain a colon
@xml
<!DOCTYPE a [<!ENTITY a:b "v">]><a/>
@error
NAME 1:24 offset=23
@end

@case a parameter entity needs a name
@xml
<!DOCTYPE a [<!ENTITY % "v">]><a/>
@error
SYNTAX 1:25 offset=24
@end

@case SYSTEM needs a literal
@xml
<!DOCTYPE a [<!ENTITY x SYSTEM>]><a/>
@error
SYNTAX 1:31 offset=30
@end

@case a public identifier may only hold public identifier characters
@xml
<!DOCTYPE a [<!ENTITY x PUBLIC "a^b" "x">]><a/>
@error
SYNTAX 1:34 offset=33
@end

@case NDATA needs a name
@xml
<!DOCTYPE a [<!ENTITY x SYSTEM "x" NDATA>]><a/>
@error
SYNTAX 1:41 offset=40
@end

@case only a general entity may be unparsed
@xml
<!DOCTYPE a [<!ENTITY % x SYSTEM "x" NDATA g>]><a/>
@error
SYNTAX 1:38 offset=37
@end

@case a declaration is closed by its own bracket-free angle
@xml
<!DOCTYPE a [<!ENTITY x "v"]><a/>
@error
SYNTAX 1:28 offset=27
@end

@case an attribute definition needs a default
@xml
<!DOCTYPE a [<!ATTLIST a b CDATA>]><a/>
@error
SYNTAX 1:33 offset=32
@end

@case attribute types are case sensitive
@xml
<!DOCTYPE a [<!ATTLIST a b cdata #IMPLIED>]><a/>
@error
SYNTAX 1:28 offset=27
@end

@case an unknown default keyword is rejected
@xml
<!DOCTYPE a [<!ATTLIST a b CDATA #REQUIRE>]><a/>
@error
SYNTAX 1:34 offset=33
@end

@case a default value must be quoted
@xml
<!DOCTYPE a [<!ATTLIST a b CDATA v>]><a/>
@error
SYNTAX 1:34 offset=33
@end

@case an attribute default may not contain a less-than sign
@xml
<!DOCTYPE a [<!ATTLIST a b CDATA "<">]><a/>
@error
SYNTAX 1:35 offset=34
@end

@case an enumeration may not have an empty token
@xml
<!DOCTYPE a [<!ATTLIST a b (x|) #IMPLIED>]><a/>
@error
SYNTAX 1:31 offset=30
@end

@case enumeration tokens are separated by a bar
@xml
<!DOCTYPE a [<!ATTLIST a b (x y) #IMPLIED>]><a/>
@error
SYNTAX 1:31 offset=30
@end

@case an element name in an attribute list may not contain a dollar sign
@xml
<!DOCTYPE a [<!ATTLIST a$ b CDATA #IMPLIED>]><a/>
@error
SYNTAX 1:25 offset=24
@end

@case NOTATION needs its names
@xml
<!DOCTYPE a [<!ATTLIST a b NOTATION #IMPLIED>]><a/>
@error
SYNTAX 1:37 offset=36
@end

@case FIXED needs a value
@xml
<!DOCTYPE a [<!ATTLIST a b CDATA #FIXED>]><a/>
@error
SYNTAX 1:40 offset=39
@end

@case an attribute name in an attribute list may not hold a multiplication sign
@xml
<!DOCTYPE a [<!ATTLIST a b\xc3\x97c CDATA #IMPLIED>]><a/>
@error
NAME 1:27 offset=26
@end

@case an element declaration needs a content specification
@xml
<!DOCTYPE a [<!ELEMENT a>]><a/>
@error
SYNTAX 1:25 offset=24
@end

@case a content model may not end in a separator
@xml
<!DOCTYPE a [<!ELEMENT a (b,)>]><a/>
@error
SYNTAX 1:29 offset=28
@end

@case a content model must balance its parentheses
@xml
<!DOCTYPE a [<!ELEMENT a (b|c>]><a/>
@error
SYNTAX 1:30 offset=29
@end

@case a mixed model with names must end in a star
@xml
<!DOCTYPE a [<!ELEMENT a (#PCDATA|b)>]><a/>
@error
SYNTAX 1:37 offset=36
@end

@case content model items are separated
@xml
<!DOCTYPE a [<!ELEMENT a (b c)>]><a/>
@error
SYNTAX 1:29 offset=28
@end

@case EMPTY is a whole keyword
@xml
<!DOCTYPE a [<!ELEMENT a EMPTYY>]><a/>
@error
SYNTAX 1:26 offset=25
@end

@case nothing may follow a content model
@xml
<!DOCTYPE a [<!ELEMENT a (b)+ x>]><a/>
@error
SYNTAX 1:31 offset=30
@end

@case a mixed model has no nested groups
@xml
<!DOCTYPE a [<!ELEMENT a (#PCDATA|(b))*>]><a/>
@error
SYNTAX 1:35 offset=34
@end

@case a quantifier does not follow a mixed name
@xml
<!DOCTYPE a [<!ELEMENT a (#PCDATA|b?)*>]><a/>
@error
SYNTAX 1:36 offset=35
@end

@case PCDATA must be first
@xml
<!DOCTYPE a [<!ELEMENT a (b|#PCDATA)*>]><a/>
@error
SYNTAX 1:29 offset=28
@end

@case a notation needs an identifier
@xml
<!DOCTYPE a [<!NOTATION n>]><a/>
@error
SYNTAX 1:26 offset=25
@end

@case a notation SYSTEM needs a literal
@xml
<!DOCTYPE a [<!NOTATION n SYSTEM>]><a/>
@error
SYNTAX 1:33 offset=32
@end

@case a notation name may not contain a colon
@xml
<!DOCTYPE a [<!NOTATION n:m SYSTEM "x">]><a/>
@error
NAME 1:26 offset=25
@end

@case a DOCTYPE SYSTEM needs a literal
@xml
<!DOCTYPE a SYSTEM><a/>
@error
SYNTAX 1:19 offset=18
@end

@case nothing may follow the external identifier
@xml
<!DOCTYPE a SYSTEM "x" junk><a/>
@error
SYNTAX 1:24 offset=23
@end

@case a DOCTYPE PUBLIC needs a system literal
@xml
<!DOCTYPE a PUBLIC "p"><a/>
@error
SYNTAX 1:23 offset=22
@end

@case the external identifier is separated from the name
@xml
<!DOCTYPE a SYSTEM'x'><a/>
@error
SYNTAX 1:19 offset=18
@end

@case an unknown external identifier keyword
@xml
<!DOCTYPE a foo><a/>
@error
SYNTAX 1:13 offset=12
@end

@case a DOCTYPE public identifier may only hold public identifier characters
@xml
<!DOCTYPE a PUBLIC "a^b" "x"><a/>
@error
SYNTAX 1:22 offset=21
@end

@case only whitespace may follow the subset
@xml
<!DOCTYPE a SYSTEM "x" [] junk><a/>
@error
SYNTAX 1:27 offset=26
@end

@case a DOCTYPE name may not be followed by a dollar sign
@xml
<!DOCTYPE a$><a/>
@error
SYNTAX 1:12 offset=11
@end

@case stray text in the subset
@xml
<!DOCTYPE a [ junk ]><a/>
@error
SYNTAX 1:15 offset=14
@end

@case an unknown declaration keyword
@xml
<!DOCTYPE a [<!FOO x>]><a/>
@error
SYNTAX 1:14 offset=13
@end

@case conditional sections are not allowed in an internal subset
@xml
<!DOCTYPE a [<![INCLUDE[]]>]><a/>
@error
SYNTAX 1:14 offset=13
@end

@case a parameter-entity reference ends in a semicolon
@xml
<!DOCTYPE a [%pe]><a/>
@error
SYNTAX 1:17 offset=16
@end

@case a parameter-entity reference needs a name
@xml
<!DOCTYPE a [%;]><a/>
@error
SYNTAX 1:15 offset=14
@end

@case a parameter-entity name may not start with a digit
@xml
<!DOCTYPE a [%1;]><a/>
@error
NAME 1:15 offset=14
@end

@case a double dash inside a subset comment
@xml
<!DOCTYPE a [<!-- a -- b -->]><a/>
@error
SYNTAX 1:23 offset=22
@end

@case a subset comment may not end in three dashes
@xml
<!DOCTYPE a [<!-- a --->]><a/>
@error
SYNTAX 1:23 offset=22
@end

@case the xml target is reserved in the subset
@xml
<!DOCTYPE a [<?xml x?>]><a/>
@error
SYNTAX 1:14 offset=13
@end

@case a subset PI target may not start with a digit
@xml
<!DOCTYPE a [<?1x d?>]><a/>
@error
NAME 1:16 offset=15
@end

@case a subset PI target may not contain a colon
@xml
<!DOCTYPE a [<?p:q d?>]><a/>
@error
NAME 1:17 offset=16
@end

@case a subset PI target is followed by whitespace
@xml
<!DOCTYPE a [<?pi$?>]><a/>
@error
SYNTAX 1:18 offset=17
@end

@case an unterminated internal subset
@xml
<!DOCTYPE a [
@error
UNEXPECTED_EOF 1:14 offset=13
@end
