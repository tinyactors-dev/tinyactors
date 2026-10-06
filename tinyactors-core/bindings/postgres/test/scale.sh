#!/usr/bin/env bash
# Millions of durable actors, a few of them hot — against the instance
# `mise run pg:start` runs (a database of its own, dropped at the start):
#
#   population  N session actors (a small set of ids, a 1 h timeout that every
#               touch resets: the common timeout pattern) — every one of them
#               has a pending timer, so actors_due holds N entries
#   idle        run() with nothing due: must cost what it finds, not N
#   hot         pgbench touches H actors (row cache warm)
#   cold        pgbench touches random actors among all N
#   one hot     8 clients on one actor for 3 x SECS: its row lock serializes
#               them; the rate must not decay as its row versions pile up
#   due         the clock moved 2 h ahead: all N are due; run(5000) repeatedly
#               in one backend — cost per call independent of N, backend
#               memory flat
#
#   TA_SCALE_N (2000000)  TA_SCALE_HOT (10000)  TA_SCALE_SECONDS (15 per load)
#   TA_PG_PORT (54320)  TA_SCALE_KEEP=1 keeps the database (dropped otherwise:
#   ~500 bytes per actor)
set -euo pipefail
cd "$(dirname "$0")/.."
PGDIR="$(mise where conda:postgresql)"
export PATH="$PGDIR/bin:$PATH"
N="${TA_SCALE_N:-2000000}"
H="${TA_SCALE_HOT:-10000}"
SECS="${TA_SCALE_SECONDS:-15}"
export PGHOST=127.0.0.1 PGPORT="${TA_PG_PORT:-54320}" PGUSER=postgres
DB=tinyactors_scale
TMP="$(mktemp -d)"
cleanup() {
  rm -rf "$TMP"
  [ "${TA_SCALE_KEEP:-}" = 1 ] || psql -qX -d postgres -c "DROP DATABASE IF EXISTS $DB" > /dev/null 2>&1 || true
}
trap cleanup EXIT
pg_isready -q || { echo "PostgreSQL is not running on $PGHOST:$PGPORT (mise run pg:start)"; exit 1; }
q() { psql -qXtA -v ON_ERROR_STOP=1 -d "$DB" -c "$1"; }
fail() { echo "FAIL $*"; exit 1; }
now() { date +%s.%N; }
secs() { python3 -c "print(f'{$2 - $1:.1f}')"; }

PGOPTIONS="-c client_min_messages=warning" psql -qX -d postgres -c "DROP DATABASE IF EXISTS $DB" -c "CREATE DATABASE $DB" > /dev/null
psql -qX -v ON_ERROR_STOP=1 -d "$DB" > /dev/null <<'SQL'
SET client_min_messages = warning;
CREATE EXTENSION tinyactors;
SELECT tinyactors.define('session', $x$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="session" initial="active">
  <datamodel>
    <data id="owner" expr="0"/>
    <data id="ids" expr="[]"/>
    <data id="touches" expr="0"/>
  </datamodel>
  <state id="active">
    <onentry><send event="timeout" delay="3600s" id="timeout"/></onentry>
    <transition event="touch" target="active">
      <cancel sendid="timeout"/>
      <if cond="ids.indexOf(_event.data.id) &lt; 0 &amp;&amp; ids.length &lt; 16">
        <script>ids.push(_event.data.id)</script>
      </if>
      <assign location="touches" expr="touches + 1"/>
    </transition>
    <transition event="timeout" target="expired"/>
  </state>
  <final id="expired"/>
</scxml>$x$);
SQL

