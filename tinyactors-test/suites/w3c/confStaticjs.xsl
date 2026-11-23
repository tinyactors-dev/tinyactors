<?xml version="1.0"?>
<!--
  The W3C SCXML IRP's conf: notation, translated for the "staticjs"
  datamodel (tinyactors-core/src/datamodel_staticjs): staticjs expressions
  in cond, expr, the *expr attributes and child content, paths in
  location, idlocation and a <foreach>'s item and index. Ours, not W3C's:
  it follows confEcma.xsl (irp/confEcma.xsl) template by template, in the
  same order, as confPython.xsl and confJSON.xsl do, and every template in
  it has one here.

  staticjs is a pure subset of JavaScript (staticjs/docs/guide/language.md):
  no ==, no !=, no var, let or assignment in an expression, no loops; ===
  on two objects or arrays faults; + of a string and an object faults, as
  does < on values that are not two numbers or two strings; reading a
  property an object lacks gives undefined, but for the names of
  Object.prototype, which fault. So where confEcma.xsl writes == it writes
  ===, and where it compares structures (or a variable with _event or
  _ioprocessors) it compares their JSON.stringify texts.

  What the staticjs datamodel exposes, as these templates assume it:
  - every expression is one staticjs expression (SJS_GOAL_EXPRESSION, read
    as JavaScript reads `(` source `)`), compiled with these globals only:
    the chart's <data> ids, the names a <foreach>'s item and index bind,
    `_event`, `_sessionid`, `_name`, `_ioprocessors` and `In` (In('s') is
    true when state s is active); a name that is none of these is refused
    when compiled, and so is a syntax error: the chart still loads, and
    the expression's evaluation is error.execution (W3C 156, 312, 487);
  - a <data> declared without a value is undefined;
  - a location is a path: a variable, then `.name` or `[expression]`
    steps; assigning to `_event` or a system variable is error.execution;
    a write builds the changed value (values are immutable) and may append
    at an array's length;
  - a cond is read with ECMAScript's ToBoolean (undefined, null, false, 0,
    NaN and '' are false); an expression that cannot be compiled is
    error.execution, so false (W3C 309, 344);
  - `_event` is an object with all of the keys name, type, sendid, origin,
    origintype, invokeid, raw and data; a field with no value is undefined
    (the key is there: `'sendid' in _event` holds for an internal event,
    W3C 330); `_event.data` is the payload's value (an object for <param>s
    and a namelist, the value for <content>), undefined without data;
    before the first event `_event` is undefined (W3C 319); `origin` is
    text, as are the other fields that have a value;
  - `_ioprocessors` is an object keyed by each processor's type URI and by
    the short names 'scxml' and 'basichttp', each {location};
    `_sessionid` and `_name` are strings;
  - child content of <data>, <assign> and <content> that compiles as an
    expression is its value ([1,2,3], 123, a quoted string), else its
    whitespace-normalized text as a string (`foo`, an unknown name, is the
    string 'foo'); XML children are not supported;
  - `<script>` is always error.execution: staticjs has no statement that
    changes state, and <assign> is the datamodel's only write.

  Every expression is one line with no leading space. Expressions that
  must fail (conf:illegalExpr, conf:nonBoolean) are `return`, which
  staticjs refuses when compiled (an expression program holds no
  statement), so their evaluation is error.execution.
-->
<xsl:stylesheet
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:conf="http://www.w3.org/2005/scxml-conformance"
    xmlns:sjs="urn:tinyactors:confStaticjs"
    exclude-result-prefixes="xs sjs"
    version="2.0">

<!-- a string literal of $text, in single quotes -->
<xsl:function name="sjs:quote" as="xs:string">
  <xsl:param name="text" as="xs:string"/>
  <xsl:sequence select="concat('''', replace(replace($text, '\\', '\\\\'), '''', '\\'''), '''')"/>
</xsl:function>

<!-- the member access of the property $key: .key when it is an
     identifier, else ['key'] -->
<xsl:function name="sjs:key" as="xs:string">
  <xsl:param name="key" as="xs:string"/>
  <xsl:sequence select="
      if (matches($key, '^[A-Za-z_$][A-Za-z0-9_$]*$')) then concat('.', $key)
      else concat('[', sjs:quote($key), ']')"/>
</xsl:function>

