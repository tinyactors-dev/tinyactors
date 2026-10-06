#!/usr/bin/env bash
# Concurrency and durability, against the server test/run.sh started
# (PGHOST/PGPORT/PGUSER set; TA_PG_DATA is its data directory):
#   1. one hot actor, many clients: every event counted (no lost updates)
#   2. actors messaging each other under contention: events skipped under
#      lock are not lost (run() delivers them), totals add up
#   3. a rolled-back send leaks no events to other actors
#   4. a restart: state is durable, timers armed before it still fire
set -euo pipefail
DB=tinyactors_concurrency
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
PGOPTIONS="-c client_min_messages=warning" psql -qX -c "DROP DATABASE IF EXISTS $DB" -c "CREATE DATABASE $DB" > /dev/null
q() { psql -qXtA -v ON_ERROR_STOP=1 -d "$DB" -c "$1"; }
check() { # name, sql, expected
  local got
  got="$(q "$2")"
  if [ "$got" != "$3" ]; then echo "FAIL $1: got '$got', expected '$3'"; exit 1; fi
  echo "ok   $1 ($3)"
}

psql -qX -v ON_ERROR_STOP=1 -d "$DB" > /dev/null <<'SQL'
CREATE EXTENSION tinyactors;
SELECT tinyactors.define('counter', $x$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="counter" initial="s">
  <datamodel><data id="count" expr="0"/></datamodel>
  <state id="s"><transition event="inc"><assign location="count" expr="count + 1"/></transition></state>
</scxml>$x$);
SELECT tinyactors.define('pinger', $x$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="pinger" initial="s">
  <datamodel><data id="peer" expr="null"/><data id="pings" expr="0"/><data id="pongs" expr="0"/><data id="pokes" expr="0"/></datamodel>
  <state id="s">
    <transition event="ping"><assign location="pings" expr="pings + 1"/><send targetexpr="peer" event="pong"/></transition>
    <transition event="pong"><assign location="pongs" expr="pongs + 1"/></transition>
    <transition event="poke"><assign location="pokes" expr="pokes + 1"/></transition>
  </state>
</scxml>$x$);
SELECT tinyactors.define('reminder', $x$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="reminder" initial="waiting">
  <state id="waiting"><onentry><send event="ring" delay="60s"/></onentry><transition event="ring" target="rang"/></state>
  <state id="rang"/>
</scxml>$x$);
SELECT tinyactors.spawn('counter', '{}', 'hot');
DO $$ DECLARE i int; b bigint; BEGIN
  FOR i IN 1..20 LOOP
    b := tinyactors.spawn('pinger', '{}', 'b' || i);
    PERFORM tinyactors.spawn('pinger', jsonb_build_object('peer', '#_scxml_' || b), 'a' || i);
  END LOOP;
END $$;
SQL

# 1. one hot actor
echo "SELECT 1 FROM tinyactors.send('hot', 'inc');" > "$TMP/hot.sql"
pgbench -n -c 8 -j 4 -t 250 -f "$TMP/hot.sql" "$DB" > "$TMP/hot.log" 2>&1 || { cat "$TMP/hot.log"; exit 1; }
check "hot actor: 8 clients x 250 events, none lost" "SELECT datamodel->>'count' FROM tinyactors.actors WHERE name = 'hot'" 2000
check "hot actor: events counted once" "SELECT events FROM tinyactors.actors WHERE name = 'hot'" 2000

# 2. pairs under contention: a_i pings b_i (b_i may be busy with pokes: SKIP LOCKED)
cat > "$TMP/ping.sql" <<'SQL'
\set i random(1, 20)
SELECT 1 FROM tinyactors.send('a' || :i, 'ping');
SQL
cat > "$TMP/poke.sql" <<'SQL'
\set i random(1, 20)
SELECT 1 FROM tinyactors.send('b' || :i, 'poke');
SQL
pgbench -n -c 8 -j 4 -t 300 -f "$TMP/ping.sql" -f "$TMP/poke.sql" "$DB" > "$TMP/pairs.log" 2>&1 || { cat "$TMP/pairs.log"; exit 1; }
left="$(q "SELECT count(*) FROM tinyactors.inbox")"
echo "     $left events were left for run() (receiver busy)"
for _ in $(seq 1 50); do
  [ "$(q "SELECT count(*) FROM tinyactors.inbox")" = 0 ] && break
  q "SELECT tinyactors.run()" > /dev/null
done
check "pairs: inbox drained" "SELECT count(*) FROM tinyactors.inbox" 0
check "pairs: every ping and poke processed" \
  "SELECT sum((datamodel->>'pings')::int + (datamodel->>'pokes')::int) FROM tinyactors.actors WHERE chart = 'pinger'" 2400
check "pairs: one pong per ping" \
  "SELECT sum((datamodel->>'pongs')::int) = sum((datamodel->>'pings')::int) FROM tinyactors.actors WHERE chart = 'pinger'" t

# 2b. deterministically: b1 is busy (another transaction holds it); a1's pong
#     waits in b1's inbox — the ping itself commits — and run() delivers it later
pongs="$(q "SELECT datamodel->>'pongs' FROM tinyactors.actors WHERE name = 'b1'")"
psql -qXtA -d "$DB" -c "BEGIN; SELECT 1 FROM tinyactors.actors WHERE name = 'b1' FOR NO KEY UPDATE; SELECT pg_sleep(2); COMMIT;" > /dev/null &
holder=$!
sleep 0.5
check "busy receiver: the sender does not wait" "SELECT datamodel->>'pings' FROM tinyactors.send('a1', 'ping')" \
  "$(( $(q "SELECT datamodel->>'pings' FROM tinyactors.actors WHERE name = 'a1'") + 1 ))"
check "busy receiver: the pong waits in its inbox" \
  "SELECT count(*) FROM tinyactors.inbox i JOIN tinyactors.actors a ON a.id = i.actor WHERE a.name = 'b1'" 1
wait "$holder"
check "busy receiver: run() delivers it" "SELECT tinyactors.run() >= 1" t
check "busy receiver: one more pong" "SELECT datamodel->>'pongs' FROM tinyactors.actors WHERE name = 'b1'" "$((pongs + 1))"

# 3. rollback: the ping and the pong it caused are both gone
before="$(q "SELECT datamodel->>'pongs' FROM tinyactors.actors WHERE name = 'b1'")"
q "BEGIN; SELECT tinyactors.send('a1', 'ping'); ROLLBACK;" > /dev/null
check "rollback leaks no events" "SELECT datamodel->>'pongs' FROM tinyactors.actors WHERE name = 'b1'" "$before"

# 4. a restart: durable state, timers armed before it fire after it
q "SET tinyactors.now = '2030-01-01 00:00:00+00'; SELECT tinyactors.spawn('reminder', '{}', 'alarm')" > /dev/null
pg_ctl -D "$TA_PG_DATA" -m fast restart -w -l "$TA_PG_DATA/server.log" > /dev/null
check "after a restart: state" "SELECT datamodel->>'count' FROM tinyactors.actors WHERE name = 'hot'" 2000
check "after a restart: nothing due at 00:00:30" \
  "SET tinyactors.now = '2030-01-01 00:00:30+00'; SELECT tinyactors.run()" 0
check "after a restart: the timer fires at 00:01:01" \
  "SET tinyactors.now = '2030-01-01 00:01:01+00'; SELECT tinyactors.run(); SELECT configuration FROM tinyactors.actors WHERE name = 'alarm'" $'1\n{rang}'
