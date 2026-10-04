#!/usr/bin/env bash
# scripts/pocketparse-setup.sh — provision pocketparse's Zig 0.15.1 toolchain.
#
# pocketparse pins Zig 0.15.1 via tools/pocketparse/mise.toml, independent of
# the repo's 0.16 freestanding toolchain. This SessionStart helper makes that
# compiler reachable in the web sandbox. It is:
#   - idempotent     (safe to re-run; skips work already done)
#   - fault-tolerant (no network => warn and exit 0, never wedge the session)
#   - asynchronous-friendly (does the slow fetch only when needed)
#
# Resolution order build.sh uses: $POCKETPARSE_ZIG, then mise, then a local
# .toolchain/zig, then any `zig` on PATH, then the repo's freestanding zig.
set -uo pipefail

ROOT="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP="$ROOT/tools/pocketparse"
ZIG_VERSION="${POCKETPARSE_ZIG_VERSION:-0.15.1}"
TC="$PP/.toolchain"

log() { printf '[pocketparse-setup] %s\n' "$*"; }

# ast-grep backs the no-statics correctness gate (ci/pocketparse-no-statics.sh,
# run by the metric). When it is absent the gate SKIPS rather than fails, which
# once let a `static` mutable slip into the engine unnoticed. Provision it here
# so the pocketparse gate is self-sufficient and never silently skipped. It is
# idempotent (skips when already present) and never fails the session.
ensure_ast_grep() {
  command -v ast-grep >/dev/null 2>&1 && { log "ok  ast-grep $(ast-grep --version 2>/dev/null)"; return 0; }
  if command -v npm >/dev/null 2>&1; then
    if npm install -g @ast-grep/cli >/dev/null 2>&1 && command -v ast-grep >/dev/null 2>&1; then
      log "ok  installed ast-grep $(ast-grep --version 2>/dev/null) via npm"
      return 0
    fi
  fi
  log "warn ast-grep not available and could not be installed; the no-statics"
  log "     gate will be skipped until it is present (no-statics correctness un-enforced)."
  return 0
}

# Only provision automatically in the web sandbox (or when forced); a dev box
# uses its own mise. Never fail the session here.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ] && [ "${POCKETPARSE_SETUP:-}" != "1" ]; then
  log "not the web sandbox; skipping (set POCKETPARSE_SETUP=1 to force)."
  exit 0
fi

# Always ensure ast-grep first — it must run regardless of which zig path the
# rest of this script exits through (the zig provisioning below has several
# early `exit 0`s).
ensure_ast_grep

# Already have a 0.15.x zig somewhere build.sh will find? Then we are done.
if [ -x "$TC/zig/zig" ] && "$TC/zig/zig" version 2>/dev/null | grep -q '^0\.15\.'; then
  log "ok  zig $("$TC/zig/zig" version) at $TC/zig/zig"
  exit 0
fi
if command -v mise >/dev/null 2>&1; then
  if ( cd "$PP" && mise install ) >/dev/null 2>&1; then
    log "ok  mise install provisioned zig 0.15.1"
    exit 0
  fi
  log "warn mise present but 'mise install' did not complete; falling back to direct fetch."
fi

# Direct fetch fallback: pull the pinned tarball from ziglang.org.
arch="$(uname -m)"; os="$(uname -s | tr 'A-Z' 'a-z')"
case "$arch" in aarch64|arm64) arch=aarch64 ;; x86_64|amd64) arch=x86_64 ;; esac
url="https://ziglang.org/download/${ZIG_VERSION}/zig-${arch}-${os}-${ZIG_VERSION}.tar.xz"
# 0.15 used a different naming on some platforms; try the legacy form too.
url_legacy="https://ziglang.org/download/${ZIG_VERSION}/zig-${os}-${arch}-${ZIG_VERSION}.tar.xz"

mkdir -p "$TC"
tmp="$(mktemp -d)"
fetch() { command -v curl >/dev/null 2>&1 && curl -fsSL "$1" -o "$2" || wget -qO "$2" "$1"; }

if fetch "$url" "$tmp/zig.tar.xz" || fetch "$url_legacy" "$tmp/zig.tar.xz"; then
  if tar -xJf "$tmp/zig.tar.xz" -C "$tmp" 2>/dev/null; then
    d="$(find "$tmp" -maxdepth 1 -type d -name 'zig-*' | head -1)"
    if [ -n "$d" ] && [ -x "$d/zig" ]; then
      rm -rf "$TC/zig"; mv "$d" "$TC/zig"
      log "ok  fetched zig $("$TC/zig/zig" version) -> $TC/zig/zig"
      rm -rf "$tmp"; exit 0
    fi
  fi
fi

rm -rf "$tmp"
log "warn could not provision zig $ZIG_VERSION (no network?). The metric will run;"
log "     pocketparse build steps will be skipped until a compiler is available."
exit 0
