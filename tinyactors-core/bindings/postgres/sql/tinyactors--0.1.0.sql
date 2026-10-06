-- tinyactors 0.1.0: durable SCXML actors in PostgreSQL.
--
-- Charts and actors are rows. An event is processed inside the transaction
-- that sends it: the actor's new state, the events it sends to other actors,
-- its timers and its outbound effects commit or roll back together.
-- See README.md (reference) and docs/postgres.md (design).

\echo Use "CREATE EXTENSION tinyactors" to load this file. \quit

-- ---------------------------------------------------------------- tables

-- Chart definitions: every define() adds a version. Actors keep the version
-- they started on.
CREATE TABLE tinyactors.charts (
    name        text        NOT NULL,
    version     integer     NOT NULL,
    scxml       text        NOT NULL,
    datamodel   text        NOT NULL,
    image       bytea       NOT NULL,   -- the compiled, immutable chart image
    created_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (name, version)
);
COMMENT ON TABLE tinyactors.charts IS 'SCXML charts, one row per version (define() adds one)';

CREATE SEQUENCE tinyactors.actor_id_seq;

-- Session ids of <invoke>d sessions: a range of their own (from 2^62, never an
-- actor id), drawn in blocks of 2^20 by each processing that may invoke.
CREATE SEQUENCE tinyactors.session_seq
    START WITH 4611686018427387904 MINVALUE 4611686018427387904 INCREMENT BY 1048576;

-- Actors: the durable state of every session. configuration and datamodel are
-- written on every change, so actors can be queried like any table.
CREATE TABLE tinyactors.actors (
    id            bigint      PRIMARY KEY,              -- the SCXML session id
    chart         text        NOT NULL,
    version       integer     NOT NULL,
    name          text        UNIQUE,                   -- an optional handle
    status        text        NOT NULL DEFAULT 'active'
                  CHECK (status IN ('active', 'done', 'failed', 'stopped')),
    configuration text[]      NOT NULL DEFAULT '{}',    -- active atomic states
    datamodel     jsonb,                                -- the <datamodel> variables
    output        jsonb,                                -- <donedata>, once done
    next_due_at   timestamptz,                          -- earliest pending <send delay>
    events        bigint      NOT NULL DEFAULT 0,       -- events processed
    steps         bigint      NOT NULL DEFAULT 0,       -- processings (effect ids: <session>:<step>.<n>)
    failures      integer     NOT NULL DEFAULT 0,       -- run(): failed processings in a row
    last_error    text,                                 -- the last failure (cleared by a good one)
    retry_at      timestamptz,                          -- run() leaves it alone until then (backoff)
    state         bytea,                                -- the actor and the sessions it invoked (NULL once it ended)
    sessions      bigint[],                             -- ids of its running invoked sessions (NULL: none)
    created_at    timestamptz NOT NULL DEFAULT now(),
    updated_at    timestamptz NOT NULL DEFAULT now(),
    FOREIGN KEY (chart, version) REFERENCES tinyactors.charts
);
CREATE INDEX actors_due ON tinyactors.actors (next_due_at)
    WHERE status = 'active' AND next_due_at IS NOT NULL;
COMMENT ON TABLE tinyactors.actors IS 'SCXML sessions; change them through send(), not by UPDATE';
CREATE INDEX actors_sessions ON tinyactors.actors USING gin (sessions) WHERE sessions IS NOT NULL;
-- Room on every page, so an actor's update can stay on its page (a HOT
-- update: no new index entries) when it does not move its timer; and vacuum
-- driven by what changed, not by the size of the table — with the default
-- (20% of the rows) a table of 5 million actors would wait for a million dead
-- versions, while the few hot actors' versions pile up in front of every
-- lookup of them.
ALTER TABLE tinyactors.actors SET (
    fillfactor = 80,
    autovacuum_vacuum_scale_factor = 0.01,
    autovacuum_vacuum_insert_scale_factor = 0.01,
    autovacuum_analyze_scale_factor = 0.01
);

