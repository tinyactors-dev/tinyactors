#!/usr/bin/env bash
# Restart the instance `mise run pg:start` runs (a crash-free restart: a fast
# shutdown, then up again) — for durability demonstrations.
set -euo pipefail
cd "$(dirname "$0")/.."
PGDIR="$(mise where conda:postgresql)"
DATA="${TA_PG_DATA:-$HOME/.local/share/tinyactors/pg}"
PORT="${TA_PG_PORT:-54320}"
if command -v amp > /dev/null && amp orb service status tinyactors-pg > /dev/null 2>&1; then
  amp orb service restart tinyactors-pg > /dev/null
else
  "$PGDIR/bin/pg_ctl" -D "$DATA" -m fast restart -w -l "$DATA/server.log" > /dev/null
fi
for _ in $(seq 1 120); do "$PGDIR/bin/pg_isready" -q -h 127.0.0.1 -p "$PORT" && break; sleep 0.25; done
echo "restarted"