# ---- population: 4 clients, 2,000 spawns per transaction
B=2000
C=4
T=$(( (N + B * C - 1) / (B * C) ))
echo "SELECT count(tinyactors.spawn('session', jsonb_build_object('owner', :client_id))) FROM generate_series(1, $B);" > "$TMP/spawn.sql"
t0=$(now)
pgbench -n -c $C -j $C -t $T -f "$TMP/spawn.sql" "$DB" > "$TMP/spawn.log" 2>&1 || { cat "$TMP/spawn.log"; exit 1; }
t1=$(now)
total="$(q "SELECT count(*) FROM tinyactors.actors")"
[ "$total" -ge "$N" ] || fail "population: $total < $N"
q "VACUUM ANALYZE tinyactors.actors" > /dev/null
spawn_s=$(secs "$t0" "$t1")
rate=$(python3 -c "print(round($total / $spawn_s))")
read -r rel idx <<< "$(q "SELECT pg_relation_size('tinyactors.actors') + coalesce(pg_relation_size(reltoastrelid), 0), pg_indexes_size('tinyactors.actors') FROM pg_class WHERE oid = 'tinyactors.actors'::regclass" | tr '|' ' ')"
per=$(( (rel + idx) / total ))
echo "ok   population: $total actors in ${spawn_s} s ($rate/s); $(( (rel + idx) / 1048576 )) MB with indexes ($per B per actor)"
due_entries="$(q "SELECT count(*) FROM tinyactors.actors WHERE status = 'active' AND next_due_at IS NOT NULL")"
[ "$due_entries" = "$total" ] || fail "every actor should have a pending timer ($due_entries)"

# ---- idle: nothing due, nothing queued
plan="$(q "EXPLAIN SELECT id FROM tinyactors.actors WHERE status = 'active' AND next_due_at <= now() ORDER BY next_due_at LIMIT 1000 FOR NO KEY UPDATE SKIP LOCKED")"
echo "$plan" | grep -q "actors_due" || { echo "$plan"; fail "run() does not use the actors_due index"; }
idle_ms="$(psql -qXtA -d "$DB" -c "SET client_min_messages = notice" -c "DO \$\$ DECLARE t0 timestamptz := clock_timestamp(); n int := 0; BEGIN FOR i IN 1..100 LOOP n := n + tinyactors.run(1000); END LOOP; IF n <> 0 THEN RAISE EXCEPTION 'run() processed % actors with nothing due', n; END IF; RAISE NOTICE 'idle %', round((extract(epoch FROM clock_timestamp() - t0) * 10)::numeric, 3); END \$\$" 2>&1 | sed -n 's/.*idle //p')"
[ -n "$idle_ms" ] || fail "idle run() measurement"
python3 -c "import sys; sys.exit(0 if $idle_ms < 50 else 1)" || fail "idle run() takes $idle_ms ms with $total actors"
echo "ok   idle: run() with nothing due takes $idle_ms ms ($total actors, all with timers)"

# ---- hot and cold touches
lo="$(q "SELECT min(id) FROM tinyactors.actors")"
hi="$(q "SELECT max(id) FROM tinyactors.actors")"
touch_load() { # name, lo, hi
  cat > "$TMP/$1.sql" <<SQL
\\set id random($2, $3)
\\set k random(1, 1000)
SELECT 1 FROM tinyactors.send(:id, 'touch', jsonb_build_object('id', :k));
SQL
  pgbench -n -c 8 -j 4 -T "$SECS" -f "$TMP/$1.sql" "$DB" > "$TMP/$1.log" 2>&1 || { cat "$TMP/$1.log"; exit 1; }
  local tps lat
  tps="$(sed -n 's/^tps = \([0-9.]*\).*/\1/p' "$TMP/$1.log")"
  lat="$(sed -n 's/^latency average = \([0-9.]*\) ms/\1/p' "$TMP/$1.log")"
  echo "${tps%.*} $lat"
}
read -r hot_tps hot_lat <<< "$(touch_load hot "$lo" $((lo + H - 1)))"
echo "ok   hot: $H actors touched by 8 clients: $hot_tps events/s, $hot_lat ms average"
read -r cold_tps cold_lat <<< "$(touch_load cold "$lo" "$hi")"
echo "ok   cold: random actors among $total: $cold_tps events/s, $cold_lat ms average"
touched="$(q "SELECT sum((datamodel->>'touches')::int) FROM tinyactors.actors")"
errors="$(cat "$TMP/hot.log" "$TMP/cold.log" | grep -c 'ERROR' || true)"
[ "$errors" = 0 ] || fail "touch errors"
echo "ok   touches recorded: $touched"

