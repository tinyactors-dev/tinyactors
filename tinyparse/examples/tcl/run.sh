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

# Locate the built CLI. Prefer the dist matrix; fall back to PATH.
plat=$(uname -s)/$(uname -m)
case "$plat" in
  Linux/x86_64)            sub=linux-x86_64 ;;
  Darwin/arm64|Darwin/aarch64) sub=macos-arm64 ;;
  *)                       sub=linux-x86_64 ;;
esac
tp="$here/../../dist/$sub/tinyparse"
[ -x "$tp" ] || tp=tinyparse

line=${1:-'echo {*}"a b"${c}d[expr {1+1}]'}

printf '%s' "$line" | "$tp" run "$grammar" --input - --dump-tree
