<?xml version="1.0"?>
<!--
  The W3C SCXML IRP's conf: notation, translated for the "json" datamodel
  (docs/proposals/json-datamodel.md, a sketch): paths, JSON literals, calls
  of registered functions and In() in cond, expr, location and the *expr
  attributes. Ours, not W3C's: it follows confEcma.xsl (irp/confEcma.xsl)
  template by template, in the same order, as confPython.xsl does, and
  every template in it has one here.

  What the json datamodel exposes, as these templates assume it (the
  proposal's "Values", "The session document" and "Expressions"):
  - a session's variables are the keys of one JSON object; `<data
    id="Var1">` is the key Var1, `undefined` until it is initialized;
  - an expression is a path (`Var1`, `Var1.a`, `Var1[0]`, `Var1['k']`,
    `_event.data.aParam`, `_sessionid`, `_name`, `_ioprocessors[type].location`),
    a JSON literal (strings in single or double quotes, and `undefined`), a
    call `f(expr, ...)` of a registered function, or `In('stateid')`;
    there are no operators, so every comparison and computation is a call;
  - `_event` is a map: `name`, `type`, `sendid`, `origin`, `origintype`,
    `invokeid` and `raw` are text, empty when the event has none; `data`
    is the payload, `undefined` when there is none; before the first event
    `_event` is unbound, and reading it is error.execution;
  - reading a key or a root that is not there is error.execution (a read
    never gives `undefined` for something missing);
  - a cond must be `true` or `false`, anything else is error.execution;
  - eventexpr, targetexpr, typeexpr, srcexpr, delayexpr and sendidexpr
    must give text;
  - `<script>` is always error.execution: the datamodel has no scripts,
    and `<assign>` is its only write;
  - child text of `<data>`, `<assign>` and `<content>` is the value of the
    JSON literal it spells, or else the text, whitespace normalized, as a
    string;
  - an ill-formed expression does not refuse the chart: its evaluation is
    error.execution (W3C 156, 312, 487).

  The function library. The conformance adapter registers these for every
  chart; a binding registers them in its language. Each takes the number
  of arguments shown; a wrong number of arguments, or an argument of
  another type than the one given, is the call's failure (error.execution).
  Values are the value table's; `undefined` is a value like any other.
  - eq(a, b), ne(a, b): whether a and b are (are not) the same value:
    numbers by value (1 and 1.0 are equal), text by its characters, arrays
    item by item and maps key by key (order of keys ignored), true, false,
    null and undefined each equal only to itself; values of different
    types are not equal (eq('2', 2) is false), never an error.
  - lt(a, b), le(a, b), gt(a, b), ge(a, b): a < b, a <= b, a > b, a >= b,
    for two numbers, or two texts compared by code point.
  - and(a, b), or(a, b), not(a): on booleans only; both arguments are
    evaluated (no short circuit).
  - add(a, b): the sum of two numbers.
  - concat(a, b): text a followed by text b.
  - isDefined(x): x is not `undefined`.
  - has(map, key): the map has the text key.
  - length(x): the number of items of an array, or of characters of a text.
  - append(array, v): a new array, the array's items then v.
  - text(x): x itself when it is text, an address's text, otherwise x's
    JSON text (`text(2)` is '2').
  - startsWith(text, prefix): text begins with prefix (W3C 224: an
    invokeid begins with its state's id).
  - contains(text, part): part occurs in text (the BasicHTTP tests, which
    read the raw message).
  Not registered: formParam(text, name), formParamName(text, n) and
  formParamValue(text, n) appear only in the templates of confEcma.xsl
  that no automatic test uses (getNamedParamVal, getIDParamVal,
  getNthParamName, getNthParamVal), so their evaluation is
  error.execution; a test that comes to use one registers it: the value
  of the form field `name` in the last line of text, and the name and the
  value of the field at position n (from 0) of the body after the first
  empty line, fields separated by & and name from value by =.

  Every expression is one line with no leading space. Expressions that
  must fail (conf:illegalExpr, conf:nonBoolean) fail when evaluated, as
  the datamodel never refuses a chart for an expression.
-->
<xsl:stylesheet
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:conf="http://www.w3.org/2005/scxml-conformance"
    xmlns:json="urn:tinyactors:confJSON"
    exclude-result-prefixes="xs json"
    version="2.0">

<!-- a string literal of $text, in single quotes -->
<xsl:function name="json:quote" as="xs:string">
  <xsl:param name="text" as="xs:string"/>
  <xsl:sequence select="concat('''', replace(replace($text, '\\', '\\\\'), '''', '\\'''), '''')"/>
</xsl:function>

<!-- the segment of a path that steps into a map by the key $key: .key
     when the grammar's name spells it, else ['key'] -->
<xsl:function name="json:key" as="xs:string">
  <xsl:param name="key" as="xs:string"/>
  <xsl:sequence select="
      if (matches($key, '^[A-Za-z_][A-Za-z0-9_\-]*$')) then concat('.', $key)
      else concat('[', json:quote($key), ']')"/>
</xsl:function>

<!-- "N op rest" (op one of = < > <= >=): its parts, and the function of
     the library that compares as op does -->
<xsl:variable name="json:comparison" select="'^\s*([0-9]+)([=&lt;&gt;]=?)(.*?)\s*$'"/>

<xsl:function name="json:left" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:sequence select="replace($spec, $json:comparison, '$1')"/>
</xsl:function>

<xsl:function name="json:op" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:variable name="op" select="replace($spec, $json:comparison, '$2')"/>
  <xsl:sequence select="
      if ($op = ('=', '==')) then 'eq'
      else if ($op = '&lt;') then 'lt'
      else if ($op = '&lt;=') then 'le'
      else if ($op = '&gt;') then 'gt'
      else 'ge'"/>
</xsl:function>

<xsl:function name="json:right" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:sequence select="replace($spec, $json:comparison, '$3')"/>
</xsl:function>

<!-- "N op rest" as "op(VarN, right)", where right is what $kind makes of
     rest: value (as written), quoted (a string literal) or var (VarM) -->
<xsl:function name="json:compare" as="xs:string">
  <xsl:param name="spec" as="xs:string"/>
  <xsl:param name="kind" as="xs:string"/>
  <xsl:variable name="right" select="
      if ($kind = 'quoted') then json:quote(json:right($spec))
      else if ($kind = 'var') then concat('Var', json:right($spec))
      else json:right($spec)"/>
  <xsl:sequence select="concat(json:op($spec), '(Var', json:left($spec), ', ', $right, ')')"/>
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
  <xsl:attribute name="datamodel">json</xsl:attribute>
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

<!-- names an invalid location for <assign>, etc.: the document has no key
     foo, so neither a write nor a read finds it -->
<xsl:template match="//@conf:invalidLocation">
  <xsl:attribute name="location">foo.bar.baz</xsl:attribute>
</xsl:template>

<!-- uses a system var as the location for assign, etc.: a write to it is
     error.execution -->
<xsl:template match="//@conf:systemVarLocation">
  <xsl:attribute name="location" select="normalize-space(.)"/>
</xsl:template>

<!-- expr is evaluated (the tests give integers) -->
<xsl:template match="//@conf:expr">
  <xsl:attribute name="expr" select="normalize-space(.)"/>
</xsl:template>

<!-- targetexpr is the corresponding ID -->
<xsl:template match="//@conf:targetVar">
  <xsl:attribute name="targetexpr" select="concat('Var', .)"/>
</xsl:template>

<!-- expr is quoted -->
<xsl:template match="//@conf:quoteExpr">
  <xsl:attribute name="expr" select="json:quote(.)"/>
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
     variable: reading a key that is not there is error.execution -->
<xsl:template match="//@conf:varNonexistentStruct">
  <xsl:attribute name="expr" select="concat('Var', ., '.bar')"/>
</xsl:template>

<!-- a value that cannot be assigned to a var: not an expression of the
     grammar (no operator exists), so its evaluation is error.execution
     (confEcma.xsl's `return` is a path here, a key the document lacks) -->
<xsl:template match="//@conf:illegalExpr">
  <xsl:attribute name="expr">1 +</xsl:attribute>
</xsl:template>

<!-- this should add 1 to the value of the specified variable -->
<xsl:template match="conf:incrementID">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id)"/>
    <xsl:attribute name="expr" select="concat('add(Var', @id, ', 1)')"/>
  </assign>
</xsl:template>

<!-- concatenates the value of id2 to id1 and assigns the result to id1
     (the tests and the scenarios concatenate text) -->
<xsl:template match="conf:concatVars">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id1)"/>
    <xsl:attribute name="expr" select="concat('concat(Var', @id1, ', Var', @id2, ')')"/>
  </assign>
</xsl:template>

<!-- assigns the sum of the values of two vars to the first var -->
<xsl:template match="//conf:sumVars">
  <assign xmlns="http://www.w3.org/2005/07/scxml">
    <xsl:attribute name="location" select="concat('Var', @id1)"/>
    <xsl:attribute name="expr" select="concat('add(Var', @id1, ', Var', @id2, ')')"/>
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

<!-- a content element whose value is the string 'foo': text that is no
     JSON literal is that text -->
<xsl:template match="//conf:contentFoo">
  <content xmlns="http://www.w3.org/2005/07/scxml">foo</content>
</xsl:template>

<!-- the JSON literal 123 -->
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

<!-- same value in an expr (the tests only assign it, as an initial value) -->
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
  <xsl:attribute name="expr" select="concat('_event', json:key(normalize-space(.)))"/>
</xsl:template>

<!-- returns the raw message structure as a string -->
<xsl:template match="//@conf:eventRaw">
  <xsl:attribute name="expr">_event.raw</xsl:attribute>
</xsl:template>

<!-- the value of the specified item in _event.data -->
<xsl:template match="//@conf:eventDataFieldValue">
  <xsl:attribute name="expr" select="concat('_event.data', json:key(normalize-space(.)))"/>
</xsl:template>

<!-- the value of a KVP specified by <param> from _event.data -->
<xsl:template match="//@conf:eventDataParamValue">
  <xsl:attribute name="expr" select="concat('_event.data', json:key(normalize-space(.)))"/>
</xsl:template>

<!-- the value of a KVP specified by namelist from _event.data -->
<xsl:template match="//@conf:eventDataNamelistValue">
  <xsl:attribute name="expr" select="concat('_event.data', json:key(concat('Var', normalize-space(.))))"/>
</xsl:template>

<!-- the location of the SCXML event I/O processor -->
<xsl:template match="//@conf:scxmlEventIOLocation">
  <xsl:attribute name="expr">_ioprocessors['http://www.w3.org/TR/scxml/#SCXMLEventProcessor'].location</xsl:attribute>
</xsl:template>

<!-- templates for the expr versions of the send attributes: each variable
     holds text -->

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

<!-- a delayexpr for the value passed in, in seconds: text -->
<xsl:template match="//@conf:delay">
  <xsl:attribute name="delayexpr" select="json:quote(concat(normalize-space(.), 's'))"/>
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
     no name of the grammar -->
<xsl:template match="//@conf:invalidNamelist">
  <xsl:attribute name="namelist">&#34;foo</xsl:attribute>
</xsl:template>

<!-- transition conditions: each a call that gives true or false -->

<!-- the value is evaluated -->
<xsl:template match="//@conf:idVal">
  <xsl:attribute name="cond" select="json:compare(., 'value')"/>
</xsl:template>

<!-- compare two variables -->
<xsl:template match="//@conf:varIdVal">
  <xsl:attribute name="cond" select="json:compare(., 'var')"/>
</xsl:template>

<!-- a var whose value was passed in via namelist has the specified value
     (confEcma.xsl: ===) -->
<xsl:template match="//@conf:namelistIdVal">
  <xsl:attribute name="cond" select="json:compare(., 'value')"/>
</xsl:template>

<!-- true if the two vars have the same value (confEcma.xsl: ===) -->
<xsl:template match="//@conf:VarEqVar">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^([0-9]+)\W+([0-9]+)$', 'eq(Var$1, Var$2)')"/>
</xsl:template>

<!-- true if the two vars have the same value, which is a structure: eq
     compares arrays and maps by value -->
<xsl:template match="//@conf:VarEqVarStruct">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^([0-9]+)\W+([0-9]+)$', 'eq(Var$1, Var$2)')"/>
</xsl:template>

<!-- the value is quoted -->
<xsl:template match="//@conf:idQuoteVal">
  <xsl:attribute name="cond" select="json:compare(., 'quoted')"/>
</xsl:template>

<!-- test on the value of two vars -->
<xsl:template match="//@conf:compareIDVal">
  <xsl:attribute name="cond" select="json:compare(., 'var')"/>
</xsl:template>

<!-- the specified var has the value of <conf:someInlineVal> -->
<xsl:template match="//@conf:idSomeVal">
  <xsl:attribute name="cond" select="concat('eq(Var', ., ', 123)')"/>
</xsl:template>

<!-- the event's name field has the value specified -->
<xsl:template match="//@conf:eventNameVal">
  <xsl:attribute name="cond" select="concat('eq(_event.name, ', json:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- the field VarN of _event.data compared with a value -->
<xsl:template match="//@conf:eventvarVal">
  <xsl:attribute name="cond" select="concat(json:op(.), '(_event.data', json:key(concat('Var', json:left(.))), ', ', json:right(.), ')')"/>
</xsl:template>

<!-- a variable compared with a system variable (the system variable's name
     is the right side) -->
<xsl:template match="//@conf:idSystemVarVal">
  <xsl:attribute name="cond" select="json:compare(., 'value')"/>
</xsl:template>

<!-- _event.data equals the specified value (a JSON literal) -->
<xsl:template match="//@conf:eventdataVal">
  <xsl:attribute name="cond" select="concat('eq(_event.data, ', normalize-space(.), ')')"/>
</xsl:template>

<!-- _event.data is the value of <conf:someInlineVal> -->
<xsl:template match="//@conf:eventdataSomeVal">
  <xsl:attribute name="cond">eq(_event.data, 123)</xsl:attribute>
</xsl:template>

<!-- the event has no data -->
<xsl:template match="//@conf:emptyEventData">
  <xsl:attribute name="cond">not(isDefined(_event.data))</xsl:attribute>
</xsl:template>

<!-- the _name system variable has the specified value -->
<xsl:template match="//@conf:nameVarVal">
  <xsl:attribute name="cond" select="concat('eq(_name, ', json:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- "n m": the value of VarN is a prefix of the value of VarM (confEcma.xsl
     passes Var(group 3) as the string and Var(group 1) as the prefix) -->
<xsl:template match="//@conf:varPrefix">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\w+)\W(\w+)$', 'startsWith(Var$2, Var$1)')"/>
</xsl:template>

<xsl:template match="//@conf:inState">
  <xsl:attribute name="cond" select="concat('In(', json:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- a cond that cannot be read as a boolean: a number; a cond is true or
     false, nothing else (test 344) -->
<xsl:template match="//@conf:nonBoolean">
  <xsl:attribute name="cond">0</xsl:attribute>
</xsl:template>

<!-- true if the id has a value: a variable the chart does not declare is
     error.execution, so false (test 245) -->
<xsl:template match="//@conf:isBound">
  <xsl:attribute name="cond" select="concat('isDefined(Var', ., ')')"/>
</xsl:template>

<!-- true if the var has been created but is not bound -->
<xsl:template match="//@conf:unboundVar">
  <xsl:attribute name="cond" select="concat('not(isDefined(Var', ., '))')"/>
</xsl:template>

<!-- true if the system var has a value: reading an unbound _event is
     error.execution, so false (test 319) -->
<xsl:template match="//@conf:systemVarIsBound">
  <xsl:attribute name="cond" select="concat('isDefined(', normalize-space(.), ')')"/>
</xsl:template>

<!-- true if the id has no value: undefined, or the empty text an absent
     _event field gives (test 351, a sendid; confEcma.xsl: !VarN) -->
<xsl:template match="//@conf:noValue">
  <xsl:attribute name="cond" select="concat('or(not(isDefined(Var', ., ')), eq(Var', ., ', ''''))')"/>
</xsl:template>

<!-- always true -->
<xsl:template match="//@conf:true">
  <xsl:attribute name="cond">true</xsl:attribute>
</xsl:template>

<!-- always false -->
<xsl:template match="//@conf:false">
  <xsl:attribute name="cond">false</xsl:attribute>
</xsl:template>

<!-- true if all the required fields of _event are bound -->
<xsl:template match="//@conf:eventFieldsAreBound">
  <xsl:attribute name="cond">and(has(_event, 'name'), and(has(_event, 'type'), and(has(_event, 'sendid'), and(has(_event, 'origin'), and(has(_event, 'origintype'), and(has(_event, 'invokeid'), has(_event, 'data')))))))</xsl:attribute>
</xsl:template>

<!-- true if _event.data has the specified field -->
<xsl:template match="//@conf:eventDataHasField">
  <xsl:attribute name="cond" select="concat('has(_event.data, ', json:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- true if the specified field of _event has no value: an absent field is
     empty text -->
<xsl:template match="//@conf:eventFieldHasNoValue">
  <xsl:attribute name="cond" select="concat('eq(_event', json:key(normalize-space(.)), ', '''')')"/>
</xsl:template>

<!-- true if the language of _event matches the processor's datamodel (not
     used by any test; _event has no language field, so it is
     error.execution, as in confEcma.xsl and confPython.xsl) -->
<xsl:template match="//@conf:eventLanguageMatchesDatamodel">
  <xsl:attribute name="cond">eq(_event.language, 'json')</xsl:attribute>
</xsl:template>

<!-- true if _event was delivered on the specified I/O processor -->
<xsl:template match="//@conf:originTypeEq">
  <xsl:attribute name="cond" select="concat('eq(_event.origintype, ', json:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- scripting: the datamodel has no scripts, so a <script> is
     error.execution wherever it is; the tests whose scripts must change
     the datamodel (302, 303, 304) are in skip-json.txt -->

<xsl:template match="conf:script">
 <script xmlns="http://www.w3.org/2005/07/scxml"/>
</xsl:template>

<xsl:template match="//@conf:scriptGoodSrc">
  <xsl:attribute name="src">D:\W3C\SCXMLTests\test300.json</xsl:attribute>
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
<!-- an array containing 1 2 3 in that order: content that is a JSON
     literal -->
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

<!-- an expr that is not a valid array: <foreach> of a number is
     error.execution -->
<xsl:template match="//@conf:illegalArray">
  <xsl:attribute name="expr">7</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:item">
  <xsl:attribute name="item" select="concat('Var', .)"/>
</xsl:template>

<!-- something that cannot be a variable name: a literal is no path -->
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
    <xsl:attribute name="expr" select="concat('append(Var', @id, ', 4)')"/>
  </assign>
</xsl:template>

<!-- a multidimensional array all of whose cells are the specified value
     (not used by any test; the value must be a JSON literal) -->
<xsl:template match="//@conf:multiDimensionalArrayExpr">
  <xsl:attribute name="expr" select="concat('[[', ., ', ', ., '], [', ., ', ', ., ']]')"/>
</xsl:template>

<!-- a <foreach> that increments the cells of the specified array (not used
     by any test): no script, an <assign> to each cell, the index a path
     index -->
<xsl:template match="conf:incrementArray">
  <xsl:variable name="array" select="concat('Var', @id)"/>
  <foreach xmlns="http://www.w3.org/2005/07/scxml" item="item" index="index">
    <xsl:attribute name="array" select="$array"/>
    <assign>
      <xsl:attribute name="location" select="concat($array, '[index][0]')"/>
      <xsl:attribute name="expr" select="concat('add(', $array, '[index][0], 1)')"/>
    </assign>
    <assign>
      <xsl:attribute name="location" select="concat($array, '[index][1]')"/>
      <xsl:attribute name="expr" select="concat('add(', $array, '[index][1], 1)')"/>
    </assign>
  </foreach>
</xsl:template>

<!-- true iff each cell of the specified multidimensional array has the
     specified value (not used by any test). "N v": confPython.xsl's \W+
     does not split "1=1", since XPath's \w takes symbols such as = -->
<xsl:template match="//@conf:arrayVal">
  <xsl:variable name="array" select="replace(normalize-space(.), '^([0-9]+)[^0-9]+(.*)$', 'Var$1')"/>
  <xsl:variable name="value" select="replace(normalize-space(.), '^([0-9]+)[^0-9]+(.*)$', '$2')"/>
  <xsl:attribute name="cond" select="concat(
      'and(and(eq(', $array, '[0][0], ', $value, '), eq(', $array, '[0][1], ', $value, ')), ',
      'and(eq(', $array, '[1][0], ', $value, '), eq(', $array, '[1][1], ', $value, ')))')"/>
</xsl:template>

<!-- The Basic HTTP Event I/O Processor (as in confEcma.xsl, the site's
     address is SITE_SPECIFIC_ADDRESS) -->

<xsl:template match="//@conf:testOnServer">
  <xsl:attribute name="target" select="concat('SITE_SPECIFIC_ADDRESS', .)"/>
</xsl:template>

<!-- nothing to set up: the functions below are the library's -->
<xsl:template match="conf:setUpHTTPTest"/>

<!-- an expression evaluating to the Basic HTTP access URI -->
<xsl:template match="//@conf:basicHTTPAccessURI">
  <xsl:attribute name="expr">_ioprocessors['basichttp'].location</xsl:attribute>
</xsl:template>

<xsl:template match="//@conf:basicHTTPAccessURITarget">
  <xsl:attribute name="targetexpr">_ioprocessors['basichttp'].location</xsl:attribute>
</xsl:template>

<!-- true if POST was used to send the message -->
<xsl:template match="//@conf:methodIsPost">
  <xsl:attribute name="cond">contains(_event.raw, 'POST')</xsl:attribute>
</xsl:template>

<!-- a namelist of all the ids listed -->
<xsl:template match="//@conf:multipleNamelist">
  <xsl:attribute name="namelist" select="string-join(for $id in tokenize(normalize-space(.), '\D+')[. != ''] return concat('Var', $id), ' ')"/>
</xsl:template>

<!-- true if the event is external -->
<xsl:template match="//@conf:eventIsExternal">
  <xsl:attribute name="cond">eq(_event.type, 'external')</xsl:attribute>
</xsl:template>

<!-- "n v": _event.raw holds VarN=v (confEcma.xsl searches for the same
     text with a regular expression) -->
<xsl:template match="//@conf:eventIdParamHasValue">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\S+)\s+(\S+)$', concat('contains(_event.raw, ', json:quote('Var$1=$2'), ')'))"/>
</xsl:template>

<!-- "p v": _event.raw holds p=v -->
<xsl:template match="//@conf:eventNamedParamHasValue">
  <xsl:attribute name="cond" select="replace(normalize-space(.), '^(\S+)\s+(\S+)$', concat('contains(_event.raw, ', json:quote('$1=$2'), ')'))"/>
</xsl:template>

<!-- _event.raw holds the text -->
<xsl:template match="//@conf:messageBodyEquals">
  <xsl:attribute name="cond" select="concat('contains(_event.raw, ', json:quote(normalize-space(.)), ')')"/>
</xsl:template>

<!-- exprs that take an item out of _event.raw: the value after p= on the
     last line (not used by any automatic test; formParam is not in the
     library, see the header) -->
<xsl:template match="//@conf:getNamedParamVal">
  <xsl:attribute name="expr" select="concat('formParam(_event.raw, ', json:quote(normalize-space(.)), ')')"/>
</xsl:template>

<xsl:template match="//@conf:getIDParamVal">
  <xsl:attribute name="expr" select="concat('formParam(_event.raw, ', json:quote(concat('Var', normalize-space(.))), ')')"/>
</xsl:template>

<!-- the name and the value of the Nth parameter of the body (not used by
     any test; neither function is in the library) -->
<xsl:template match="//@conf:getNthParamName">
  <xsl:attribute name="expr" select="concat('formParamName(_event.raw, ', normalize-space(.), ')')"/>
</xsl:template>

<xsl:template match="//@conf:getNthParamVal">
  <xsl:attribute name="expr" select="concat('formParamValue(_event.raw, ', normalize-space(.), ')')"/>
</xsl:template>

<!-- an expr that evaluates to an SCXML message (not used by any test;
     confEcma.xsl's version is not a valid expression) -->
<xsl:template match="//@conf:scxmlEventExpr">
  <xsl:attribute name="expr" select="concat(
      'concat(concat(concat(''&lt;message source=&quot;'', _ioprocessors[''basichttp''].location), ',
      'concat(''&quot; sourcetype=&quot;scxml&quot; name=&quot;'', text(', ., '))), ',
      '''&quot; xmlns=&quot;http://www.w3.org/2005/07/scxml&quot; version=&quot;1.0&quot;/&gt;'')')"/>
</xsl:template>

<!-- a <content> with the message body, which holds the address the far
     side answers to -->
<xsl:template match="conf:msgContent">
  <content xmlns="http://www.w3.org/2005/07/scxml" expr="concat('address=', _ioprocessors['basichttp'].location)"/>
</xsl:template>

<!-- true if the body is the one conf:msgContent makes (\n is the string
     literal's escape) -->
<xsl:template match="//@conf:msgIsBody">
  <xsl:attribute name="cond">contains(_event.raw, '\n\naddress=')</xsl:attribute>
</xsl:template>

</xsl:stylesheet>
