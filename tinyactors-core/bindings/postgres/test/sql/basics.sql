-- Charts, actors, events: the everyday API.
SET tinyactors.log = notice;
SET client_min_messages = notice;

SELECT tinyactors.define('counter', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="counter" initial="idle">
  <datamodel>
    <data id="count" expr="0"/>
    <data id="history" expr="[]"/>
  </datamodel>
  <state id="idle">
    <transition event="inc">
      <assign location="count" expr="count + (_event.data &amp;&amp; _event.data.by || 1)"/>
      <script>history.push(count)</script>
      <log label="count" expr="count"/>
    </transition>
    <transition event="pause" target="paused"/>
    <transition event="finish" target="finished"/>
  </state>
  <state id="paused">
    <transition event="resume" target="idle"/>
  </state>
  <final id="finished">
    <donedata><param name="total" expr="count"/><param name="steps" expr="history.length"/></donedata>
  </final>
</scxml>
$scxml$) AS version;

-- spawn: input overrides <data>; a name is an optional handle
SELECT tinyactors.spawn('counter', '{"count": 100}', 'main') AS main \gset
SELECT tinyactors.spawn('counter') AS other \gset
SELECT name, status, configuration, datamodel FROM tinyactors.actors ORDER BY id;

-- send processes the event in this transaction and returns the actor
SELECT configuration, datamodel, events FROM tinyactors.send(:main, 'inc', '{"by": 5}');
SELECT configuration, datamodel, events FROM tinyactors.send('main', 'inc');
SELECT (tinyactors.send('main', 'pause')).configuration;
SELECT (tinyactors.send('main', 'inc')).datamodel;   -- no transition for inc while paused
SELECT (tinyactors.send('main', 'resume')).configuration;

-- actors are rows: query them like any table
SELECT name, datamodel->>'count' AS count FROM tinyactors.actors WHERE datamodel->'count' IS NOT NULL ORDER BY id;

-- a rolled-back event never happened
BEGIN;
SELECT (tinyactors.send('main', 'inc', '{"by": 1000}')).datamodel->'count' AS inside;
ROLLBACK;
SELECT datamodel->'count' AS after_rollback FROM tinyactors.actors WHERE name = 'main';

-- done: <donedata> becomes the output; the state is gone
SELECT status, configuration, output, state IS NULL AS state_gone FROM tinyactors.send('main', 'finish');
-- events for a finished actor are dropped
SELECT status, events FROM tinyactors.send('main', 'inc');

-- deliver(): send() without the row; no actor is not an error; no inbox row
BEGIN;
SELECT tinyactors.deliver(:other, 'inc') AS delivered, tinyactors.deliver(424242, 'inc') AS nobody;
SELECT n_tup_ins, n_tup_del FROM pg_stat_xact_user_tables WHERE relname = 'inbox';
COMMIT;

SELECT tinyactors.send(424242, 'inc');
SELECT tinyactors.send('nobody', 'inc');
SELECT tinyactors.spawn('no-such-chart');

-- stop: no done event, nothing pending
SELECT tinyactors.stop(:other);
SELECT status, state IS NULL AS state_gone FROM tinyactors.actors WHERE id = :other;

-- system id: derived from the database name, or tinyactors.system_id; it is in
-- every origin an actor sees (docs/protocol.md, Addresses)
SELECT tinyactors.define('who', $x$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="who" initial="s">
  <datamodel><data id="from" expr="null"/><data id="me" expr="null"/></datamodel>
  <state id="s">
    <transition event="ask"><send targetexpr="'tinyactors://' + _event.data.sys + '/#_scxml_' + _event.data.id" event="tell"/></transition>
    <transition event="tell"><assign location="from" expr="_event.origin"/><assign location="me" expr="_ioprocessors.scxml.location"/></transition>
  </state>
</scxml>$x$) > 0 AS defined;
SELECT tinyactors.spawn('who', '{}', 'who-a') > 0 AS a, tinyactors.spawn('who', '{}', 'who-b') > 0 AS b;
SELECT 1 FROM tinyactors.send('who-a', 'ask', jsonb_build_object('sys', 'contrib-regression', 'id', (SELECT id FROM tinyactors.actors WHERE name = 'who-b')));
SELECT regexp_replace(datamodel->>'from', '[0-9]+$', 'N') AS origin, regexp_replace(datamodel->>'me', '[0-9]+$', 'N') AS location
  FROM tinyactors.actors WHERE name = 'who-b';
SET tinyactors.system_id = 'Not Valid';
SELECT 1 FROM tinyactors.send('who-a', 'ask', '{"sys": "x", "id": 1}');
SET tinyactors.system_id = 'orders.eu';
SELECT 1 FROM tinyactors.send('who-a', 'ask', jsonb_build_object('sys', 'orders.eu', 'id', (SELECT id FROM tinyactors.actors WHERE name = 'who-b')));
SELECT regexp_replace(datamodel->>'from', '[0-9]+$', 'N') AS origin FROM tinyactors.actors WHERE name = 'who-b';
RESET tinyactors.system_id;
