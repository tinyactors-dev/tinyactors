<?xml version="1.0"?>
<!--
  The W3C SCXML IRP's conf: notation, translated for the "python" datamodel
  of tinyactors-cpython: Python expressions in cond, expr, location and the
  *expr attributes, Python statements in <script>. Ours, not W3C's: it
  follows confEcma.xsl (irp/confEcma.xsl) template by template, in the same
  order, and every template in it has one here.

  What the Python datamodel exposes, as these templates assume it (the
  datamodel is built in ROADMAP.md step 8, M1; where it is not decided yet,
  the plainest Python):
  - one globals dict per session; `<data id="Var1">` is the global Var1;
  - `_event` is a record read by attribute (`_event.name`, `_event.data`);
    a field with no value (`sendid` of an internal event) is None;
  - `_event.data` is the payload's Python value: a map is a dict, read by
    subscript (`_event.data['Var1']`); an event without data has None;
  - a variable declared without a value is None;
  - `_ioprocessors` is a dict of dicts: `_ioprocessors[type]['location']`;
  - `In(stateID)` is a function in the globals.

  Every expression is one line with no leading space, so that Python's
  compile() takes it (`mise run check-languages`). Expressions that must
  fail (conf:illegalExpr, conf:nonBoolean) fail when evaluated, not when
  compiled, so the test sees error.execution whether or not the datamodel
  compiles a chart's expressions when it loads it.
-->
<xsl:stylesheet
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:conf="http://www.w3.org/2005/scxml-conformance"
    xmlns:py="urn:tinyactors:confPython"
    exclude-result-prefixes="xs py"
    version="2.0">

<!-- a Python string literal of $text -->
<xsl:function name="py:quote" as="xs:string">
  <xsl:param name="text" as="xs:string"/>
  <xsl:sequence select="concat('''', replace(replace($text, '\\', '\\\\'), '''', '\\'''), '''')"/>
</xsl:function>

<!-- "N op rest" (op one of = < > <= >=): its parts, with = as == -->
<xsl:variable name="py:comparison" select="'^\s*([0-9]+)([=&lt;&gt;]=?)(.*?)\s*$'"/>

<xsl:function name="py:left" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:sequence select="replace($spec, $py:comparison, '$1')"/>
</xsl:function>

<xsl:function name="py:op" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:variable name="op" select="replace($spec, $py:comparison, '$2')"/>
  <xsl:sequence select="if ($op = '=') then '==' else $op"/>
</xsl:function>

<xsl:function name="py:right" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:sequence select="replace($spec, $py:comparison, '$3')"/>
</xsl:function>

<!-- "N op rest" as "VarN op right", where right is what $kind makes of
     rest: value (as written), quoted (a string literal) or var (VarM) -->
<xsl:function name="py:compare" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:param name="kind" as="xs:string"/>
  <xsl:variable name="right" select="
      if ($kind = 'quoted') then py:quote(py:right($spec))
      else if ($kind = 'var') then concat('Var', py:right($spec))
      else py:right($spec)"/>
  <xsl:sequence select="concat('Var', py:left($spec), ' ', py:op($spec), ' ', $right)"/>
</xsl:function>

<!-- Copy everything that doesn't match other rules -->
<xsl:template match="/ | @* | node()">
  <xsl:copy>
    <xsl:apply-templates select="@* | node()"/>
  </xsl:copy>
</xsl:template>

<!-- Success criteria -->

<xsl:template match="//@conf:targetpass">
  <xsl:attribute name="target">pass</xsl:attribute>
</xsl:template>

<xsl:template match="conf:pass">
 <final xmlns="http://www.w3.org/2005/07/scxml" id="pass">
   <onentry>
     <log label="Outcome" expr="'pass'"/>
   </onentry>
 </final>
</xsl:template>

<!-- Failure criteria -->

<xsl:template match="//@conf:targetfail">
  <xsl:attribute name="target">fail</xsl:attribute>
