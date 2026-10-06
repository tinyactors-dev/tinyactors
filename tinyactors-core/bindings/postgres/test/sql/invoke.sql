-- Actors with running <invoke>d SCXML sessions: stored with their session
-- tree, carried on across transactions, invoked sessions addressable.
SET client_min_messages = warning;
SET tinyactors.now = '2030-01-01 00:00:00+00';
SELECT tinyactors.define('parent', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="parent" initial="s">
  <datamodel>
    <data id="pongs" expr="[]"/><data id="tocks" expr="0"/><data id="failed" expr="null"/>
    <data id="result" expr="null"/>
  </datamodel>
  <state id="s">
    <invoke id="kid"><content>
      <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="kid" initial="k">
        <datamodel><data id="c" expr="0"/></datamodel>
        <state id="k">
          <onentry><send event="tick" delay="10s"/></onentry>
          <transition event="ping"><assign location="c" expr="c + 1"/>
            <send target="#_parent" event="pong"><param name="v" expr="c"/><param name="me" expr="_sessionid"/></send>
          </transition>
          <transition event="tick"><send target="#_parent" event="tock"/></transition>
          <transition event="work"><send type="outbox" target="svc" event="work" id="w1"/></transition>
          <transition event="error.communication">
            <send target="#_parent" event="child.failed"><param name="sendid" expr="_event.sendid"/></send>
          </transition>
          <transition event="finish" target="end"/>
        </state>
        <final id="end"><donedata><param name="c" expr="c"/></donedata></final>
      </scxml>
    </content></invoke>
    <transition event="ping"><send target="#_kid" event="ping"/></transition>
    <transition event="work"><send target="#_kid" event="work"/></transition>
    <transition event="finish"><send target="#_kid" event="finish"/></transition>
    <transition event="pong"><script>pongs.push(_event.data.v)</script></transition>
    <transition event="tock"><assign location="tocks" expr="tocks + 1"/></transition>
    <transition event="child.failed"><assign location="failed" expr="_event.data.sendid"/></transition>
    <transition event="done.invoke.kid" target="t"><assign location="result" expr="_event.data.c"/></transition>
  </state>
  <state id="t"/>
</scxml>
$scxml$) AS version;
SELECT tinyactors.define('caller', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="caller" initial="s">
  <state id="s">
    <transition event="call"><send targetexpr="'#_scxml_' + _event.data.to" event="ping"/></transition>
  </state>
</scxml>
$scxml$) AS version;

-- stored with the session it invoked (an id of their own range)
SELECT tinyactors.spawn('parent', '{}', 'p') > 0 AS spawned;
SELECT status, configuration, cardinality(sessions) AS sessions,
       sessions[1] >= 4611686018427387904 AS own_range
  FROM tinyactors.actors WHERE name = 'p';

-- the child carries on across transactions
SELECT datamodel->'pongs' AS pongs FROM tinyactors.send('p', 'ping');
SELECT datamodel->'pongs' AS pongs FROM tinyactors.send('p', 'ping');

-- the child's session id is addressable: from SQL (send() returns its actor) ...
SELECT x.name, x.datamodel->'pongs' AS pongs
  FROM tinyactors.actors p, tinyactors.send(p.sessions[1], 'ping') x WHERE p.name = 'p';
SELECT tinyactors.deliver(p.sessions[1], 'ping') AS delivered
  FROM tinyactors.actors p WHERE p.name = 'p';
-- ... and from another actor, with #_scxml_<id>
SELECT tinyactors.spawn('caller', '{}', 'q') > 0 AS spawned;
SELECT 1 FROM tinyactors.actors p, tinyactors.send('q', 'call', jsonb_build_object('to', p.sessions[1]::text))
 WHERE p.name = 'p';
SELECT datamodel->'pongs' AS pongs FROM tinyactors.actors WHERE name = 'p';

-- the child's timer is the actor's (earliest of the tree): it fires
SELECT next_due_at FROM tinyactors.actors WHERE name = 'p';
SET tinyactors.now = '2030-01-01 00:00:11+00';
SELECT tinyactors.run() AS processed;
SELECT datamodel->'tocks' AS tocks FROM tinyactors.actors WHERE name = 'p';

-- the child's effects: the outbox row names it; fail() reaches it
SELECT 1 FROM tinyactors.send('p', 'work');
SELECT o.session = p.sessions[1] AS from_child, o.idempotency_key ~ ('^' || o.session || ':[0-9]+[.][0-9]+$') AS keyed
  FROM tinyactors.outbox o, tinyactors.actors p WHERE p.name = 'p' AND o.target = 'svc';
SELECT tinyactors.fail(id, 'down') FROM tinyactors.outbox WHERE target = 'svc';
SELECT datamodel->'failed' AS failed FROM tinyactors.actors WHERE name = 'p';

-- done.invoke with donedata; the tree is gone
SELECT configuration, datamodel->'result' AS result, sessions FROM tinyactors.send('p', 'finish');
-- a send to the finished child's id: nobody has it
SELECT tinyactors.deliver(4611686018427387904, 'ping') AS delivered;
RESET tinyactors.now;
