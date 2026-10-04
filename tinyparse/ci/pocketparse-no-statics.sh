#!/usr/bin/env bash
# ci/pocketparse-no-statics.sh — the no-global-mutable-state guard for the
# pocketparse library.
#
# ax forbids global mutable state, and pocketparse inherits the rule: a file
# whose behaviour depends on a process-global written at runtime cannot be
# re-opened cleanly (the classic bug W64 fixed when vmactor's `static int
# defined` survived an engine re-open and silently dropped every later parse off
# the actor path). So NO source file under tools/pocketparse/src may declare a
# `static` variable that is written at runtime: such state belongs on the engine
# (a field of pocketparse_engine) or on the actor's data, not in file storage.
#
# Read-only tables are exempt and stay file-scoped, because they are ROM, not
# mutable state: a `static const` declaration (the embedded grammar registry, the
# manpage bytes, the lookup tables) is fine. So is a `static` FUNCTION — that is
# linkage, not data.
#
# This guard uses ast-grep to find every `static` DECLARATION that is neither
# `const`-qualified nor a function, across src/*.c, and FAILS when it finds any.
# It is the regression tripwire for requirement 1 of Project A; the metric and
# the differential prove behaviour is unchanged, this proves the constraint holds.
#
# Run it from the repo root:
#     bash ci/pocketparse-no-statics.sh
#
# Exit 0 only when zero static mutable variables remain.
set -uo pipefail

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
SRC="$REPO/tools/pocketparse/src"
cd "$REPO" || exit 1

echo "== pocketparse no-statics guard =="

if ! command -v ast-grep >/dev/null 2>&1; then
  echo "FATAL: ast-grep is not installed; cannot run the no-statics guard."
  exit 2
fi

# The rule: a C `declaration` that carries a `static` storage class, is NOT
# const-qualified anywhere in its type, and whose declarator is NOT a function
# declarator (so function prototypes and definitions are excluded). That leaves
# exactly the file-scope and function-local MUTABLE statics we forbid.
RULE="$(mktemp)"
trap 'rm -f "$RULE"' EXIT
cat > "$RULE" <<'YAML'
id: pocketparse-no-static-mutable
language: c
rule:
  kind: declaration
  all:
    - has:
        kind: storage_class_specifier
        regex: "^static$"
    - not:
        has:
          kind: type_qualifier
          regex: "const"
          stopBy: end
    - not:
        has:
          kind: function_declarator
          stopBy: end
YAML

# Scan every translation unit in src/. cli.c is hosted but still part of the
# library tree, so it is held to the same rule.
mapfile -t SOURCES < <(find "$SRC" -maxdepth 1 -name '*.c' | sort)
if [ "${#SOURCES[@]}" -eq 0 ]; then
  echo "FATAL: no source files found under $SRC"
  exit 2
fi

OUTPUT="$(ast-grep scan -r "$RULE" "${SOURCES[@]}" 2>&1)"
COUNT="$(printf '%s\n' "$OUTPUT" | grep -c 'pocketparse-no-static-mutable')"

if [ "$COUNT" -ne 0 ]; then
  echo "FAIL: found $COUNT static mutable variable(s) in tools/pocketparse/src:"
  printf '%s\n' "$OUTPUT"
  echo "STATIC_MUTABLE=$COUNT"
  echo "== no-statics: RED =="
  exit 1
fi

echo "STATIC_MUTABLE=0"
echo "== no-statics: GREEN (zero static mutable variables in src/*.c) =="
exit 0