-- Inbox: events not processed yet (normally only within the sending
-- transaction; rows survive when the receiver was busy, for run()).
CREATE TABLE tinyactors.inbox (
    id         bigserial   PRIMARY KEY,
    actor      bigint      NOT NULL REFERENCES tinyactors.actors ON DELETE CASCADE,
    event      text        NOT NULL,
    data       jsonb,
    origin     text,
    origintype text,
    sendid     text,
    kind       text        NOT NULL DEFAULT 'event'
               CHECK (kind IN ('event', 'error.communication')),
    session    bigint,     -- for one of the actor's invoked sessions (NULL: for the actor)
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX inbox_actor ON tinyactors.inbox (actor, id);

-- I/O processor types whose sends become outbox rows (the application
-- performs them). Add your own: INSERT INTO tinyactors.processors VALUES ('email');
CREATE TABLE tinyactors.processors (
    type        text PRIMARY KEY,
    description text
);
INSERT INTO tinyactors.processors VALUES
    ('http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor', 'BasicHTTP: the application makes the HTTP request'),
    ('outbox', 'anything the application knows how to do');

-- Outbox: effects for the application, at least once. Claim with claim(),
-- confirm with ack(), report failures with fail().
CREATE TABLE tinyactors.outbox (
    id              bigserial   PRIMARY KEY,
    actor           bigint      NOT NULL,
    type            text        NOT NULL,   -- the I/O processor
    target          text,
    event           text        NOT NULL,
    data            jsonb,
    sendid          text        NOT NULL,
    session         bigint,                -- sent by one of the actor's invoked sessions (NULL: the actor)
    idempotency_key text        NOT NULL UNIQUE,   -- <sending session>:<sendid>
    created_at      timestamptz NOT NULL DEFAULT now(),
    claimed_at      timestamptz,
    claimed_by      text,
    attempts        integer     NOT NULL DEFAULT 0,
    delivered_at    timestamptz,
    error           text,
    -- type 'scxml', an SCXML send to another system (a store link delivers it):
    system          text,                  -- the target's system id
    peer            text,                  -- the next hop: that system, or a peer reaching it (hello)
    envelope        jsonb                  -- the complete envelope (docs/protocol.md, "Envelope")
);
CREATE INDEX outbox_pending ON tinyactors.outbox (id) WHERE delivered_at IS NULL AND error IS NULL AND peer IS NULL;
-- pruning (run(): tinyactors.outbox_retention)
CREATE INDEX outbox_delivered ON tinyactors.outbox (delivered_at) WHERE delivered_at IS NOT NULL;
CREATE INDEX outbox_failed ON tinyactors.outbox (created_at) WHERE error IS NOT NULL;
-- hello(): pending envelopes by target system
CREATE INDEX outbox_system ON tinyactors.outbox (system)
    WHERE delivered_at IS NULL AND error IS NULL AND peer IS NOT NULL;
CREATE INDEX outbox_peer ON tinyactors.outbox (peer, id)
    WHERE delivered_at IS NULL AND error IS NULL AND peer IS NOT NULL;
CREATE INDEX outbox_msgid ON tinyactors.outbox ((envelope->>'msgid'));

-- NOTIFY tinyactors_outbox: the I/O processor type, or for envelopes the peer
-- (a store link listens and claims only what is for it). Skipped while the
-- server's notification queue is more than half full (a stalled listener
-- must not make effects fail: workers poll too) and with tinyactors.notify = off.
CREATE FUNCTION tinyactors.notify_outbox() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF current_setting('tinyactors.notify', true) IS DISTINCT FROM 'off'
       AND pg_notification_queue_usage() < 0.5 THEN
        PERFORM pg_notify('tinyactors_outbox', coalesce(NEW.peer, NEW.type));
    END IF;
    RETURN NULL;
END $$;
CREATE TRIGGER outbox_notify AFTER INSERT ON tinyactors.outbox
    FOR EACH ROW EXECUTE FUNCTION tinyactors.notify_outbox();

-- Store links (docs/protocol.md, "Store links"): other systems connected
-- through SQL. A peer says hello with its id and the systems it reaches;
-- envelopes for any of them are claimed by it (claim_for).
CREATE TABLE tinyactors.peers (
    id       text        PRIMARY KEY,
    reach    text[]      NOT NULL DEFAULT '{}',
    hello_at timestamptz NOT NULL DEFAULT now()
);
COMMENT ON TABLE tinyactors.peers IS 'Systems connected through store links (tinyactors.hello)';