<!-- "N op rest" (op one of = < > <= >=): its parts, with = as === -->
<xsl:variable name="sjs:comparison" select="'^\s*([0-9]+)([=&lt;&gt;]=?)(.*?)\s*$'"/>

<xsl:function name="sjs:left" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:sequence select="replace($spec, $sjs:comparison, '$1')"/>
</xsl:function>

<xsl:function name="sjs:op" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:variable name="op" select="replace($spec, $sjs:comparison, '$2')"/>
  <xsl:sequence select="if ($op = ('=', '==')) then '===' else $op"/>
</xsl:function>

<xsl:function name="sjs:right" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:sequence select="replace($spec, $sjs:comparison, '$3')"/>
</xsl:function>

<!-- "N op rest" as "VarN op right", where right is what $kind makes of
     rest: value (as written), quoted (a string literal) or var (VarM) -->
<xsl:function name="sjs:compare" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:param name="kind" as="xs:string"/>
  <xsl:variable name="right" select="
      if ($kind = 'quoted') then sjs:quote(sjs:right($spec))
      else if ($kind = 'var') then concat('Var', sjs:right($spec))
      else sjs:right($spec)"/>
  <xsl:sequence select="concat('Var', sjs:left($spec), ' ', sjs:op($spec), ' ', $right)"/>
</xsl:function>

<!-- whether two values have the same JSON text: === on two objects or
     arrays faults in staticjs -->
<xsl:function name="sjs:same" as="xs:string">
  <xsl:param name="a" as="xs:string"/>
  <xsl:param name="b" as="xs:string"/>
  <xsl:sequence select="concat('JSON.stringify(', $a, ') === JSON.stringify(', $b, ')')"/>
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
  <xsl:attribute name="datamodel">staticjs</xsl:attribute>
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

<!-- names an invalid location for <assign>, etc.: foo is no variable of
     the chart, so the path is refused -->
<xsl:template match="//@conf:invalidLocation">
  <xsl:attribute name="location">foo.bar.baz</xsl:attribute>
</xsl:template>

<!-- uses a system var as the location for assign, etc.: a write to it is
     error.execution -->
<xsl:template match="//@conf:systemVarLocation">
  <xsl:attribute name="location" select="normalize-space(.)"/>
</xsl:template>

<!-- expr is evaluated (the tests give integers, and _event) -->
<xsl:template match="//@conf:expr">
  <xsl:attribute name="expr" select="normalize-space(.)"/>
</xsl:template>

<!-- targetexpr is the corresponding ID -->
<xsl:template match="//@conf:targetVar">
  <xsl:attribute name="targetexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- expr is quoted -->
<xsl:template match="//@conf:quoteExpr">
  <xsl:attribute name="expr" select="sjs:quote(.)"/>
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

<!-- an expr that is the value of a non-existent substructure of a
     variable: undefined for an object, a fault for a number (W3C 307, a
     manual test, logs either) -->
<xsl:template match="//@conf:varNonexistentStruct">
  <xsl:attribute name="expr" select="concat('Var', ., '.bar')"/>
</xsl:template>

<!-- a value that cannot be assigned to a var: `return` is no expression,
     refused when compiled, so its evaluation is error.execution -->
<xsl:template match="//@conf:illegalExpr">
  <xsl:attribute name="expr">return</xsl:attribute>
</xsl:template>

<!-- this should add 1 to the value of the specified variable -->
<xsl:template match="conf:incrementID">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id)"/>
    <xsl:attribute name="expr" select="concat('Var', @id, ' + 1')"/>
  </assign>
</xsl:template>

<!-- concatenates the value of id2 to id1 and assigns the result to id1
     (the tests and the scenarios concatenate text) -->
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

<!-- a content element whose value is the string 'foo': foo is no name of
     the chart, so the content is that text -->
<xsl:template match="//conf:contentFoo">
  <content xmlns="http://www.w3.org/2005/07/scxml">foo</content>
</xsl:template>

<!-- the number 123 -->
<xsl:template match="//conf:someInlineVal">123</xsl:template>

<!-- something that is guaranteed not to be the ID of the current session
     (the tests only assign it, as an initial value or to _sessionid) -->
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
  <xsl:attribute name="expr" select="concat('_event', sjs:key(normalize-space(.)))"/>
</xsl:template>

<!-- returns the raw message structure as a string -->
<xsl:template match="//@conf:eventRaw">
  <xsl:attribute name="expr">_event.raw</xsl:attribute>
</xsl:template>