</xsl:template>

<xsl:template match="conf:fail">
 <final xmlns="http://www.w3.org/2005/07/scxml" id="fail">
   <onentry>
    <log label="Outcome" expr="'fail'"/>
   </onentry>
 </final>
</xsl:template>

<!-- datamodel -->
<xsl:template match="//@conf:datamodel">
  <xsl:attribute name="datamodel">python</xsl:attribute>
</xsl:template>

<!-- creates id for <data> element, etc. -->
<xsl:template match="//@conf:id">
  <xsl:attribute name="id" select="concat('Var', .)"/>
</xsl:template>

<!-- creates name for <param>, etc. -->
<xsl:template match="//@conf:name">
  <xsl:attribute name="name" select="concat('Var', .)"/>
</xsl:template>

<!-- creates location for <assign>, etc. -->
<xsl:template match="//@conf:location">
  <xsl:attribute name="location" select="concat('Var', .)"/>
</xsl:template>

<!-- names an invalid location for <assign>, etc.: foo is not declared -->
<xsl:template match="//@conf:invalidLocation">
  <xsl:attribute name="location">foo.bar.baz</xsl:attribute>
</xsl:template>

<!-- uses a system var as the location for assign, etc. -->
<xsl:template match="//@conf:systemVarLocation">
  <xsl:attribute name="location" select="normalize-space(.)"/>
</xsl:template>

<!-- expr is evaluated -->
<xsl:template match="//@conf:expr">
  <xsl:attribute name="expr" select="normalize-space(.)"/>
</xsl:template>

<!-- targetexpr is the corresponding ID -->
<xsl:template match="//@conf:targetVar">
  <xsl:attribute name="targetexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- expr is quoted -->
<xsl:template match="//@conf:quoteExpr">
  <xsl:attribute name="expr" select="py:quote(.)"/>
</xsl:template>

<!-- an expr that is the value of a variable -->
<xsl:template match="//@conf:varExpr">
  <xsl:attribute name="expr" select="concat('Var', .)"/>
</xsl:template>

<!-- the same as varExpr -->
<xsl:template match="//@conf:varChildExpr">
  <xsl:attribute name="expr" select="concat('Var', .)"/>
</xsl:template>

<!-- an expr that is the value of a system variable -->
<xsl:template match="//@conf:systemVarExpr">
  <xsl:attribute name="expr" select="normalize-space(.)"/>
</xsl:template>

<!-- an expr that is the value of a non-existent substructure of a variable -->
<xsl:template match="//@conf:varNonexistentStruct">
  <xsl:attribute name="expr" select="concat('Var', ., '.bar')"/>
</xsl:template>

<!-- a value that cannot be assigned to a var: evaluating it raises
     ZeroDivisionError (confEcma.xsl's `return` is a syntax error) -->
<xsl:template match="//@conf:illegalExpr">
  <xsl:attribute name="expr">1 / 0</xsl:attribute>
</xsl:template>

<!-- this should add 1 to the value of the specified variable -->
<xsl:template match="conf:incrementID">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id)"/>
    <xsl:attribute name="expr" select="concat('Var', @id, ' + 1')"/>
  </assign>
</xsl:template>

<!-- concatenates the value of id2 to id1 and assigns the result to id1 -->
<xsl:template match="conf:concatVars">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id1)"/>
    <xsl:attribute name="expr" select="concat('Var', @id1, ' + Var', @id2)"/>
  </assign>
</xsl:template>

<!-- assigns the sum of the values of two vars to the first var -->
<xsl:template match="//conf:sumVars">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id1)"/>
    <xsl:attribute name="expr" select="concat('Var', @id1, ' + Var', @id2)"/>
  </assign>
</xsl:template>

<!-- an illegal target for <send>, causing a send error to be raised -->
<xsl:template match="//@conf:illegalTarget">
  <xsl:attribute name="target">baz</xsl:attribute>
</xsl:template>

