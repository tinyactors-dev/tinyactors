#!/usr/bin/env bash
# A throwaway PostgreSQL server for the tests: pg_regress suites (test/sql,
# test/expected), then the concurrency/durability checks (test/concurrency.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
PGDIR="$(mise where conda:postgresql)"
export PATH="$PGDIR/bin:$PATH"
DATA="$(pwd)/../../build/pg-test"
PORT="${TA_PG_TEST_PORT:-54329}"
SOCK="$(mktemp -d)"
rm -rf "$DATA"
initdb -D "$DATA" -U postgres --auth=trust --no-instructions > /dev/null
# a ceiling for the throwaway server: a test that fails to bound memory must not
# take the machine down with it (address space per process, in kB)
ulimit -v 4000000
# pg_stat_statements: harden.py H24 counts the statements an event costs
pg_ctl -D "$DATA" -o "-p $PORT -k $SOCK -c listen_addresses='' -c fsync=off -c shared_preload_libraries=pg_stat_statements" -l "$DATA/server.log" -w start > /dev/null
trap 'pg_ctl -D "$DATA" -m fast stop > /dev/null; rm -rf "$SOCK"' EXIT
export PGHOST="$SOCK" PGPORT="$PORT" PGUSER=postgres TA_PG_DATA="$DATA"
mkdir -p build/regress
# TA_PG_ONLY=harden [TA_PG_K=h6]: only the hardening reproductions (while working on them)
if [ "${TA_PG_ONLY:-}" != harden ]; then
  if ! make -s PG_CONFIG="$PGDIR/bin/pg_config" installcheck; then
    for f in build/regress/regression.diffs; do [ -f "$f" ] && cat "$f"; done
    exit 1
  fi
  bash test/concurrency.sh
fi
python3 test/harden.py ${TA_PG_K:+-k "$TA_PG_K"}
