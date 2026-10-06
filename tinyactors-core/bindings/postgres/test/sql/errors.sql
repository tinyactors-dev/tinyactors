-- Errors: what SQL reports, and what stays inside the chart.
SET tinyactors.log = notice;
\set VERBOSITY default

-- a chart that does not compile: the message names the line
SELECT tinyactors.define('broken', $scxml$<scxml xmlns="http://www.w3.org/2005/07/scxml" initial="nowhere">
  <state id="s"/>
</scxml>$scxml$);
SELECT tinyactors.define('xpath', $scxml$<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="xpath" initial="s"><state id="s"/></scxml>$scxml$);
SELECT count(*) AS no_versions FROM tinyactors.charts WHERE name IN ('broken', 'xpath');

-- an error in an expression is error.execution for the chart (SCXML), not for SQL
SELECT tinyactors.define('careful', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="careful" initial="s">
  <datamodel><data id="errors" expr="0"/></datamodel>
  <state id="s">
    <transition event="go"><assign location="nope" expr="1"/></transition>
    <transition event="error.execution"><assign location="errors" expr="errors + 1"/></transition>
  </state>
</scxml>
$scxml$) AS version;
SELECT datamodel FROM tinyactors.send(tinyactors.spawn('careful'), 'go');

-- a runaway eventless loop: the transaction fails, the actor is unchanged
SELECT tinyactors.define('spinner', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="spinner" initial="idle">
  <datamodel><data id="n" expr="0"/></datamodel>
  <state id="idle"><transition event="spin" target="a"/></state>
  <state id="a"><transition target="b"><assign location="n" expr="n + 1"/></transition></state>
  <state id="b"><transition target="a"/></state>
</scxml>
$scxml$) AS version;
SELECT tinyactors.spawn('spinner', '{}', 'spinner') AS spinner \gset
SET tinyactors.fuel = 20000;
SELECT tinyactors.send('spinner', 'spin');
RESET tinyactors.fuel;
SELECT configuration, datamodel, events FROM tinyactors.actors WHERE name = 'spinner';
