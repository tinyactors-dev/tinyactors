-- Effects: sends to registered I/O processors become outbox rows the
-- application drains (claim / ack / fail); sends between actors are inbox rows.
SET tinyactors.log = notice;

INSERT INTO tinyactors.processors VALUES ('email', 'sent by the mailer service');

SELECT tinyactors.define('signup', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="signup" initial="new">
  <datamodel><data id="email" expr="null"/><data id="audit" expr="null"/></datamodel>
  <state id="new">
    <transition event="register" target="confirming">
      <assign location="email" expr="_event.data.email"/>
      <send type="email" target="smtp" event="welcome" id="welcome"><param name="to" expr="email"/></send>
      <send type="http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor" target="https://crm.example/hooks" event="lead">
        <param name="email" expr="email"/>
      </send>
      <send targetexpr="audit" event="note"><param name="what" expr="'registered ' + email"/></send>
    </transition>
  </state>
  <state id="confirming">
    <transition event="error.communication" target="undeliverable"><log expr="'failed: ' + _event.sendid"/></transition>
    <transition event="confirm" target="active"/>
  </state>
  <state id="undeliverable"/>
  <final id="active"/>
</scxml>
$scxml$) AS version;

SELECT tinyactors.define('audit', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="audit" initial="on">
  <datamodel><data id="notes" expr="[]"/></datamodel>
  <state id="on"><transition event="note"><script>notes.push(_event.data.what)</script></transition></state>
</scxml>
$scxml$) AS version;

SELECT tinyactors.spawn('audit', '{}', 'audit') AS audit \gset
SELECT tinyactors.spawn('signup', jsonb_build_object('audit', '#_scxml_' || :audit), 'ann') AS ann \gset
SELECT configuration FROM tinyactors.send(:ann, 'register', '{"email": "ann@example.com"}');

-- the event for the audit actor was processed in the same transaction
SELECT datamodel FROM tinyactors.actors WHERE id = :audit;

-- the outbox: one row per external send, keyed for idempotency
-- (<sending session>:<step>.<n>: the same when a rolled-back event is retried)
SELECT type, target, event, data, idempotency_key ~ ('^' || actor || ':[0-9]+[.][0-9]+$') AS keyed
  FROM tinyactors.outbox ORDER BY id;

-- a worker claims rows (others skip them), performs them, acks
SELECT count(*) AS claimed FROM tinyactors.claim(1, 'worker-1');
SELECT count(*) AS claimed_by_another FROM tinyactors.claim(1, 'worker-2');
SELECT count(*) AS nothing_left FROM tinyactors.claim(10, 'worker-3');
SELECT tinyactors.ack(id) FROM tinyactors.outbox WHERE event = 'lead';
-- the welcome mail bounced: the actor hears error.communication with the sendid
SELECT tinyactors.fail(id, 'mailbox full') FROM tinyactors.outbox WHERE event = 'welcome';
SELECT configuration FROM tinyactors.actors WHERE id = :ann;
SELECT event, delivered_at IS NOT NULL AS delivered, error FROM tinyactors.outbox ORDER BY id;

-- an unregistered processor type is an error in the chart (error.execution), not in SQL
SELECT tinyactors.define('typo', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="typo" initial="s">
  <state id="s">
    <transition event="go"><send type="carrier-pigeon" event="coo"/></transition>
    <transition event="error.execution" target="caught"/>
  </state>
  <state id="caught"/>
</scxml>
$scxml$) AS version;
SELECT configuration FROM tinyactors.send(tinyactors.spawn('typo'), 'go');

-- a send to a session nobody has: error.communication for the sender
SELECT tinyactors.define('lonely', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="lonely" initial="s">
  <state id="s">
    <transition event="go"><send target="#_scxml_999999" event="hello"/></transition>
    <transition event="error.communication" target="alone"/>
  </state>
  <state id="alone"/>
</scxml>
$scxml$) AS version;
SELECT configuration FROM tinyactors.send(tinyactors.spawn('lonely'), 'go');