-- Which roles may act as which peer (hello, claim_for, ack/fail/deliver_error of
-- its envelopes, deliver(envelope, peer)): tinyactors.grant_peer(peer, role).
-- Checked against the session's (login) role; the extension's owner and
-- superusers may act as any peer.
CREATE TABLE tinyactors.peer_roles (
    peer text NOT NULL,
    role name NOT NULL,
    PRIMARY KEY (peer, role)
);
COMMENT ON TABLE tinyactors.peer_roles IS 'Roles allowed to act as a store-link peer (tinyactors.grant_peer)';

-- The dedup window: message ids (or dedup keys) of envelopes delivered from
-- other systems, kept for tinyactors.dedup_ttl. A repeat is dropped.
CREATE TABLE tinyactors.seen (
    origin text        NOT NULL,
    key    text        NOT NULL,
    at     timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (origin, key)
);
CREATE INDEX seen_at ON tinyactors.seen (at);
COMMENT ON TABLE tinyactors.seen IS 'Dedup window of tinyactors.deliver(envelope)';

-- ---------------------------------------------------------- C functions

CREATE FUNCTION tinyactors.compile(scxml text, name text DEFAULT 'chart')
    RETURNS bytea LANGUAGE c STRICT AS 'MODULE_PATHNAME', 'ta_postgres_compile';
COMMENT ON FUNCTION tinyactors.compile IS 'Compile an SCXML document to a chart image (errors name the line)';

CREATE FUNCTION tinyactors.chart_datamodel(image bytea)
    RETURNS text LANGUAGE c STRICT IMMUTABLE AS 'MODULE_PATHNAME', 'ta_postgres_chart_datamodel';

CREATE FUNCTION tinyactors.spawn(chart text, input jsonb DEFAULT '{}', name text DEFAULT NULL)
    RETURNS bigint LANGUAGE c AS 'MODULE_PATHNAME', 'ta_postgres_spawn';
COMMENT ON FUNCTION tinyactors.spawn IS 'Start an actor of the current version of a chart; input overrides <data> values';

CREATE FUNCTION tinyactors.process(actor bigint)
    RETURNS integer LANGUAGE c STRICT AS 'MODULE_PATHNAME', 'ta_postgres_process';
COMMENT ON FUNCTION tinyactors.process IS 'Process the actor''s inbox and due timers (waiting for its lock), then actors it sent to (if free); returns the number of actors processed';

CREATE FUNCTION tinyactors.deliver(actor bigint, event text, data jsonb DEFAULT NULL)
    RETURNS boolean LANGUAGE c AS 'MODULE_PATHNAME', 'ta_postgres_deliver';
COMMENT ON FUNCTION tinyactors.deliver IS 'Process an event now (waiting for the actor''s lock; events it deferred earlier first), then actors it sent to (if free); the id may be an actor''s or one of its invoked sessions; false if there is no such session';

CREATE FUNCTION tinyactors.run(max_actors integer DEFAULT 1000)
    RETURNS integer LANGUAGE c STRICT AS 'MODULE_PATHNAME', 'ta_postgres_run';
COMMENT ON FUNCTION tinyactors.run IS 'Process actors with due timers or queued events that nobody is processing (SKIP LOCKED); call it periodically';

CREATE FUNCTION tinyactors.now() RETURNS timestamptz LANGUAGE c VOLATILE AS 'MODULE_PATHNAME', 'ta_postgres_now';

CREATE FUNCTION tinyactors.system_id() RETURNS text LANGUAGE c STABLE AS 'MODULE_PATHNAME', 'ta_postgres_system_id';
COMMENT ON FUNCTION tinyactors.system_id IS 'This database''s system id in actor addresses: the tinyactors.system_id setting, else derived from the database name';

CREATE FUNCTION tinyactors.address(address text, OUT system text, OUT kind text, OUT session bigint, OUT name text)
    RETURNS record LANGUAGE c STRICT IMMUTABLE AS 'MODULE_PATHNAME', 'ta_postgres_address';
CREATE FUNCTION tinyactors.data_ok(data jsonb) RETURNS boolean
    LANGUAGE c STRICT IMMUTABLE AS 'MODULE_PATHNAME', 'ta_postgres_data_ok';
