# Limits are configuration, not heuristics.
#
# Reaching one ends the parse with a TXML_DOC_LIMIT error naming the limit.
# Each case pairs a document that stays inside the limit with one that does
# not, so the boundary itself is pinned.

@case nesting within max_depth is fine
@limits max_depth=2
@xml
<a><b/></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 element {}b end=3 parent=1 depth=2 attributes=0 flags=EMPTY
@names
0 {}a
1 {}b
@end

@case nesting past max_depth stops the parse
@limits max_depth=2
@xml
<a><b><c/></b></a>
@error
LIMIT 1:7 offset=6 limit=DEPTH
@end

@case attributes within max_attributes are fine
@limits max_attributes=2
@xml
<a x="1" y="2"/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}a end=2 parent=0 depth=1 attributes=2 flags=EMPTY
@attributes
1.0 {}x "1"
1.1 {}y "2"
@names
0 {}a
1 {}x
2 {}y
@end

@case one attribute too many stops the parse
@limits max_attributes=2
@xml
<a x="1" y="2" z="3"/>
@error
LIMIT 1:16 offset=15 limit=ATTRIBUTES
@end

@case a name of exactly max_name_length is fine
@limits max_name_length=3
@xml
<abc/>
@nodes
0 document - end=2 parent=- depth=0
1 element {}abc end=2 parent=0 depth=1 attributes=0 flags=EMPTY
@names
0 {}abc
@end

@case a longer name stops the parse
@limits max_name_length=3
@xml
<abcd/>
@error
LIMIT 1:2 offset=1 limit=NAME_LENGTH
@end

@case distinct names within max_names are fine
@limits max_names=2
@xml
<a><b/></a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 element {}b end=3 parent=1 depth=2 attributes=0 flags=EMPTY
@names
0 {}a
1 {}b
@end

@case one distinct name too many stops the parse
@limits max_names=2
@xml
<a><b><c/></b></a>
@error
LIMIT 1:8 offset=7 limit=NAMES
@end

@case a repeated name does not consume the names budget
@limits max_names=2
@xml
<a><b/><b/><b/></a>
@nodes
0 document - end=5 parent=- depth=0
1 element {}a end=5 parent=0 depth=1 attributes=0
2 element {}b end=3 parent=1 depth=2 attributes=0 flags=EMPTY
3 element {}b end=4 parent=1 depth=2 attributes=0 flags=EMPTY
4 element {}b end=5 parent=1 depth=2 attributes=0 flags=EMPTY
@names
0 {}a
1 {}b
@end

# The classic billion-laughs shape: each level multiplies the previous one.
# The budget counts bytes produced by expansion, so the parse stops at the
# reference that would exceed it rather than allocating what it asks for.
@case entity expansion stops at the budget
@flags INTERNAL_SUBSET
@limits entity_expansion_budget=64
@xml
<!DOCTYPE a [
<!ENTITY e0 "xxxxxxxxxx">
<!ENTITY e1 "&e0;&e0;&e0;&e0;&e0;&e0;&e0;&e0;&e0;&e0;">
]>
<a>&e1;</a>
@error
LIMIT 5:4 offset=102 limit=ENTITY_EXPANSION
@end

@case expansion within the budget is fine
@flags INTERNAL_SUBSET
@limits entity_expansion_budget=64
@xml
<!DOCTYPE a [<!ENTITY e "xxxxx">]>
<a>&e;&e;</a>
@nodes
0 document - end=3 parent=- depth=0
1 element {}a end=3 parent=0 depth=1 attributes=0
2 text "xxxxxxxxxx" end=3 parent=1 depth=2
@names
0 {}a
@end