<!-- the value of the specified item in _event.data -->
<xsl:template match="//@conf:eventDataFieldValue">
  <xsl:attribute name="expr" select="concat('_event.data', sjs:key(normalize-space(.)))"/>
</xsl:template>

<!-- the value of a KVP specified by <param> from _event.data -->
<xsl:template match="//@conf:eventDataParamValue">
  <xsl:attribute name="expr" select="concat('_event.data', sjs:key(normalize-space(.)))"/>
</xsl:template>

<!-- the value of a KVP specified by namelist from _event.data -->
<xsl:template match="//@conf:eventDataNamelistValue">
  <xsl:attribute name="expr" select="concat('_event.data', sjs:key(concat('Var', normalize-space(.))))"/>
</xsl:template>

<!-- the location of the SCXML event I/O processor -->
<xsl:template match="//@conf:scxmlEventIOLocation">
  <xsl:attribute name="expr">_ioprocessors['http://www.w3.org/TR/scxml/#SCXMLEventProcessor'].location</xsl:attribute>
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
  <xsl:attribute name="delayexpr" select="sjs:quote(concat(normalize-space(.), 's'))"/>
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

<!-- a reference to an invalid namelist, one that should cause an error:
     no name of the chart -->
<xsl:template match="//@conf:invalidNamelist">
  <xsl:attribute name="namelist">&#34;foo</xsl:attribute>
</xsl:template>

<!-- transition conditions -->

<!-- the value is evaluated -->
<xsl:template match="//@conf:idVal">
  <xsl:attribute name="cond" select="sjs:compare(., 'value')"/>
</xsl:template>

<!-- compare two variables -->
<xsl:template match="//@conf:varIdVal">
  <xsl:attribute name="cond" select="sjs:compare(., 'var')"/>
</xsl:template>

<!-- a var whose value was passed in via namelist has the specified value -->
<xsl:template match="//@conf:namelistIdVal">
  <xsl:attribute name="cond" select="sjs:compare(., 'value')"/>
</xsl:template>

<!-- true if the two vars have the same value (the tests compare text) -->
<xsl:template match="//@conf:VarEqVar">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^([0-9]+)\W+([0-9]+)$', 'Var$1 === Var$2')"/>
</xsl:template>

<!-- true if the two vars have the same value, which is a structure: the
     same JSON text (=== on two objects faults) -->
<xsl:template match="//@conf:VarEqVarStruct">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^([0-9]+)\W+([0-9]+)$', 'JSON.stringify(Var$1) === JSON.stringify(Var$2)')"/>
</xsl:template>

<!-- the value is quoted -->
<xsl:template match="//@conf:idQuoteVal">
  <xsl:attribute name="cond" select="sjs:compare(., 'quoted')"/>
</xsl:template>

<!-- test on the value of two vars (confEcma.xsl writes = for =, which
     would be an assignment) -->
<xsl:template match="//@conf:compareIDVal">
  <xsl:attribute name="cond" select="sjs:compare(., 'var')"/>
</xsl:template>

<!-- the specified var has the value of <conf:someInlineVal> -->
<xsl:template match="//@conf:idSomeVal">
  <xsl:attribute name="cond" select="concat('Var', ., ' === 123')"/>
</xsl:template>

<!-- the event's name field has the value specified -->
<xsl:template match="//@conf:eventNameVal">
  <xsl:attribute name="cond" select="concat('_event.name === ', sjs:quote(normalize-space(.)))"/>
</xsl:template>

<!-- the field VarN of _event.data compared with a value -->
<xsl:template match="//@conf:eventvarVal">
  <xsl:attribute name="cond" select="concat('_event.data', sjs:key(concat('Var', sjs:left(.))), ' ', sjs:op(.), ' ', sjs:right(.))"/>
</xsl:template>

<!-- a variable compared with a system variable (its name is the right
     side): the same JSON text for =, since _event and _ioprocessors are
     objects (W3C 329) -->
<xsl:template match="//@conf:idSystemVarVal">
  <xsl:attribute name="cond" select="
      if (sjs:op(.) = '===') then sjs:same(concat('Var', sjs:left(.)), sjs:right(.))
      else sjs:compare(., 'value')"/>
</xsl:template>

<!-- _event.data equals the specified value (a literal: 'foo', 123, 21) -->
<xsl:template match="//@conf:eventdataVal">
  <xsl:attribute name="cond" select="concat('_event.data === ', normalize-space(.))"/>
