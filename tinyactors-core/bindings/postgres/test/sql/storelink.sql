-- Store links (docs/protocol.md, "Store links"): envelopes from other systems
-- in (deliver(envelope), the dedup window), envelopes to other systems out
-- (outbox rows per peer, hello, claim_for, deliver_error).
SET tinyactors.log = notice;
SET tinyactors.system_id = 'db';

SELECT tinyactors.system_id();
SELECT * FROM tinyactors.address('tinyactors://web/#_scxml_7');
SELECT * FROM tinyactors.address('tinyactors://db/@order%2D1');
SELECT * FROM tinyactors.address('#_scxml_7');

-- replies to whoever asked; counts; forwards to other systems
SELECT tinyactors.define('ledger', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="ledger" initial="on">
  <datamodel><data id="n" expr="0"/><data id="seen" expr="[]"/><data id="last" expr="null"/></datamodel>
  <state id="on">
    <transition event="add">
      <assign location="n" expr="n + _event.data.amount"/>
      <script>seen.push({origin: _event.origin, sendid: _event.sendid || null, bytes: _event.data.blob ? _event.data.blob.length : null})</script>
      <send event="total" targetexpr="_event.origin"><param name="n" expr="n"/></send>
    </transition>
    <transition event="tell">
      <send event="note" targetexpr="_event.data.to" idlocation="last"><param name="raw" expr="new Uint8Array([1, 2, 255])"/></send>
    </transition>
    <transition event="error.communication"><log expr="'lost ' + _event.sendid"/></transition>
  </state>
</scxml>
$scxml$) AS version;
SELECT tinyactors.spawn('ledger', '{}', 'ledger') AS ledger \gset

-- in: to a name, with an origin, a sendid and a neutral value ($bytes)
SELECT tinyactors.deliver('{"target": "tinyactors://db/@ledger", "name": "add",
  "data": {"amount": 5, "blob": {"$bytes": "AAEC"}},
  "origin": "tinyactors://web/#_scxml_7", "sendid": "s1", "msgid": "tinyactors://web/#_scxml_7/1"}');
-- the same msgid again: dropped (the dedup window)
SELECT tinyactors.deliver('{"target": "tinyactors://db/@ledger", "name": "add", "data": {"amount": 5},
  "origin": "tinyactors://web/#_scxml_7", "msgid": "tinyactors://web/#_scxml_7/1"}');
-- a dedup key replaces the msgid: two msgids, one delivery
SELECT tinyactors.deliver(jsonb_build_object('target', 'tinyactors://db/#_scxml_' || :ledger, 'name', 'add',
  'data', '{"amount": 1}'::jsonb, 'origin', 'tinyactors://web/#_scxml_7', 'msgid', 'tinyactors://web/#_scxml_7/2', 'dedup', 'pay-42'));
SELECT tinyactors.deliver(jsonb_build_object('target', 'tinyactors://db/#_scxml_' || :ledger, 'name', 'add',
  'data', '{"amount": 1}'::jsonb, 'origin', 'tinyactors://web/#_scxml_7', 'msgid', 'tinyactors://web/#_scxml_7/3', 'dedup', 'pay-42'));
SELECT datamodel FROM tinyactors.actors WHERE id = :ledger;
SELECT origin, key FROM tinyactors.seen ORDER BY key;

-- refusals: reasons, nothing delivered
SELECT tinyactors.deliver('{"target": "tinyactors://db/@nobody", "name": "add", "origin": "tinyactors://web/#_scxml_7"}');
SELECT tinyactors.deliver('{"target": "tinyactors://db/#_scxml_999999", "name": "add"}');
SELECT tinyactors.deliver('{"target": "tinyactors://elsewhere/@x", "name": "add"}');
SELECT tinyactors.deliver('{"target": "not an address", "name": "add"}');
SELECT tinyactors.deliver('{"target": "tinyactors://db/@ledger"}');
SELECT tinyactors.deliver('[1, 2]');

-- out: the replies went to the outbox as envelopes for the peer "web"
SELECT type, system, peer, envelope->>'target' AS target, envelope->>'name' AS name, envelope->'data' AS data,
       envelope->>'origin' = 'tinyactors://db/#_scxml_' || actor AS origin_ok,
       envelope->>'msgid' = (envelope->>'origin') || '/' || split_part(idempotency_key, ':', 2) AS msgid_ok, envelope->'hops' AS hops
  FROM tinyactors.outbox ORDER BY id;
-- the application's claim() does not take them; the store link's claim_for does, in order
SELECT count(*) AS for_the_application FROM tinyactors.claim(10, 'app');
SELECT envelope->'data' AS data, claimed_by FROM tinyactors.claim_for('web', 10, 'link-1');
SELECT count(*) AS claimed_again FROM tinyactors.claim_for('web', 10, 'link-2');
SELECT count(*) AS acked FROM (SELECT tinyactors.ack(id) FROM tinyactors.outbox WHERE claimed_by = 'link-1') x;
SELECT count(*) AS delivered FROM tinyactors.outbox WHERE peer = 'web' AND delivered_at IS NOT NULL;

-- a send to a browser behind the "web" system: waits under the browser's id
-- until a hello says who reaches it, then it is web's
SELECT (tinyactors.send(:ledger, 'tell', '{"to": "tinyactors://tab-1/#_scxml_3"}')).id = :ledger AS sent;
SELECT system, peer, envelope->'data' AS data FROM tinyactors.outbox WHERE system = 'tab-1';
SELECT tinyactors.hello('web', ARRAY['tab-1', 'tab-2']);
SELECT system, peer FROM tinyactors.outbox WHERE system = 'tab-1';
SELECT (tinyactors.send(:ledger, 'tell', '{"to": "tinyactors://tab-2/#_scxml_9"}')).id = :ledger AS sent;
SELECT system, peer FROM tinyactors.outbox WHERE system = 'tab-2';
SELECT id AS tab1 FROM tinyactors.outbox WHERE system = 'tab-1' \gset

-- the far side refused it: an err back makes error.communication for the sender
SELECT tinyactors.deliver_error(jsonb_build_object('origin', envelope->>'origin', 'msgid', envelope->>'msgid',
       'sendid', envelope->>'sendid', 'reason', 'unknown-target'))
  FROM tinyactors.outbox WHERE id = :tab1;
SELECT error FROM tinyactors.outbox WHERE id = :tab1;
SELECT tinyactors.deliver_error('{"origin": "tinyactors://web/#_scxml_7", "msgid": "x", "reason": "denied"}') AS not_ours;

-- the dedup window is pruned after tinyactors.dedup_ttl
SET tinyactors.dedup_ttl = '0 seconds';
SELECT tinyactors.prune_seen() >= 2 AS pruned;
RESET tinyactors.dedup_ttl;

-- without tinyactors.system_id, the database name is the system id (lowercased,
-- other characters as "-"); an invalid setting is refused where it is used
RESET tinyactors.system_id;
SELECT tinyactors.system_id() AS from_database_name, current_database();
SET tinyactors.system_id = 'Not_Valid';
SELECT tinyactors.system_id();
RESET tinyactors.system_id;
