-- Versions: define() adds one; running actors keep theirs; new actors start on the newest.
SET tinyactors.log = notice;

SELECT tinyactors.define('greeter', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="greeter" initial="s">
  <state id="s"><transition event="hi"><log expr="'hello (v1)'"/></transition></state>
</scxml>
$scxml$) AS version;
SELECT tinyactors.spawn('greeter', '{}', 'old') AS old \gset

SELECT tinyactors.define('greeter', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="greeter" initial="s">
  <state id="s"><transition event="hi"><log expr="'hello (v2)'"/></transition></state>
</scxml>
$scxml$) AS version;
SELECT tinyactors.spawn('greeter', '{}', 'new') AS new \gset

SELECT name, version FROM tinyactors.actors WHERE chart = 'greeter' ORDER BY id;
SELECT (tinyactors.send('old', 'hi')).version;
SELECT (tinyactors.send('new', 'hi')).version;
SELECT name, version FROM tinyactors.current_charts WHERE name = 'greeter';