# ---- one hot actor, sustained: 8 clients, one row lock
echo "SELECT 1 FROM tinyactors.send($lo, 'touch', jsonb_build_object('id', 1));" > "$TMP/one.sql"
pgbench -n -c 8 -j 4 -T $((3 * SECS)) -P "$SECS" -f "$TMP/one.sql" "$DB" > "$TMP/one.log" 2>&1 || { cat "$TMP/one.log"; exit 1; }
one=($(sed -n 's/^progress: .* \([0-9.]*\) tps.*/\1/p' "$TMP/one.log" | cut -d. -f1))
[ "${#one[@]}" -ge 3 ] || { cat "$TMP/one.log"; fail "one hot actor: no progress lines"; }
[ $(( one[2] * 2 )) -ge "${one[0]}" ] || fail "one hot actor decays: ${one[*]} events/s"
echo "ok   one hot actor: ${one[*]} events/s over $((3 * SECS)) s (8 clients serialized by its row lock)"

# ---- everything due: run() costs what it processes, memory stays flat.
# Twelve calls of run(5000) in one backend expire 60,000 actors. Memory: the
# backend's PostgreSQL contexts must stay small (nothing is kept per actor),
# and its private memory (RssAnon: RSS also counts the shared buffers the
# backend has touched, up to shared_buffers) must not grow with the actors
# processed.
{
  echo "SET client_min_messages = warning;"
  echo "SET tinyactors.now = '$(q "SELECT (now() + interval '2 hours')::text")';"
  printf 'SELECT pg_backend_pid() AS pid \\gset\n\\setenv TA_PID :pid\n\\timing on\n'
  for _ in $(seq 1 12); do printf 'SELECT tinyactors.run(5000);\n\\! grep RssAnon /proc/$TA_PID/status\n'; done
  printf '\\timing off\nSELECT sum(total_bytes) / 1024 AS contexts_kb FROM pg_backend_memory_contexts;\n'
} > "$TMP/due.sql"
psql -qXtA -v ON_ERROR_STOP=1 -d "$DB" -f "$TMP/due.sql" > "$TMP/due.log" 2>&1 || { cat "$TMP/due.log"; exit 1; }
runs="$(grep -c '^5000$' "$TMP/due.log")"
[ "$runs" = 12 ] || { cat "$TMP/due.log"; fail "run(5000) with $total actors due should process 5000 each time"; }
ms="$(sed -n 's/^Time: \([0-9.]*\) ms.*/\1/p' "$TMP/due.log" | head -12 | tr '\n' ' ')"
rss=($(sed -n 's/^RssAnon:[[:space:]]*\([0-9]*\) kB/\1/p' "$TMP/due.log"))
contexts_kb="$(tail -1 "$TMP/due.log" | cut -d. -f1)"
avg="$(echo "$ms" | awk '{for (i = 1; i <= NF; i++) s += $i; printf "%.0f", s / NF}')"
expired="$(q "SELECT count(*) FROM tinyactors.actors WHERE status = 'done'")"
[ "$expired" = 60000 ] || fail "60000 timeouts should have fired ($expired)"
[ "$contexts_kb" -lt 32768 ] || fail "the backend's memory contexts hold $contexts_kb kB after 60,000 actors"
growth=$(( rss[11] - rss[3] ))
[ "$growth" -lt 8192 ] || fail "backend memory kept growing: ${rss[*]} kB"
echo "ok   due: all $total due; run(5000) x12 in one backend: ${avg} ms per call ($(( 5000 * 1000 / avg )) timers/s); per call: $ms"
echo "ok   memory: backend private memory ${rss[0]} kB after the first call, ${rss[3]} after 4, ${rss[11]} after 12; PostgreSQL contexts $contexts_kb kB"

echo
echo "| Actors | Spawn rate | Size per actor | Idle run() | Hot set events/s (latency) | Cold events/s (latency) | One hot actor | run(5000), all due |"
echo "|---|---|---|---|---|---|---|---|"
echo "| $total | $rate/s | $per B | $idle_ms ms | $hot_tps ($hot_lat ms) | $cold_tps ($cold_lat ms) | ${one[2]}/s | $avg ms |"