<!-- a legal, but unreachable, target for <send>, causing a send error to be raised -->
<xsl:template match="//@conf:unreachableTarget">
  <xsl:attribute name="target">#_scxml_foo</xsl:attribute>
</xsl:template>

<!-- illegal content for <send>, causing the message to be rejected -->
<xsl:template match="//conf:illegalContent">
  <content xmlns="http://www.w3.org/2005/07/scxml"> xyz </content>
</xsl:template>

<!-- a content element whose value is the string 'foo' -->
<xsl:template match="//conf:contentFoo">
  <content xmlns="http://www.w3.org/2005/07/scxml">foo</content>
</xsl:template>

<xsl:template match="//conf:someInlineVal">123</xsl:template>

<!-- something that is guaranteed not to be the ID of the current session -->
<xsl:template match="//@conf:invalidSessionID">
  <xsl:attribute name="expr">27</xsl:attribute>
</xsl:template>

<!-- something that is guaranteed not to be a valid event I/O processor -->
<xsl:template match="//@conf:invalidSendType">
  <xsl:attribute name="type">27</xsl:attribute>
</xsl:template>

<!-- same value in an expr -->
<xsl:template match="//@conf:invalidSendTypeExpr">
  <xsl:attribute name="expr">27</xsl:attribute>
</xsl:template>

<!-- exprs that return the value of the event fields -->

<xsl:template match="//@conf:eventName">
  <xsl:attribute name="expr">_event.name</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:eventType">
  <xsl:attribute name="expr">_event.type</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:eventSendid">
  <xsl:attribute name="expr">_event.sendid</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:eventField">
  <xsl:attribute name="expr" select="concat('_event.', normalize-space(.))"/>
</xsl:template>

<!-- returns the raw message structure as a string -->
<xsl:template match="//@conf:eventRaw">
  <xsl:attribute name="expr">_event.raw</xsl:attribute>
</xsl:template>

<!-- the value of the specified item in _event.data -->
<xsl:template match="//@conf:eventDataFieldValue">
  <xsl:attribute name="expr" select="concat('_event.data[', py:quote(normalize-space(.)), ']')"/>
</xsl:template>

<!-- the value of a KVP specified by <param> from _event.data -->
<xsl:template match="//@conf:eventDataParamValue">
  <xsl:attribute name="expr" select="concat('_event.data[', py:quote(normalize-space(.)), ']')"/>
</xsl:template>

<!-- the value of a KVP specified by namelist from _event.data -->
<xsl:template match="//@conf:eventDataNamelistValue">
  <xsl:attribute name="expr" select="concat('_event.data[', py:quote(concat('Var', normalize-space(.))), ']')"/>
</xsl:template>

<!-- the location of the SCXML event I/O processor -->
<xsl:template match="//@conf:scxmlEventIOLocation">
  <xsl:attribute name="expr">_ioprocessors['http://www.w3.org/TR/scxml/#SCXMLEventProcessor']['location']</xsl:attribute>
</xsl:template>

<!-- templates for the expr versions of the send attributes -->

<!-- eventexpr takes the value of the specified variable -->
<xsl:template match="//@conf:eventExpr">
  <xsl:attribute name="eventexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- targetexpr takes the value of the specified variable -->
<xsl:template match="//@conf:targetExpr">
  <xsl:attribute name="targetexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- typeexpr takes the value of the specified variable -->
<xsl:template match="//@conf:typeExpr">
  <xsl:attribute name="typeexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- delayexpr takes the value of the specified variable -->
<xsl:template match="//@conf:delayFromVar">
  <xsl:attribute name="delayexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- a delayexpr for the value passed in, in seconds -->
<xsl:template match="//@conf:delay">
  <xsl:attribute name="delayexpr" select="py:quote(concat(normalize-space(.), 's'))"/>
</xsl:template>

<!-- the specified variable is used as idlocation -->
<xsl:template match="//@conf:idlocation">
  <xsl:attribute name="idlocation" select="concat('Var', .)"/>