COMMENT ON FUNCTION tinyactors.data_ok IS 'Event data an actor accepts: nested at most 100 levels';
COMMENT ON FUNCTION tinyactors.address IS 'Parse tinyactors://<system>/#_scxml_<id> or .../@<name> (all NULL if it is not an address)';
COMMENT ON FUNCTION tinyactors.now IS 'The actors'' clock: the tinyactors.now setting if set (tests), else clock_timestamp()';

-- ------------------------------------------------------------ SQL API

-- Define (a new version of) a chart; returns the version.
CREATE FUNCTION tinyactors.define(name text, scxml text) RETURNS integer LANGUAGE plpgsql AS $$
DECLARE
    compiled       bytea := tinyactors.compile(scxml, name);
    datamodel_name text  := tinyactors.chart_datamodel(compiled);
    v              integer;
BEGIN
    IF datamodel_name NOT IN ('ecmascript', 'null', '') THEN
        RAISE EXCEPTION 'tinyactors: the "%" datamodel is not supported (use ecmascript or null)', datamodel_name;
    END IF;
    PERFORM pg_advisory_xact_lock(hashtext('tinyactors.define:' || name));
    SELECT coalesce(max(c.version), 0) + 1 INTO v FROM tinyactors.charts c WHERE c.name = define.name;
    INSERT INTO tinyactors.charts (name, version, scxml, datamodel, image)
        VALUES (define.name, v, scxml, coalesce(nullif(datamodel_name, ''), 'null'), compiled);
    RETURN v;
END $$;

