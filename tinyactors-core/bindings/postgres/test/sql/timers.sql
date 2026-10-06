-- Delayed sends are durable: due times are absolute; run() fires what is due.
SET tinyactors.log = notice;
SET tinyactors.now = '2030-01-01 00:00:00+00';

SELECT tinyactors.define('reminder', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="reminder" initial="waiting">
  <datamodel><data id="rings" expr="0"/></datamodel>
  <state id="waiting">
    <onentry><send event="ring" delay="5s" id="alarm"/></onentry>
    <transition event="snooze"><cancel sendid="alarm"/><send event="ring" delay="10s" id="alarm"/></transition>
    <transition event="ring" target="ringing"/>
  </state>
  <state id="ringing">
    <onentry><assign location="rings" expr="rings + 1"/><log expr="'ring ' + rings"/></onentry>
    <transition event="dismiss" target="waiting"/>
  </state>
</scxml>
$scxml$) AS version;

SELECT tinyactors.spawn('reminder', '{}', 'r') AS r \gset
SELECT configuration, next_due_at FROM tinyactors.actors WHERE id = :r;

SET tinyactors.now = '2030-01-01 00:00:03+00';
SELECT tinyactors.run() AS processed;                     -- nothing due yet
SELECT (tinyactors.send(:r, 'snooze')).next_due_at;        -- cancelled and re-armed: 00:13

SET tinyactors.now = '2030-01-01 00:00:06+00';
SELECT tinyactors.run() AS processed;                     -- the cancelled one does not fire

SET tinyactors.now = '2030-01-01 00:00:14+00';
SELECT tinyactors.run() AS processed;
SELECT configuration, datamodel, next_due_at FROM tinyactors.actors WHERE id = :r;

-- the next cycle arms a new timer relative to the clock at that moment
SELECT (tinyactors.send(:r, 'dismiss')).next_due_at;
SET tinyactors.now = '2030-01-01 00:00:20+00';
SELECT tinyactors.run() AS processed;
SELECT datamodel->'rings' AS rings FROM tinyactors.actors WHERE id = :r;
SELECT tinyactors.now() = '2030-01-01 00:00:20+00'::timestamptz AS clock_is_settable;