</xsl:template>

<!-- the specified variable is used as sendidexpr -->
<xsl:template match="//@conf:sendIDExpr">
  <xsl:attribute name="sendidexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- the specified variable is used as srcexpr -->
<xsl:template match="//@conf:srcExpr">
  <xsl:attribute name="srcexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- the specified variable is used as namelist -->
<xsl:template match="//@conf:namelist">
  <xsl:attribute name="namelist" select="concat('Var', .)"/>
</xsl:template>

<!-- a reference to an invalid namelist, one that should cause an error -->
<xsl:template match="//@conf:invalidNamelist">
  <xsl:attribute name="namelist">&#34;foo</xsl:attribute>
</xsl:template>

<!-- transition conditions -->

<!-- the value is evaluated -->
<xsl:template match="//@conf:idVal">
  <xsl:attribute name="cond" select="py:compare(., 'value')"/>
</xsl:template>

<!-- compare two variables -->
<xsl:template match="//@conf:varIdVal">
  <xsl:attribute name="cond" select="py:compare(., 'var')"/>
</xsl:template>

<!-- a var whose value was passed in via namelist has the specified value
     (confEcma.xsl: ===) -->
<xsl:template match="//@conf:namelistIdVal">
  <xsl:attribute name="cond" select="py:compare(., 'value')"/>
</xsl:template>

<!-- true if the two vars have the same value (confEcma.xsl: ===) -->
<xsl:template match="//@conf:VarEqVar">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^([0-9]+)\W+([0-9]+)$', 'Var$1 == Var$2')"/>
</xsl:template>

<!-- true if the two vars have the same value, which is a structure:
     Python's == compares dicts and lists by value -->
<xsl:template match="//@conf:VarEqVarStruct">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^([0-9]+)\W+([0-9]+)$', 'Var$1 == Var$2')"/>
</xsl:template>

<!-- the value is quoted -->
<xsl:template match="//@conf:idQuoteVal">
  <xsl:attribute name="cond" select="py:compare(., 'quoted')"/>
</xsl:template>

<!-- test on the value of two vars -->
<xsl:template match="//@conf:compareIDVal">
  <xsl:attribute name="cond" select="py:compare(., 'var')"/>
</xsl:template>

<!-- the specified var has the value of <conf:someInlineVal> -->
<xsl:template match="//@conf:idSomeVal">
  <xsl:attribute name="cond" select="concat('Var', ., ' == 123')"/>
</xsl:template>

<!-- the event's name field has the value specified -->
<xsl:template match="//@conf:eventNameVal">
  <xsl:attribute name="cond" select="concat('_event.name == ', py:quote(normalize-space(.)))"/>
</xsl:template>

<!-- the field VarN of _event.data compared with a value -->
<xsl:template match="//@conf:eventvarVal">
  <xsl:attribute name="cond" select="concat('_event.data[', py:quote(concat('Var', py:left(.))), '] ', py:op(.), ' ', py:right(.))"/>
</xsl:template>

<!-- a variable compared with a system variable (the system variable's name
     is the right side) -->
<xsl:template match="//@conf:idSystemVarVal">
  <xsl:attribute name="cond" select="py:compare(., 'value')"/>
</xsl:template>

<!-- _event.data equals the specified value -->
<xsl:template match="//@conf:eventdataVal">
  <xsl:attribute name="cond" select="concat('_event.data == ', normalize-space(.))"/>
</xsl:template>

<!-- _event.data is the value of <conf:someInlineVal> -->
<xsl:template match="//@conf:eventdataSomeVal">
  <xsl:attribute name="cond">_event.data == 123</xsl:attribute>
</xsl:template>

<!-- the event has no data -->
<xsl:template match="//@conf:emptyEventData">
  <xsl:attribute name="cond">_event.data is None</xsl:attribute>
</xsl:template>

