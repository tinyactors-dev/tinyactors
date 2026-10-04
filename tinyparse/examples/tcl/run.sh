#!/usr/bin/env sh
# run.sh — demo the Tcl-ish grammar on a single command line.
#
#   ./run.sh 'echo {*}"a b"${c}d[expr {1+1}]'
#
# It feeds the argument to the pocketparse CLI on stdin and dumps the tree.
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
pp="$here/../../dist/$sub/pocketparse"
[ -x "$pp" ] || pp=pocketparse

line=${1:-'echo {*}"a b"${c}d[expr {1+1}]'}

printf '%s' "$line" | "$pp" run "$grammar" --input - --dump-tree