</xsl:template>

<!-- _event.data is the value of <conf:someInlineVal> -->
<xsl:template match="//@conf:eventdataSomeVal">
  <xsl:attribute name="cond">_event.data === 123</xsl:attribute>
</xsl:template>

<!-- the event has no data -->
<xsl:template match="//@conf:emptyEventData">
  <xsl:attribute name="cond">_event.data === undefined</xsl:attribute>
</xsl:template>

<!-- the _name system variable has the specified value -->
<xsl:template match="//@conf:nameVarVal">
  <xsl:attribute name="cond" select="concat('_name === ', sjs:quote(normalize-space(.)))"/>
</xsl:template>

<!-- "n m": the value of VarN is a prefix of the value of VarM (confEcma.xsl
     passes Var(group 3) as the string and Var(group 1) as the prefix, each
     through String) -->
<xsl:template match="//@conf:varPrefix">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\w+)\W(\w+)$', 'String(Var$2).startsWith(String(Var$1))')"/>
</xsl:template>

<xsl:template match="//@conf:inState">
  <xsl:attribute name="cond" select="concat('In(', sjs:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- a cond that cannot be read as a boolean: every staticjs value has a
     truthiness, so an expression refused when compiled, whose evaluation
     is error.execution (W3C 309, 344) -->
<xsl:template match="//@conf:nonBoolean">
  <xsl:attribute name="cond">return</xsl:attribute>
</xsl:template>

<!-- true if the id has a value: a variable the chart does not declare is
     refused, so error.execution, so false (W3C 245) -->
<xsl:template match="//@conf:isBound">
  <xsl:attribute name="cond" select="concat('Var', ., ' !== undefined')"/>
</xsl:template>

<!-- true if the var has been created but is not bound -->
<xsl:template match="//@conf:unboundVar">
  <xsl:attribute name="cond" select="concat('Var', ., ' === undefined')"/>
</xsl:template>

<!-- true if the system var has a value: _event is undefined before the
     first event (W3C 319) -->
<xsl:template match="//@conf:systemVarIsBound">
  <xsl:attribute name="cond" select="concat(normalize-space(.), ' !== undefined')"/>
</xsl:template>

<!-- true if the id has no value (W3C 351: the sendid of an internal
     event, undefined) -->
<xsl:template match="//@conf:noValue">
  <xsl:attribute name="cond" select="concat('!Var', .)"/>
</xsl:template>

<!-- always true -->
<xsl:template match="//@conf:true">
  <xsl:attribute name="cond">true</xsl:attribute>
</xsl:template>

<!-- always false -->
<xsl:template match="//@conf:false">
  <xsl:attribute name="cond">false</xsl:attribute>
</xsl:template>

<!-- true if all the required fields of _event are bound: each a key of
     _event, undefined when it has no value -->
<xsl:template match="//@conf:eventFieldsAreBound">
  <xsl:attribute name="cond">'name' in _event &amp;&amp; 'type' in _event &amp;&amp; 'sendid' in _event &amp;&amp; 'origin' in _event &amp;&amp; 'origintype' in _event &amp;&amp; 'invokeid' in _event &amp;&amp; 'data' in _event</xsl:attribute>
</xsl:template>

<!-- true if _event.data has the specified field (confEcma.xsl leaves the
     name unquoted, a variable's value; not used by any test) -->
<xsl:template match="//@conf:eventDataHasField">
  <xsl:attribute name="cond" select="concat(sjs:quote(normalize-space(.)), ' in _event.data')"/>
</xsl:template>

<!-- true if the specified field of _event has no value -->
<xsl:template match="//@conf:eventFieldHasNoValue">
  <xsl:attribute name="cond" select="concat('_event', sjs:key(normalize-space(.)), ' === undefined')"/>
</xsl:template>

<!-- true if the language of _event matches the processor's datamodel (not
     used by any test; _event has no language field, so undefined, so
     false) -->
<xsl:template match="//@conf:eventLanguageMatchesDatamodel">
  <xsl:attribute name="cond">_event.language === 'staticjs'</xsl:attribute>
</xsl:template>

<!-- true if _event was delivered on the specified I/O processor -->
<xsl:template match="//@conf:originTypeEq">
  <xsl:attribute name="cond" select="concat('_event.origintype === ', sjs:quote(normalize-space(.)))"/>
</xsl:template>