<!-- the _name system variable has the specified value -->
<xsl:template match="//@conf:nameVarVal">
  <xsl:attribute name="cond" select="concat('_name == ', py:quote(normalize-space(.)))"/>
</xsl:template>

<!-- "n m": the value of VarN is a prefix of the value of VarM (confEcma.xsl
     passes Var(group 3) as the string and Var(group 1) as the prefix) -->
<xsl:template match="//@conf:varPrefix">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\w+)\W(\w+)$', 'str(Var$2).startswith(str(Var$1))')"/>
</xsl:template>

<xsl:template match="//@conf:inState">
  <xsl:attribute name="cond" select="concat('In(', py:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- a cond that cannot be read as a boolean: in Python every value can, so
     it raises when evaluated, as test 344 asks -->
<xsl:template match="//@conf:nonBoolean">
  <xsl:attribute name="cond">1 / 0</xsl:attribute>
</xsl:template>

<!-- true if the id has a value -->
<xsl:template match="//@conf:isBound">
  <xsl:attribute name="cond" select="concat('Var', ., ' is not None')"/>
</xsl:template>

<!-- true if the var has been created but is not bound -->
<xsl:template match="//@conf:unboundVar">
  <xsl:attribute name="cond" select="concat('Var', ., ' is None')"/>
</xsl:template>

<!-- true if the system var has a value -->
<xsl:template match="//@conf:systemVarIsBound">
  <xsl:attribute name="cond" select="normalize-space(.)"/>
</xsl:template>

<!-- true if the id has no value -->
<xsl:template match="//@conf:noValue">
  <xsl:attribute name="cond" select="concat('Var', ., ' is None')"/>
</xsl:template>

<!-- always true -->
<xsl:template match="//@conf:true">
  <xsl:attribute name="cond">True</xsl:attribute>
</xsl:template>

<!-- always false -->
<xsl:template match="//@conf:false">
  <xsl:attribute name="cond">False</xsl:attribute>
</xsl:template>

<!-- true if all the required fields of _event are bound -->
<xsl:template match="//@conf:eventFieldsAreBound">
  <xsl:attribute name="cond">hasattr(_event, 'name') and hasattr(_event, 'type') and hasattr(_event, 'sendid') and hasattr(_event, 'origin') and hasattr(_event, 'origintype') and hasattr(_event, 'invokeid') and hasattr(_event, 'data')</xsl:attribute>
</xsl:template>

<!-- true if _event.data has the specified field -->
<xsl:template match="//@conf:eventDataHasField">
  <xsl:attribute name="cond" select="concat(py:quote(normalize-space(.)), ' in _event.data')"/>
</xsl:template>

<!-- true if the specified field of _event has no value -->
<xsl:template match="//@conf:eventFieldHasNoValue">
  <xsl:attribute name="cond" select="concat('_event.', normalize-space(.), ' is None')"/>
</xsl:template>

<!-- true if the language of _event matches the processor's datamodel -->
<xsl:template match="//@conf:eventLanguageMatchesDatamodel">
  <xsl:attribute name="cond">_event.language == 'python'</xsl:attribute>
</xsl:template>

<!-- true if _event was delivered on the specified I/O processor -->
<xsl:template match="//@conf:originTypeEq">
  <xsl:attribute name="cond" select="concat('_event.origintype == ', py:quote(normalize-space(.)))"/>
</xsl:template>

<!-- scripting -->

<xsl:template match="conf:script">
 <script xmlns="http://www.w3.org/2005/07/scxml">Var1 = 1</script>
</xsl:template>

<xsl:template match="//@conf:scriptGoodSrc">
  <xsl:attribute name="src">D:\W3C\SCXMLTests\test300.py</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:scriptBadSrc">
  <xsl:attribute name="src">D:\foo</xsl:attribute>
</xsl:template>

