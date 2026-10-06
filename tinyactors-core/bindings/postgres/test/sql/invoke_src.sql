-- <invoke src> names charts of the definitions table
SET client_min_messages = warning;
SELECT tinyactors.define('worker', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="worker" initial="w">
  <datamodel><data id="job" expr="null"/><data id="v" expr="1"/></datamodel>
  <state id="w">
    <transition event="go" target="done"/>
  </state>
  <final id="done"><donedata><param name="job" expr="job"/><param name="v" expr="v"/></donedata></final>
</scxml>
$scxml$) AS version;
SELECT tinyactors.define('boss', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="boss" initial="s">
  <datamodel><data id="results" expr="[]"/></datamodel>
  <state id="s">
    <invoke id="a" src="worker"><param name="job" expr="'first'"/></invoke>
    <invoke id="b" src="tinyactors:worker"><param name="job" expr="'second'"/></invoke>
    <invoke id="c" srcexpr="'worker'"><param name="job" expr="'third'"/></invoke>
    <transition event="go"><send target="#_a" event="go"/><send target="#_b" event="go"/><send target="#_c" event="go"/></transition>
    <transition event="done.invoke.*"><script>results.push(_event.data.job + ':' + _event.data.v)</script></transition>
  </state>
</scxml>
$scxml$) AS version;
SELECT tinyactors.spawn('boss', '{}', 'boss-1') > 0 AS spawned;
SELECT cardinality(sessions) AS invoked FROM tinyactors.actors WHERE name = 'boss-1';
SELECT datamodel->'results' AS results FROM tinyactors.send('boss-1', 'go');

-- the invoked chart is fixed when the invoking chart is defined
SELECT tinyactors.define('worker', replace(scxml, 'expr="1"', 'expr="2"')) AS version
  FROM tinyactors.charts WHERE name = 'worker' AND version = 1;
SELECT tinyactors.spawn('boss', '{}', 'boss-2') > 0 AS spawned;
SELECT datamodel->'results' AS still_v1 FROM tinyactors.send('boss-2', 'go');
-- defining it again picks up the newest; worker@1 pins a version
SELECT tinyactors.define('boss', scxml) AS version FROM tinyactors.charts WHERE name = 'boss' AND version = 1;
SELECT tinyactors.define('pinned', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="pinned" initial="s">
  <datamodel><data id="v" expr="0"/></datamodel>
  <state id="s">
    <invoke id="a" src="worker@1"/>
    <transition event="go"><send target="#_a" event="go"/></transition>
    <transition event="done.invoke.a"><assign location="v" expr="_event.data.v"/></transition>
  </state>
</scxml>
$scxml$) AS version;
SELECT tinyactors.spawn('boss', '{}', 'boss-3') > 0 AS spawned;
SELECT datamodel->'results' AS now_v2 FROM tinyactors.send('boss-3', 'go');
SELECT tinyactors.spawn('pinned', '{}', 'pinned-1') > 0 AS spawned;
SELECT datamodel->'v' AS v FROM tinyactors.send('pinned-1', 'go');

-- a src that names no defined chart: the definition fails, naming the line
SELECT tinyactors.define('broken', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" initial="s">
  <state id="s"><invoke src="nobody"/></state>
</scxml>
$scxml$);
-- a srcexpr naming a chart the image does not have: error.execution in the chart
SELECT tinyactors.define('dyn', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="dyn" initial="s">
  <datamodel><data id="err" expr="null"/></datamodel>
  <state id="s">
    <invoke srcexpr="'nobody'"/>
    <transition event="error.execution"><assign location="err" expr="_event.name"/></transition>
  </state>
</scxml>
$scxml$) AS version;
SELECT tinyactors.spawn('dyn', '{}', 'dyn-1') > 0 AS spawned;
SELECT status, datamodel->'err' AS err FROM tinyactors.actors WHERE name = 'dyn-1';
