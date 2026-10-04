#!/usr/bin/env sh
# run.sh — demo the Tcl-ish grammar on a single command line.
#
#   ./run.sh 'echo {*}"a b"${c}d[expr {1+1}]'
#
# It feeds the argument to the tinyparse CLI on stdin and dumps the tree.
# This is a convenience for humans; the test suite drives the CLI directly.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
grammar="$here/tcl.scm"

# The CLI `mise run cli` builds; $TP when the test suite runs it; else PATH.
tp="${TP:-$here/../../build/bin/tinyparse}"
[ -x "$tp" ] || tp=tinyparse

line=${1:-'echo {*}"a b"${c}d[expr {1+1}]'}

printf '%s' "$line" | "$tp" run "$grammar" --input - --dump-tree
