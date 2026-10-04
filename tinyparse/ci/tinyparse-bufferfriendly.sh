#!/usr/bin/env bash
# ci/tinyparse-bufferfriendly.sh -- the W17-4 BUFFER-FRIENDLY gate.
#
# The GENERATED native parser must be drivable over a GROWING buffer: it signals
# TINYPARSE_MORE (NEED_MORE_INPUT) at the SHALLOW top-level-item boundary so a
# driver can feed it more bytes and resume at the next item -- and the tree it
# builds that way must be byte-identical to parsing the input WHOLE, and to the VM.
# Native recursion cannot suspend mid-stack and there are no fibers, so the boundary is
# the start rule's repeat of top-level items only; the explicit-stack VM stays
# the engine for arbitrary-granularity async push (tinyparse_push streaming), which
# this gate does NOT touch.
#
# The gate drives examples/bufferfriendly -- the generated sexpr parser (start rule
# Source = (seq (repeat Form) EOF), a repeat of top-level items) -- and asserts:
#   1. fed in SEVERAL chunk sizes via the TINYPARSE_MORE loop, the resulting tree
#      is byte-identical to the VM's `run --dump-tree' of the same document, AND
#   2. it is identical across every chunk size (chunked == whole == VM), AND
#   3. byte-at-a-time (chunk size 1) genuinely SUSPENDS (>0 TINYPARSE_MORE), so the
#      boundary is exercised, not bypassed.
#
# Run it with `mise run gate bufferfriendly`.
#
# Exit 0 only when chunked == whole == VM, byte for byte.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP_DIR="$REPO"
EX="$TP_DIR/examples/bufferfriendly"
cd "$REPO" || exit 1

: "${TP:?run the gates through mise run gate, which builds what they need and sets TP, TP_LIB, TP_CC and TP_NM}"
LIB="$TP_LIB"

echo "== tinyparse buffer-friendly gate (W17-4) =="

# 1. The CLI, the library archive and the example, which `mise run examples`
#    builds into $TP_EXAMPLES/bufferfriendly: the sexpr parser-lib generated
#    with the CLI (sx_load/sx_run/sx_run_chunk) linked with the buffer-friendly
#    driver.
BUILT="$TP_EXAMPLES/bufferfriendly"
if [ ! -x "$TP" ] || [ ! -f "$LIB" ]; then
  echo "FATAL: CLI ($TP) or libtinyparse ($LIB) not built"
  echo "RESULT=0/0"
  exit 1
fi
if [ ! -x "$BUILT/bufferfriendly" ]; then
  echo "FAIL: example binary $BUILT/bufferfriendly was not produced (mise run examples)"
  echo "== bufferfriendly: RED =="
  exit 1
fi

# 2b. The generated file must actually carry the buffer-friendly entry, proving the
#     boundary was emitted (not silently skipped).
if ! grep -q "sx_run_chunk" "$BUILT/sx.c"; then
  echo "FAIL: generated parser has no sx_run_chunk -- the boundary was not emitted"
  echo "== bufferfriendly: RED =="
  exit 1
fi

SAMPLE="$EX/sample.sx"

# 3. The oracle: the VM's whole-input dump of the same grammar on the same document.
VM="$("$TP" run "$EX/sexpr.scm" --input "$SAMPLE" --dump-tree)"
RC=$?
if [ "$RC" -ne 0 ]; then
  echo "FAIL: VM oracle parse exited $RC (expected 0)"
  echo "== bufferfriendly: RED =="
  exit 1
fi

# 4. Drive the GENERATED parser over the document in several chunk sizes through the
#    TINYPARSE_MORE loop; each result must equal the VM byte for byte.
ok=0
total=0
for cs in 1 2 3 5 8 17 64 1000000; do
  total=$((total + 1))
  GOT="$("$BUILT/bufferfriendly" "$SAMPLE" "$cs" 2>/tmp/bf-stderr.$$)"
  RC=$?
  MORES="$(grep -oE '[0-9]+ TINYPARSE_MORE' /tmp/bf-stderr.$$ | grep -oE '^[0-9]+' || echo 0)"
  if [ "$RC" -ne 0 ]; then
    echo "FAIL: chunk=$cs exited $RC"
    sed 's/^/    /' /tmp/bf-stderr.$$
    rm -f /tmp/bf-stderr.$$
    echo "== bufferfriendly: RED =="
    exit 1
  fi
  if ! diff -u <(printf '%s\n' "$VM") <(printf '%s\n' "$GOT") >/tmp/bf-diff.$$; then
    echo "FAIL: chunk=$cs tree differs from the VM (chunked must equal whole)"
    sed 's/^/    /' /tmp/bf-diff.$$ | head -30
    rm -f /tmp/bf-stderr.$$ /tmp/bf-diff.$$
    echo "== bufferfriendly: RED =="
    exit 1
  fi
  # The smallest chunk size must genuinely suspend, proving the boundary fires.
  if [ "$cs" = "1" ] && [ "${MORES:-0}" -le 0 ]; then
    echo "FAIL: byte-at-a-time (chunk=1) produced 0 TINYPARSE_MORE -- the boundary was bypassed"
    rm -f /tmp/bf-stderr.$$ /tmp/bf-diff.$$
    echo "== bufferfriendly: RED =="
    exit 1
  fi
  echo "  chunk=$cs: byte-identical to VM (${MORES:-0} TINYPARSE_MORE suspensions)"
  ok=$((ok + 1))
done
rm -f /tmp/bf-stderr.$$ /tmp/bf-diff.$$

echo "RESULT=$ok/$total"
if [ "$ok" -ne "$total" ]; then
  echo "== bufferfriendly: RED =="
  exit 1
fi
echo "== bufferfriendly: GREEN (chunked == whole == VM, boundary exercised) =="
exit 0