<!-- scripting: staticjs has no statement that changes state, so a
     <script> is error.execution wherever it is; the tests whose scripts
     must change the datamodel (302, 303, 304) are in skip-staticjs.txt -->

<xsl:template match="conf:script">
 <script xmlns="http://www.w3.org/2005/07/scxml"/>
</xsl:template>

<xsl:template match="//@conf:scriptGoodSrc">
  <xsl:attribute name="src">D:\W3C\SCXMLTests\test300.js</xsl:attribute>
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
<!-- an array containing 1 2 3 in that order: content that compiles as an
     expression -->
<xsl:template match="//conf:array123">[1, 2, 3]</xsl:template>

<!-- the value of the indicated variable as the 'array' attribute -->
<xsl:template match="//@conf:arrayVar">
  <xsl:attribute name="array" select="concat('Var', .)"/>
</xsl:template>

<!-- the same as arrayVar -->
<xsl:template match="//@conf:arrayTextVar">
  <xsl:attribute name="array" select="concat('Var', .)"/>
</xsl:template>

<!-- an expr that yields an array containing 1 2 3 in that order
     (confEcma.xsl's lacks its opening bracket) -->
<xsl:template match="//@conf:arrayExpr123">
  <xsl:attribute name="expr">[1, 2, 3]</xsl:attribute>
</xsl:template>

<!-- an expr that is not a valid array: <foreach> of a number is
     error.execution -->
<xsl:template match="//@conf:illegalArray">
  <xsl:attribute name="expr">7</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:item">
  <xsl:attribute name="item" select="concat('Var', .)"/>
</xsl:template>

<!-- something that cannot be a variable name: a string literal is no
     path -->
<xsl:template match="//@conf:illegalItem">
  <xsl:attribute name="item">'continue'</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:index">
  <xsl:attribute name="index" select="concat('Var', .)"/>
</xsl:template>

<!-- adds an item to the end of the specified array, a new array -->
<xsl:template match="conf:extendArray">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id)"/>
    <xsl:attribute name="expr" select="concat('[...Var', @id, ', 4]')"/>
  </assign>
</xsl:template>

<!-- a multidimensional array all of whose cells are the specified value
     (not used by any test) -->
<xsl:template match="//@conf:multiDimensionalArrayExpr">
  <xsl:attribute name="expr" select="concat('[[', ., ', ', ., '], [', ., ', ', ., ']]')"/>
</xsl:template>

<!-- a <foreach> that increments the cells of the specified array (not used
     by any test): no script, an <assign> to each cell, a path with the
     index as its step -->
<xsl:template match="conf:incrementArray">
  <xsl:variable name="array" select="concat('Var', @id)"/>
  <foreach xmlns="http://www.w3.org/2005/07/scxml" item="item" index="index">
    <xsl:attribute name="array" select="$array"/>
    <assign>
      <xsl:attribute name="location" select="concat($array, '[index][0]')"/>
      <xsl:attribute name="expr" select="concat($array, '[index][0] + 1')"/>
    </assign>
    <assign>
      <xsl:attribute name="location" select="concat($array, '[index][1]')"/>
      <xsl:attribute name="expr" select="concat($array, '[index][1] + 1')"/>
    </assign>
  </foreach>
</xsl:template>

<!-- true iff each cell of the specified multidimensional array has the
     specified value (not used by any test). "N v": XPath's \W+ does not
     split "1=1", since its \w takes symbols such as = -->
<xsl:template match="//@conf:arrayVal">
  <xsl:variable name="array" select="replace(normalize-space(.), '^([0-9]+)[^0-9]+(.*)$', 'Var$1')"/>
  <xsl:variable name="value" select="replace(normalize-space(.), '^([0-9]+)[^0-9]+(.*)$', '$2')"/>
  <xsl:attribute name="cond" select="concat(
      $array, '[0][0] === ', $value, ' &amp;&amp; ', $array, '[0][1] === ', $value, ' &amp;&amp; ',
      $array, '[1][0] === ', $value, ' &amp;&amp; ', $array, '[1][1] === ', $value)"/>
</xsl:template>

<!-- The Basic HTTP Event I/O Processor (as in confEcma.xsl, the site's
     address is SITE_SPECIFIC_ADDRESS) -->

<xsl:template match="//@conf:testOnServer">
  <xsl:attribute name="target" select="concat('SITE_SPECIFIC_ADDRESS', .)"/>
</xsl:template>