-- Send an event and process it now, in this transaction; returns the actor
-- (for an invoked session's id: the actor whose session tree holds it).
CREATE FUNCTION tinyactors.send(actor bigint, event text, data jsonb DEFAULT NULL)
    RETURNS tinyactors.actors LANGUAGE plpgsql AS $$
DECLARE
    a tinyactors.actors;
BEGIN
    IF NOT tinyactors.deliver(send.actor, send.event, send.data) THEN
        RAISE EXCEPTION 'tinyactors: no actor %', actor USING ERRCODE = 'no_data_found';
    END IF;
    SELECT * INTO a FROM tinyactors.actors x WHERE x.id = send.actor;
    IF NOT FOUND THEN
        SELECT * INTO a FROM tinyactors.actors x WHERE x.sessions @> ARRAY[send.actor];
    END IF;
    RETURN a;
END $$;

CREATE FUNCTION tinyactors.send(name text, event text, data jsonb DEFAULT NULL)
    RETURNS tinyactors.actors LANGUAGE plpgsql AS $$
DECLARE
    id bigint;
BEGIN
    SELECT x.id INTO id FROM tinyactors.actors x WHERE x.name = send.name;
    IF id IS NULL THEN
        RAISE EXCEPTION 'tinyactors: no actor named %', name USING ERRCODE = 'no_data_found';
    END IF;
    RETURN tinyactors.send(id, event, data);
END $$;

-- ------------------------------------------------------------ store links

-- Storage adapters; only the SECURITY DEFINER SQL entry points call these.
CREATE FUNCTION tinyactors._envelope(envelope jsonb, peer text) RETURNS text
    LANGUAGE c AS 'MODULE_PATHNAME', 'ta_postgres_envelope';
CREATE FUNCTION tinyactors._repeer() RETURNS void
    LANGUAGE c AS 'MODULE_PATHNAME', 'ta_postgres_repeer';
CREATE FUNCTION tinyactors._error(origin text, session bigint, sendid text, data jsonb) RETURNS boolean
    LANGUAGE c AS 'MODULE_PATHNAME', 'ta_postgres_error';

-- An envelope from another system (docs/protocol.md, "Envelope"): delivered to
-- a session or @name and processed now, in this transaction. Returns 'ok',
-- 'duplicate' (its msgid/dedup key was seen within tinyactors.dedup_ttl:
-- nothing happens), or the reason it is refused: 'malformed', 'unreachable'
-- (another system's address), 'unknown-target'. The origin is kept: replies
-- go back out through the outbox.
CREATE FUNCTION tinyactors.deliver(envelope jsonb, peer text DEFAULT NULL) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    -- from a store link (peer): only as a peer this role may act as, and only
    -- with an origin that peer speaks for (its id, or what its hello announced);
    -- without: the application's own host code (the 'app' privileges)
    IF peer IS NOT NULL THEN
        PERFORM tinyactors._require(peer);
    ELSIF NOT tinyactors._has('app') THEN
        RAISE EXCEPTION 'tinyactors: deliver(envelope) without a peer needs the app privileges'
            USING ERRCODE = 'insufficient_privilege';
    END IF;
    RETURN tinyactors._envelope(envelope, peer);
END $$;

-- A store link's hello: peer `id` reaches the systems in `reach` (and itself).
-- Envelopes for them, also those already waiting, are for it (claim_for).
-- Returns this database's system id (the hello back).
CREATE FUNCTION tinyactors.hello(id text, reach text[] DEFAULT '{}') RETURNS text LANGUAGE plpgsql AS $$
#variable_conflict use_column
BEGIN
    PERFORM tinyactors._require(hello.id);
    -- a peer may announce systems nobody was granted (browsers behind it), or ones it
    -- may act as itself — never another granted peer's system
    hello.reach := ARRAY(SELECT r FROM unnest(coalesce(hello.reach, '{}')) r
                          WHERE NOT EXISTS (SELECT 1 FROM tinyactors.peer_roles g WHERE g.peer = r)
                             OR tinyactors.may_act_as(r));
    INSERT INTO tinyactors.peers AS p (id, reach) VALUES (hello.id, coalesce(hello.reach, '{}'))
        ON CONFLICT (id) DO UPDATE SET reach = EXCLUDED.reach, hello_at = now();
    PERFORM tinyactors._repeer();
    RETURN tinyactors.system_id();
END $$;

-- Claim up to n envelopes for one peer, oldest first (a claim older than lease
-- is taken over). One worker per peer keeps them in order.
CREATE FUNCTION tinyactors.claim_for(peer text, n integer DEFAULT 100, worker text DEFAULT NULL,
                                     lease interval DEFAULT '30 seconds')
    RETURNS SETOF tinyactors.outbox LANGUAGE sql AS $$
    SELECT tinyactors._require(peer);
    WITH c AS (
        UPDATE tinyactors.outbox o
           SET claimed_at = now(), claimed_by = worker, attempts = o.attempts + 1
         WHERE o.id IN (SELECT x.id FROM tinyactors.outbox x
                         WHERE x.peer = claim_for.peer AND x.delivered_at IS NULL AND x.error IS NULL
                           AND (x.claimed_at IS NULL OR x.claimed_at < now() - lease)
                         ORDER BY x.id
                         LIMIT n
                         FOR UPDATE SKIP LOCKED)
        RETURNING o.*)
    SELECT * FROM c ORDER BY id;
$$;

-- An `err` from another system about an envelope sent from here ({origin,
-- msgid, sendid, reason}): the sending session gets error.communication.
-- False if the origin is not a session of this database.
CREATE FUNCTION tinyactors.deliver_error(err jsonb) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    id bigint;
BEGIN
    SELECT x.id INTO id FROM tinyactors.outbox x
     WHERE x.envelope->>'msgid' = err->>'msgid' AND x.envelope->>'origin' = err->>'origin';
    IF id IS NOT NULL THEN
        PERFORM tinyactors.fail(id, coalesce(err->>'reason', 'unreachable'));  -- checks the peer
        RETURN true;
    END IF;
    IF NOT tinyactors._has('app') THEN  -- only host code tells sessions about other errors
        RETURN false;
    END IF;
    RETURN tinyactors._error(err->>'origin', NULL, err->>'sendid', jsonb_build_object('error', err->>'reason'));
END $$;

-- Prune the dedup window (call it now and then, e.g. with run()).
CREATE FUNCTION tinyactors.prune_seen() RETURNS integer LANGUAGE sql AS $$
    WITH d AS (DELETE FROM tinyactors.seen
                WHERE at < now() - coalesce(nullif(current_setting('tinyactors.dedup_ttl', true), ''), '7 days')::interval
                RETURNING 1)
    SELECT count(*)::integer FROM d;
$$;

-- Stop an actor (no done event); its pending events and timers are dropped.
CREATE FUNCTION tinyactors.stop(actor bigint) RETURNS void LANGUAGE sql AS $$
    UPDATE tinyactors.actors SET status = 'stopped', state = NULL, next_due_at = NULL, updated_at = now()
     WHERE id = actor AND status = 'active';
    DELETE FROM tinyactors.inbox WHERE inbox.actor = stop.actor;
$$;

-- Claim up to n undelivered outbox rows for the application's I/O processors
-- (a claim older than lease is taken over). Envelopes for other systems are
-- not among them: store links take those (claim_for).
CREATE FUNCTION tinyactors.claim(n integer DEFAULT 10, worker text DEFAULT NULL,
                                 lease interval DEFAULT '30 seconds')
    RETURNS SETOF tinyactors.outbox LANGUAGE sql AS $$
    UPDATE tinyactors.outbox o
       SET claimed_at = now(), claimed_by = worker, attempts = o.attempts + 1
     WHERE o.id IN (SELECT id FROM tinyactors.outbox
                     WHERE delivered_at IS NULL AND error IS NULL AND peer IS NULL
                       AND (claimed_at IS NULL OR claimed_at < now() - lease)
                     ORDER BY id
                     LIMIT n
                     FOR UPDATE SKIP LOCKED)
    RETURNING o.*;
$$;

-- The effect was performed.
CREATE FUNCTION tinyactors.ack(id bigint) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    PERFORM tinyactors._require((SELECT o.peer FROM tinyactors.outbox o WHERE o.id = ack.id));
    UPDATE tinyactors.outbox SET delivered_at = now() WHERE outbox.id = ack.id;
END $$;

-- The effect failed for good: the session that sent it gets error.communication
-- (with the sendid).
CREATE FUNCTION tinyactors.fail(id bigint, error text DEFAULT 'failed') RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    o tinyactors.outbox;
BEGIN
    PERFORM tinyactors._require((SELECT x.peer FROM tinyactors.outbox x WHERE x.id = fail.id));
    UPDATE tinyactors.outbox SET error = fail.error WHERE outbox.id = fail.id RETURNING * INTO o;
    IF o.id IS NOT NULL AND EXISTS (SELECT 1 FROM tinyactors.actors a WHERE a.id = o.actor AND a.status = 'active') THEN
        PERFORM tinyactors._error(NULL, coalesce(o.session, o.actor), o.sendid, jsonb_build_object('error', fail.error));
    END IF;
END $$;

-- What a chart's current version is.
CREATE VIEW tinyactors.current_charts AS
    SELECT DISTINCT ON (name) name, version, datamodel, created_at
      FROM tinyactors.charts ORDER BY name, version DESC;

-- ------------------------------------------------------------- privileges
--
-- Nothing is executable by PUBLIC. The API runs SECURITY DEFINER (as the
-- extension's owner, with a fixed search_path), so roles need no privileges on
-- the tables: tinyactors.grant(role, what) hands out function groups —
--   app     spawn, send, deliver (actors and envelopes from host code), process,
--           run, stop; SELECT on actors and charts
--   worker  claim, ack, fail (the outbox of the application's I/O processors);
--           SELECT on outbox
--   link    hello, claim_for, ack, fail, deliver(envelope, peer), deliver_error —
--           only as peers granted with tinyactors.grant_peer(peer, role)
--   admin   all of them, define/compile, prune_seen, and all table privileges
-- tinyactors.now (the actors' clock) can only be set by superusers.

-- may the session's role act as store-link peer `peer`?
CREATE FUNCTION tinyactors.may_act_as(peer text) RETURNS boolean LANGUAGE sql STABLE AS $$
    SELECT pg_has_role(session_user, (SELECT c.relowner FROM pg_class c
                                       WHERE c.oid = 'tinyactors.actors'::regclass), 'MEMBER')
        OR EXISTS (SELECT 1 FROM tinyactors.peer_roles r
                    WHERE r.peer = may_act_as.peer AND pg_has_role(session_user, r.role, 'MEMBER'));
$$;

-- does the session's role have a group's privileges ('app', 'worker')?
CREATE FUNCTION tinyactors._has(what text) RETURNS boolean LANGUAGE sql STABLE AS $$
    SELECT has_function_privilege(session_user,
             CASE what WHEN 'app' THEN 'tinyactors.spawn(text,jsonb,text)'
                       WHEN 'worker' THEN 'tinyactors.claim(integer,text,interval)' END, 'EXECUTE');
$$;

-- an outbox row's or a peer's operations: as that peer (peer NULL: the worker's rows)
CREATE FUNCTION tinyactors._require(peer text) RETURNS void LANGUAGE plpgsql STABLE AS $$
BEGIN
    IF peer IS NULL THEN
        IF NOT tinyactors._has('worker') THEN
            RAISE EXCEPTION 'tinyactors: not allowed: the outbox of I/O processors needs the worker privileges'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
    ELSIF NOT tinyactors.may_act_as(peer) THEN
        RAISE EXCEPTION 'tinyactors: not allowed to act as peer "%"', peer
            USING ERRCODE = 'insufficient_privilege',
                  HINT = format('SELECT tinyactors.grant_peer(%L, <role>);', peer);
    END IF;
END $$;

CREATE FUNCTION tinyactors.grant(role name, what text DEFAULT 'app') RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    f text;
    fs text[];
    app text[] := ARRAY['tinyactors.spawn(text,jsonb,text)', 'tinyactors.send(bigint,text,jsonb)',
                        'tinyactors.send(text,text,jsonb)', 'tinyactors.deliver(bigint,text,jsonb)',
                        'tinyactors.deliver(jsonb,text)', 'tinyactors.process(bigint)',
                        'tinyactors.run(integer)', 'tinyactors.stop(bigint)', 'tinyactors.deliver_error(jsonb)'];
    worker text[] := ARRAY['tinyactors.claim(integer,text,interval)', 'tinyactors.ack(bigint)',
                           'tinyactors.fail(bigint,text)'];
    link text[] := ARRAY['tinyactors.hello(text,text[])', 'tinyactors.claim_for(text,integer,text,interval)',
                         'tinyactors.ack(bigint)', 'tinyactors.fail(bigint,text)',
                         'tinyactors.deliver(jsonb,text)', 'tinyactors.deliver_error(jsonb)'];
    common text[] := ARRAY['tinyactors.now()', 'tinyactors.system_id()', 'tinyactors.address(text)',
                           'tinyactors.data_ok(jsonb)'];
BEGIN
    fs := CASE what WHEN 'app' THEN app WHEN 'worker' THEN worker WHEN 'link' THEN link
                    WHEN 'admin' THEN app || worker || link || ARRAY['tinyactors.define(text,text)',
                         'tinyactors.compile(text,text)', 'tinyactors.chart_datamodel(bytea)',
                         'tinyactors.prune_seen()']
         END;
    IF fs IS NULL THEN
        RAISE EXCEPTION 'tinyactors.grant: what is app, worker, link or admin (not %)', what;
    END IF;
    EXECUTE format('GRANT USAGE ON SCHEMA tinyactors TO %I', role);
    FOREACH f IN ARRAY fs || common LOOP
        EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO %I', f, role);
    END LOOP;
    IF what IN ('app', 'admin') THEN
        EXECUTE format('GRANT SELECT ON tinyactors.actors, tinyactors.charts, tinyactors.current_charts TO %I', role);
    END IF;
    IF what IN ('worker', 'admin') THEN
        EXECUTE format('GRANT SELECT ON tinyactors.outbox TO %I', role);
    END IF;
    IF what = 'admin' THEN
        EXECUTE format('GRANT ALL ON ALL TABLES IN SCHEMA tinyactors TO %I', role);
        EXECUTE format('GRANT ALL ON ALL SEQUENCES IN SCHEMA tinyactors TO %I', role);
    END IF;
END $$;
COMMENT ON FUNCTION tinyactors.grant IS 'Give a role the app, worker, link or admin privileges';

CREATE FUNCTION tinyactors.grant_peer(peer text, role name) RETURNS void LANGUAGE sql AS $$
    INSERT INTO tinyactors.peer_roles VALUES (peer, role) ON CONFLICT DO NOTHING;
$$;
COMMENT ON FUNCTION tinyactors.grant_peer IS 'Allow a role to act as a store-link peer (with the link privileges)';

REVOKE ALL ON ALL FUNCTIONS IN SCHEMA tinyactors FROM PUBLIC;
DO $$
DECLARE
    f regprocedure;
BEGIN
    FOR f IN SELECT p.oid::regprocedure FROM pg_proc p WHERE p.pronamespace = 'tinyactors'::regnamespace LOOP
        EXECUTE format('ALTER FUNCTION %s SECURITY DEFINER SET search_path = pg_catalog, pg_temp', f);
    END LOOP;
END $$;
