#!/usr/bin/env bash
# A PostgreSQL instance with the tinyactors extension, for this machine:
#   data      ~/.local/share/tinyactors/pg (kept across runs)
#   listens   127.0.0.1:54320 and the socket in /tmp
#   database  tinyactors (CREATE EXTENSION done; a demo chart defined)
# In an Amp orb it runs as the supervised service "tinyactors-pg" (survives
# CLI restarts and orb pause/resume); elsewhere pg_ctl starts it. Idempotent.
set -euo pipefail
cd "$(dirname "$0")/.."
PGDIR="$(mise where conda:postgresql)"
export PATH="$PGDIR/bin:$PATH"
DATA="${TA_PG_DATA:-$HOME/.local/share/tinyactors/pg}"
PORT="${TA_PG_PORT:-54320}"
DB="${TA_PG_DB:-tinyactors}"

if [ ! -f "$DATA/PG_VERSION" ]; then
  mkdir -p "$(dirname "$DATA")"
  initdb -D "$DATA" -U postgres --auth=trust --no-instructions > /dev/null
  echo "initialized $DATA"
fi

if pg_isready -q -h 127.0.0.1 -p "$PORT"; then
  echo "already running on 127.0.0.1:$PORT"
elif command -v amp > /dev/null && amp orb service list > /dev/null 2>&1; then
  amp orb service start tinyactors-pg --command \
    "$PGDIR/bin/postgres -D $DATA -p $PORT -k /tmp -c listen_addresses=127.0.0.1" > /dev/null
  echo "started the orb service tinyactors-pg"
else
  pg_ctl -D "$DATA" -o "-p $PORT -k /tmp -c listen_addresses=127.0.0.1" -l "$DATA/server.log" start > /dev/null
  echo "started with pg_ctl (log: $DATA/server.log)"
fi
for _ in $(seq 1 60); do pg_isready -q -h 127.0.0.1 -p "$PORT" && break; sleep 0.25; done
pg_isready -q -h 127.0.0.1 -p "$PORT" || { echo "PostgreSQL did not come up"; exit 1; }

psql -qX -h 127.0.0.1 -p "$PORT" -U postgres -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname = '$DB'" | grep -q 1 \
  || psql -qX -h 127.0.0.1 -p "$PORT" -U postgres -d postgres -c "CREATE DATABASE $DB" > /dev/null
psql -qX -v ON_ERROR_STOP=1 -h 127.0.0.1 -p "$PORT" -U postgres -d "$DB" > /dev/null <<'SQL'
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS tinyactors;
-- a demo chart, so there is something to look at
SELECT tinyactors.define('order', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="order" initial="cart">
  <datamodel><data id="items" expr="[]"/><data id="total" expr="0"/></datamodel>
  <state id="cart">
    <transition event="add"><script>items.push(_event.data.sku)</script><assign location="total" expr="total + _event.data.price"/></transition>
    <transition event="checkout" cond="items.length &gt; 0" target="paying">
      <send type="outbox" target="payments" event="charge" id="charge"><param name="amount" expr="total"/></send>
    </transition>
  </state>
  <state id="paying">
    <onentry><send event="payment.timeout" delay="900s" id="timeout"/></onentry>
    <transition event="paid" target="shipped"><cancel sendid="timeout"/></transition>
    <transition event="payment.timeout" target="cart"/>
    <transition event="error.communication" target="cart"/>
  </state>
  <final id="shipped"><donedata><param name="total" expr="total"/></donedata></final>
</scxml>$scxml$)
WHERE NOT EXISTS (SELECT 1 FROM tinyactors.charts WHERE name = 'order');
SQL
echo "ready: psql -h 127.0.0.1 -p $PORT -U postgres -d $DB"