<!-- nothing to set up -->
<xsl:template match="conf:setUpHTTPTest"/>

<!-- an expression evaluating to the Basic HTTP access URI -->
<xsl:template match="//@conf:basicHTTPAccessURI">
  <xsl:attribute name="expr">_ioprocessors['basichttp'].location</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:basicHTTPAccessURITarget">
  <xsl:attribute name="targetexpr">_ioprocessors['basichttp'].location</xsl:attribute>
</xsl:template>

<!-- true if POST was used to send the message (a string's search takes
     only a regular expression in staticjs) -->
<xsl:template match="//@conf:methodIsPost">
  <xsl:attribute name="cond">_event.raw.includes('POST')</xsl:attribute>
</xsl:template>

<!-- a namelist of all the ids listed -->
<xsl:template match="//@conf:multipleNamelist">
  <xsl:attribute name="namelist" select="string-join(for $id in tokenize(normalize-space(.), '\D+')[. != ''] return concat('Var', $id), ' ')"/>
</xsl:template>

<!-- true if the event is external -->
<xsl:template match="//@conf:eventIsExternal">
  <xsl:attribute name="cond">_event.type === 'external'</xsl:attribute>
</xsl:template>

<!-- "n v": _event.raw holds VarN=v (confEcma.xsl searches for the same
     text with a regular expression) -->
<xsl:template match="//@conf:eventIdParamHasValue">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\S+)\s+(\S+)$', concat('_event.raw.includes(', sjs:quote('Var$1=$2'), ')'))"/>
</xsl:template>

<!-- "p v": _event.raw holds p=v -->
<xsl:template match="//@conf:eventNamedParamHasValue">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\S+)\s+(\S+)$', concat('_event.raw.includes(', sjs:quote('$1=$2'), ')'))"/>
</xsl:template>

<!-- _event.raw holds the text -->
<xsl:template match="//@conf:messageBodyEquals">
  <xsl:attribute name="cond" select="concat('_event.raw.includes(', sjs:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- exprs that take an item out of _event.raw (not used by any automatic
     test): the value after p= at the end of the text -->
<xsl:template match="//@conf:getNamedParamVal">
  <xsl:attribute name="expr" select="concat('_event.raw.match(/', normalize-space(.), '=(\S+)$/)[1]')"/>
</xsl:template>

<!-- if input is 1, find param Var1, etc. -->
<xsl:template match="//@conf:getIDParamVal">
  <xsl:attribute name="expr" select="concat('_event.raw.match(/Var', normalize-space(.), '=(\S+)$/)[1]')"/>
</xsl:template>

<!-- the name and the value of the Nth parameter of the body after the
     first empty line (not used by any test; confEcma.xsl reads an unbound
     txt) -->
<xsl:template match="//@conf:getNthParamName">
  <xsl:attribute name="expr" select="concat('_event.raw.split(''\n\n'')[1].split(''&amp;'')[', normalize-space(.), '].split(''='')[0]')"/>
</xsl:template>

<xsl:template match="//@conf:getNthParamVal">
  <xsl:attribute name="expr" select="concat('_event.raw.split(''\n\n'')[1].split(''&amp;'')[', normalize-space(.), '].split(''='')[1]')"/>
</xsl:template>

<!-- an expr that evaluates to an SCXML message (not used by any test;
     confEcma.xsl's version is not a valid expression) -->
<xsl:template match="//@conf:scxmlEventExpr">
  <xsl:attribute name="expr" select="concat(
      '''&lt;message source=&quot;'' + _ioprocessors[''basichttp''].location + ',
      '''&quot; sourcetype=&quot;scxml&quot; name=&quot;'' + String(', ., ') + ',
      '''&quot; xmlns=&quot;http://www.w3.org/2005/07/scxml&quot; version=&quot;1.0&quot;/&gt;''')"/>
</xsl:template>

<!-- a <content> with the message body, which holds the address the far
     side answers to -->
<xsl:template match="conf:msgContent">
  <content xmlns="http://www.w3.org/2005/07/scxml" expr="'address=' + _ioprocessors['basichttp'].location"/>
</xsl:template>

<!-- true if the body is the one conf:msgContent makes (\n is the string
     literal's escape) -->
<xsl:template match="//@conf:msgIsBody">
  <xsl:attribute name="cond">_event.raw.includes('\n\naddress=')</xsl:attribute>
</xsl:template>

</xsl:stylesheet>
