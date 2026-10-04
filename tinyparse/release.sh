#!/usr/bin/env bash
# release.sh — assemble the pocketparse release artifacts into dist/release/.
#
# It produces the three deliverables described in docs/distribution.md:
#
#   pocketparse-$V-linux-x86_64.tar.gz   prefix tree (bin/lib/include/share)
#   pocketparse-$V-macos-arm64.tar.gz    prefix tree (bin/lib/include/share)
#   pocketparse-$V-amalgamation.tar.gz   the single-file distribution
#
# Subcommands:
#   tarballs   build the dist matrix, then stage and tar all artifacts (default)
#   all        alias for tarballs
#
# The release version is read from VERSION; nothing is hard-coded. dist/ is
# gitignored, so everything this script writes is a build artifact.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DIST="$HERE/dist"
RELEASE="$DIST/release"

V="$(cat "$HERE/VERSION")"

log()  { printf '[pp-release] %s\n' "$*" >&2; }
warn() { printf '[pp-release] WARNING: %s\n' "$*" >&2; }
die()  { printf '[pp-release] ERROR: %s\n' "$*" >&2; exit 1; }

shared_ext() { case "$1" in macos-arm64) echo dylib ;; *) echo so ;; esac; }

# ---------------------------------------------------------------------------
# stage_platform: build one prefix tree under a temp staging dir and tar it.
# Missing artifacts are warned about but do not abort the run; a platform with
# no CLI at all is skipped entirely (warned).
# ---------------------------------------------------------------------------
stage_platform() {
  local plat="$1"
  local ext; ext="$(shared_ext "$plat")"
  local src="$DIST/$plat"
  local name="pocketparse-$V-$plat"

  if [ ! -f "$src/pocketparse" ]; then
    warn "no CLI for $plat at $src/pocketparse; skipping its tarball"
    return 0
  fi

  local stage; stage="$(mktemp -d)"
  local root="$stage/$name"
  mkdir -p "$root/bin" "$root/lib" "$root/include" "$root/share/man/man1"

  install -m 0755 "$src/pocketparse" "$root/bin/pocketparse"

  if [ -f "$src/libpocketparse.a" ]; then
    install -m 0644 "$src/libpocketparse.a" "$root/lib/libpocketparse.a"
  else
    warn "$plat: libpocketparse.a missing"
  fi
  if [ -f "$src/libpocketparse.$ext" ]; then
    install -m 0644 "$src/libpocketparse.$ext" "$root/lib/libpocketparse.$ext"
  else
    warn "$plat: libpocketparse.$ext missing"
  fi
  if [ -f "$src/pocketparse.h" ]; then
    install -m 0644 "$src/pocketparse.h" "$root/include/pocketparse.h"
  else
    warn "$plat: pocketparse.h missing"
  fi
  if [ -f "$HERE/pocketparse.1" ]; then
    install -m 0644 "$HERE/pocketparse.1" "$root/share/man/man1/pocketparse.1"
  else
    warn "$plat: pocketparse.1 missing"
  fi

  write_install_note "$root/INSTALL.md" "$plat"

  ( cd "$stage" && tar czf "$RELEASE/$name.tar.gz" "$name" ) \
    || die "$plat: tar failed"
  rm -rf "$stage"
  log "wrote $RELEASE/$name.tar.gz"
}

# ---------------------------------------------------------------------------
# write_install_note: a short INSTALL.md for a platform tarball. Uses the
# template at INSTALL.md if present, expanding @VERSION@/@PLATFORM@; otherwise
# emits a minimal built-in note.
# ---------------------------------------------------------------------------
write_install_note() {
  local out="$1" plat="$2"
  if [ -f "$HERE/INSTALL.md" ]; then
    sed -e "s/@VERSION@/$V/g" -e "s/@PLATFORM@/$plat/g" "$HERE/INSTALL.md" > "$out"
  else
    cat > "$out" <<EOF
# pocketparse $V — $plat

Install into a prefix by copying the tree:

    sudo cp -r * /usr/local/

Or into your home prefix:

    cp -r * ~/.local/

The tree is prefix-shaped: bin/, lib/, include/, share/man/man1/.
EOF
  fi
}

# ---------------------------------------------------------------------------
# stage_amalgamation: build the single-file distribution and tar it.
# ---------------------------------------------------------------------------
stage_amalgamation() {
  local name="pocketparse-$V-amalgamation"
  local amalg="$DIST/amalgamation"

  bash "$HERE/amalgamate.sh" || die "amalgamate.sh failed"

  local f
  for f in pocketparse.c pocketparse.h pocketparse_cli.c README.md; do
    [ -f "$amalg/$f" ] || die "amalgamation missing $f"
  done

  local stage; stage="$(mktemp -d)"
  local root="$stage/$name"
  mkdir -p "$root"
  install -m 0644 "$amalg/pocketparse.c"     "$root/pocketparse.c"
  install -m 0644 "$amalg/pocketparse.h"     "$root/pocketparse.h"
  install -m 0644 "$amalg/pocketparse_cli.c" "$root/pocketparse_cli.c"
  install -m 0644 "$amalg/README.md"         "$root/README.md"

  ( cd "$stage" && tar czf "$RELEASE/$name.tar.gz" "$name" ) \
    || die "amalgamation: tar failed"
  rm -rf "$stage"
  log "wrote $RELEASE/$name.tar.gz"
}

cmd_tarballs() {
  mkdir -p "$RELEASE"

  # Ensure the dist matrix (and pocketparse.1) is built.
  log "building dist matrix via build.sh dist"
  ( cd "$HERE" && bash build.sh dist ) 2>&1 | sed 's/^/  /'

  stage_platform linux-x86_64
  stage_platform macos-arm64
  stage_amalgamation

  log "release artifacts in $RELEASE:"
  ls -la "$RELEASE" >&2 || true
}

cmd="${1:-tarballs}"
case "$cmd" in
  tarballs|all) cmd_tarballs ;;
  *) die "unknown subcommand: $cmd (try: tarballs)" ;;
esac