<!-- sends an event back to the sender of the current event -->
<xsl:template match="conf:sendToSender">
 <send xmlns="http://www.w3.org/2005/07/scxml">
   <xsl:attribute name="event" select="@name"/>
   <xsl:attribute name="targetexpr">_event.origin</xsl:attribute>
   <xsl:attribute name="typeexpr">_event.origintype</xsl:attribute>
 </send>
</xsl:template>

<!-- foreach -->
<!-- an array containing 1 2 3 in that order -->
<xsl:template match="//conf:array123">[1, 2, 3]</xsl:template>

<!-- the value of the indicated variable as the 'array' attribute -->
<xsl:template match="//@conf:arrayVar">
  <xsl:attribute name="array" select="concat('Var', .)"/>
</xsl:template>

<!-- the same as arrayVar -->
<xsl:template match="//@conf:arrayTextVar">
  <xsl:attribute name="array" select="concat('Var', .)"/>
</xsl:template>

<!-- an expr that yields an array containing 1 2 3 in that order -->
<xsl:template match="//@conf:arrayExpr123">
  <xsl:attribute name="expr">[1, 2, 3]</xsl:attribute>
</xsl:template>

<!-- an expr that is not a valid array: an int is not iterable -->
<xsl:template match="//@conf:illegalArray">
  <xsl:attribute name="expr">7</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:item">
  <xsl:attribute name="item" select="concat('Var', .)"/>
</xsl:template>

<!-- something that cannot be a variable name -->
<xsl:template match="//@conf:illegalItem">
  <xsl:attribute name="item">'continue'</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:index">
  <xsl:attribute name="index" select="concat('Var', .)"/>
</xsl:template>

<!-- adds an item to the end of the specified array, a new list -->
<xsl:template match="conf:extendArray">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id)"/>
    <xsl:attribute name="expr" select="concat('Var', @id, ' + [4]')"/>
  </assign>
</xsl:template>

<!-- a multidimensional array all of whose cells are the specified value
     (not used by any test) -->
<xsl:template match="//@conf:multiDimensionalArrayExpr">
  <xsl:attribute name="expr" select="concat('[[', ., ', ', ., '], [', ., ', ', ., ']]')"/>
</xsl:template>

<!-- a <foreach> that increments the cells of the specified array (not used
     by any test) -->
<xsl:template match="conf:incrementArray">
  <xsl:variable name="array" select="concat('Var', @id)"/>
  <foreach xmlns="http://www.w3.org/2005/07/scxml" item="item" index="index">
    <xsl:attribute name="array" select="$array"/>
    <script><xsl:value-of select="concat($array, '[index][0] = ', $array, '[index][0] + 1&#10;', $array, '[index][1] = ', $array, '[index][1] + 1')"/></script>
  </foreach>
</xsl:template>

<!-- true iff each cell of the specified multidimensional array has the
     specified value (not used by any test) -->
<xsl:template match="//@conf:arrayVal">
  <xsl:variable name="array" select="replace(normalize-space(.), '^([0-9]+)\W+(.*)$', 'Var$1')"/>
  <xsl:variable name="value" select="replace(normalize-space(.), '^([0-9]+)\W+(.*)$', '$2')"/>
  <xsl:attribute name="cond" select="concat('all(cell == ', $value, ' for row in ', $array, ' for cell in row)')"/>
</xsl:template>

<!-- The Basic HTTP Event I/O Processor (as in confEcma.xsl, the site's
     address is SITE_SPECIFIC_ADDRESS) -->

<xsl:template match="//@conf:testOnServer">
  <xsl:attribute name="target" select="concat('SITE_SPECIFIC_ADDRESS', .)"/>
</xsl:template>

<!-- nothing to set up: the expressions below need no module -->
<xsl:template match="conf:setUpHTTPTest"/>

<!-- an expression evaluating to the Basic HTTP access URI -->
<xsl:template match="//@conf:basicHTTPAccessURI">
  <xsl:attribute name="expr">_ioprocessors['basichttp']['location']</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:basicHTTPAccessURITarget">
  <xsl:attribute name="targetexpr">_ioprocessors['basichttp']['location']</xsl:attribute>
