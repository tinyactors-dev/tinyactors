# Round-trips: parse, then write the document back with txml_write_subtree().
#
# The writer runs with TXML_WRITE_EMPTY_TAGS and nothing else, so `<a/>` stays
# `<a/>`. Because the parse keeps every construct (ADR 0017), what survives is
# now everything the document is made of: names, prefixes, attribute order,
# namespace declarations, CDATA, comments, PIs, whitespace, the XML
# declaration and the doctype. What does not survive is which entity reference
# was used, CRLF, quote style, and spacing inside a tag — those are normalized
# on the way in and cannot be recovered.
#
# A declaration and a doctype have no writer event of their own: the event API
# is start/attribute/text/cdata/comment/pi/end (ADR 0011). txml_write_subtree
# emits them directly from the document, which is what keeps these two cases
# exact. TXML_WRITE_DECLARATION therefore means "emit one even when the
# document has no declaration node", and never doubles an existing one.

@case elements and attributes
@xml
<a><b x="1"/></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 element {}b end=3 parent=1 depth=2 attributes=1 flags=EMPTY
@roundtrip
<a><b x="1"/></a>
@end

@case text escaping is restored
@xml
<a>a &lt; b &amp; c &gt; d</a>
@roundtrip
<a>a &lt; b &amp; c &gt; d</a>
@end

@case attribute escaping is restored
@xml
<a k="a&quot;b&lt;c&amp;d"/>
@roundtrip
<a k="a&quot;b&lt;c&amp;d"/>
@end

@case prefixed namespaces round-trip
@xml
<p:a xmlns:p="urn:x"><p:b/></p:a>
@roundtrip
<p:a xmlns:p="urn:x"><p:b/></p:a>
@end

@case a default namespace round-trips
@xml
<a xmlns="urn:x"><b/></a>
@roundtrip
<a xmlns="urn:x"><b/></a>
@end

@case a scoped redeclaration round-trips
@xml
<a xmlns="urn:1"><b xmlns="urn:2"><c/></b><d/></a>
@roundtrip
<a xmlns="urn:1"><b xmlns="urn:2"><c/></b><d/></a>
@end

@case CDATA round-trips
@xml
<a><![CDATA[x<y]]></a>
@roundtrip
<a><![CDATA[x<y]]></a>
@end

@case comments and PIs round-trip
@xml
<a><!--c--><?p d?></a>
@roundtrip
<a><!--c--><?p d?></a>
@end

@case an XML declaration round-trips
@xml
<?xml version="1.0" encoding="UTF-8"?><a/>
@roundtrip
<?xml version="1.0" encoding="UTF-8"?><a/>
@end

@case a doctype round-trips
@xml
<!DOCTYPE a SYSTEM "a.dtd"><a/>
@roundtrip
<!DOCTYPE a SYSTEM "a.dtd"><a/>
@end

@case an internal subset round-trips as written
@xml
<!DOCTYPE a [<!ENTITY e "v">]><a k="&e;"/>
@roundtrip
<!DOCTYPE a [<!ENTITY e "v">]><a k="v"/>
@end

@case whitespace between elements is preserved
@xml
<a>
  <b/>
</a>
@roundtrip
<a>
  <b/>
</a>
@end

@case whitespace in the prolog and epilog is preserved
@xml
\n<a/>\n
@roundtrip
\n<a/>\n
@end

@case an element pair with no children stays a pair
@xml
<a></a>
@roundtrip
<a></a>
@end

@case a reference is replaced by the character it denotes
@chunk 5
@xml
<a>&#65;&amp;&#x42;</a>
@roundtrip
<a>A&amp;B</a>
@end

@case mixed content round-trips
@xml
<p>one<b>two</b>three</p>
@roundtrip
<p>one<b>two</b>three</p>
@end