</xsl:template>

<!-- true if POST was used to send the message -->
<xsl:template match="//@conf:methodIsPost">
  <xsl:attribute name="cond">'POST' in _event.raw</xsl:attribute>
</xsl:template>

<!-- a namelist of all the ids listed -->
<xsl:template match="//@conf:multipleNamelist">
  <xsl:attribute name="namelist" select="string-join(for $id in tokenize(normalize-space(.), '\D+')[. != ''] return concat('Var', $id), ' ')"/>
</xsl:template>

<!-- true if the event is external -->
<xsl:template match="//@conf:eventIsExternal">
  <xsl:attribute name="cond">_event.type == 'external'</xsl:attribute>
</xsl:template>

<!-- "n v": _event.raw holds VarN=v (confEcma.xsl searches for the same
     text with a regular expression) -->
<xsl:template match="//@conf:eventIdParamHasValue">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\S+)\s+(\S+)$', concat(py:quote('Var$1=$2'), ' in _event.raw'))"/>
</xsl:template>

<!-- "p v": _event.raw holds p=v -->
<xsl:template match="//@conf:eventNamedParamHasValue">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\S+)\s+(\S+)$', concat(py:quote('$1=$2'), ' in _event.raw'))"/>
</xsl:template>

<!-- _event.raw holds the text -->
<xsl:template match="//@conf:messageBodyEquals">
  <xsl:attribute name="cond" select="concat(py:quote(normalize-space(.)), ' in _event.raw')"/>
</xsl:template>

<!-- exprs that take an item out of _event.raw: the value after p= on the
     last line (not used by any automatic test) -->
<xsl:template match="//@conf:getNamedParamVal">
  <xsl:attribute name="expr" select="concat('_event.raw.rsplit(', py:quote(concat(normalize-space(.), '=')), ', 1)[1].split()[0]')"/>
</xsl:template>

<xsl:template match="//@conf:getIDParamVal">
  <xsl:attribute name="expr" select="concat('_event.raw.rsplit(', py:quote(concat('Var', normalize-space(.), '=')), ', 1)[1].split()[0]')"/>
</xsl:template>

<!-- the name and the value of the Nth parameter of the body (not used by
     any test) -->
<xsl:template match="//@conf:getNthParamName">
  <xsl:attribute name="expr" select="concat('_event.raw.split(''\n\n'', 1)[1].split(''&amp;'')[', ., '].split(''='')[0]')"/>
</xsl:template>

<xsl:template match="//@conf:getNthParamVal">
  <xsl:attribute name="expr" select="concat('_event.raw.split(''\n\n'', 1)[1].split(''&amp;'')[', ., '].split(''='')[1]')"/>
</xsl:template>

<!-- an expr that evaluates to an SCXML message (not used by any test;
     confEcma.xsl's version is not a valid expression) -->
<xsl:template match="//@conf:scxmlEventExpr">
  <xsl:attribute name="expr" select="concat('''&lt;message source=&quot;'' + _ioprocessors[''basichttp''][''location''] + ''&quot; sourcetype=&quot;scxml&quot; name=&quot;'' + str(', ., ') + ''&quot; xmlns=&quot;http://www.w3.org/2005/07/scxml&quot; version=&quot;1.0&quot;/&gt;''')"/>
</xsl:template>

<!-- a <content> with the message body, which holds the address the far
     side answers to -->
<xsl:template match="conf:msgContent">
  <content xmlns="http://www.w3.org/2005/07/scxml" expr="'address=' + _ioprocessors['basichttp']['location']"/>
</xsl:template>

<!-- true if the body is the one conf:msgContent makes -->
<xsl:template match="//@conf:msgIsBody">
  <xsl:attribute name="cond">'\n\naddress=' in _event.raw</xsl:attribute>
</xsl:template>

</xsl:stylesheet>
