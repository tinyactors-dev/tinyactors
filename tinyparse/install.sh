#!/bin/sh
# install.sh — fetch the matching pocketparse release tarball and install it
# into a prefix. Designed to be run from a laptop, including via `curl | sh`.
#
# Usage:
#   install.sh [--prefix DIR] [--version VER] [--repo OWNER/REPO]
#              [--from-file TARBALL]
#
#   --prefix DIR      install root (default: $HOME/.local)
#   --version VER     release version (default: latest GitHub release)
#   --repo OWNER/REPO GitHub repository (default: dhamidi/ax)
#   --from-file FILE  install from a local tarball; skips all network access
#
# The result is a prefix-shaped tree:
#   <prefix>/bin/pocketparse
#   <prefix>/lib/libpocketparse.{a,so,dylib}
#   <prefix>/include/pocketparse.h
#   <prefix>/share/man/man1/pocketparse.1
set -eu

PREFIX="${HOME:-}/.local"
REPO="dhamidi/ax"
VERSION=""
FROM_FILE=""

PROG="pocketparse"

die() { printf '[pp-install] ERROR: %s\n' "$*" >&2; exit 1; }
log() { printf '[pp-install] %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# Parse arguments.
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)    [ $# -ge 2 ] || die "--prefix needs an argument"; PREFIX="$2"; shift 2 ;;
    --prefix=*)  PREFIX="${1#--prefix=}"; shift ;;
    --version)   [ $# -ge 2 ] || die "--version needs an argument"; VERSION="$2"; shift 2 ;;
    --version=*) VERSION="${1#--version=}"; shift ;;
    --repo)      [ $# -ge 2 ] || die "--repo needs an argument"; REPO="$2"; shift 2 ;;
    --repo=*)    REPO="${1#--repo=}"; shift ;;
    --from-file) [ $# -ge 2 ] || die "--from-file needs an argument"; FROM_FILE="$2"; shift 2 ;;
    --from-file=*) FROM_FILE="${1#--from-file=}"; shift ;;
    -h|--help)
      sed -n '2,17p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done

[ -n "$PREFIX" ] || die "--prefix must not be empty"

# ---------------------------------------------------------------------------
# detect_platform: map uname output to a release platform string.
# ---------------------------------------------------------------------------
detect_platform() {
  os="$(uname -s)"
  arch="$(uname -m)"
  case "$os/$arch" in
    Linux/x86_64)                  echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64)   echo macos-arm64 ;;
    *) die "unsupported platform: $os/$arch (supported: Linux/x86_64, Darwin/arm64)" ;;
  esac
}

# ---------------------------------------------------------------------------
# have: is a command available?
# ---------------------------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# fetch URL OUTFILE: download a URL with curl or wget.
# ---------------------------------------------------------------------------
fetch() {
  url="$1"; out="$2"
  if have curl; then
    curl -fsSL -o "$out" "$url"
  elif have wget; then
    wget -qO "$out" "$url"
  else
    die "need curl or wget to download $url"
  fi
}

# ---------------------------------------------------------------------------
# fetch_stdout URL: print a URL's body to stdout (curl or wget).
# ---------------------------------------------------------------------------
fetch_stdout() {
  url="$1"
  if have curl; then
    curl -fsSL "$url"
  elif have wget; then
    wget -qO - "$url"
  else
    die "need curl or wget to query $url"
  fi
}

# ---------------------------------------------------------------------------
# resolve_latest_version REPO: query the GitHub API for the latest release tag
# and strip a leading `pocketparse-v` to get the bare version.
# ---------------------------------------------------------------------------
resolve_latest_version() {
  repo="$1"
  api="https://api.github.com/repos/$repo/releases/latest"
  body="$(fetch_stdout "$api")" || die "could not query latest release from $api"
  tag="$(printf '%s\n' "$body" \
    | grep '"tag_name"' \
    | head -n 1 \
    | sed -e 's/.*"tag_name"[[:space:]]*:[[:space:]]*"//' -e 's/".*//')"
  [ -n "$tag" ] || die "could not read tag_name from $api response"
  # Strip a leading pocketparse-v (or a bare leading v) to get the version.
  ver="${tag#pocketparse-v}"
  ver="${ver#v}"
  [ -n "$ver" ] || die "could not derive a version from tag '$tag'"
  echo "$ver"
}

# ---------------------------------------------------------------------------
# Temp dir, cleaned up on exit.
# ---------------------------------------------------------------------------
TMPDIR_INSTALL=""
cleanup() { [ -n "$TMPDIR_INSTALL" ] && rm -rf "$TMPDIR_INSTALL"; }
trap cleanup EXIT INT HUP TERM

have tar || die "tar is required but was not found"
TMPDIR_INSTALL="$(mktemp -d)" || die "could not create a temp directory"

# ---------------------------------------------------------------------------
# Obtain the tarball: either a local file (offline) or a GitHub download.
# ---------------------------------------------------------------------------
TARBALL=""
if [ -n "$FROM_FILE" ]; then
  [ -f "$FROM_FILE" ] || die "no such file: $FROM_FILE"
  TARBALL="$FROM_FILE"
  log "installing from local tarball: $TARBALL"
else
  PLAT="$(detect_platform)"
  log "detected platform: $PLAT"

  if [ -z "$VERSION" ]; then
    log "resolving latest release version from $REPO ..."
    VERSION="$(resolve_latest_version "$REPO")"
  fi
  log "version: $VERSION"

  ASSET="pocketparse-$VERSION-$PLAT.tar.gz"
  URL="https://github.com/$REPO/releases/download/pocketparse-v$VERSION/$ASSET"
  TARBALL="$TMPDIR_INSTALL/$ASSET"
  log "downloading $URL"
  fetch "$URL" "$TARBALL" \
    || die "download failed (is the release published?): $URL"
  [ -s "$TARBALL" ] || die "downloaded an empty file from $URL"
fi

# ---------------------------------------------------------------------------
# Extract and install.
# ---------------------------------------------------------------------------
EXTRACT="$TMPDIR_INSTALL/extract"
mkdir -p "$EXTRACT"
tar xzf "$TARBALL" -C "$EXTRACT" || die "could not extract $TARBALL"

# The tarball unpacks to a single top-level pocketparse-<ver>-<plat>/ directory.
TOPDIR=""
for d in "$EXTRACT"/*/; do
  [ -d "$d" ] || continue
  if [ -n "$TOPDIR" ]; then
    die "unexpected tarball layout: more than one top-level directory"
  fi
  TOPDIR="$d"
done
[ -n "$TOPDIR" ] || die "unexpected tarball layout: no top-level directory"
[ -d "$TOPDIR/bin" ] || die "tarball has no bin/ directory; is this a pocketparse release?"

mkdir -p "$PREFIX" || die "could not create prefix: $PREFIX"

# Copy the contents (bin/ lib/ include/ share/) into the prefix.
for sub in bin lib include share; do
  [ -d "$TOPDIR/$sub" ] || continue
  cp -R "$TOPDIR/$sub" "$PREFIX/" || die "failed to copy $sub into $PREFIX"
done

[ -x "$PREFIX/bin/pocketparse" ] || die "install failed: $PREFIX/bin/pocketparse is missing"

# ---------------------------------------------------------------------------
# Summary and PATH hint.
# ---------------------------------------------------------------------------
log "installed pocketparse into $PREFIX"
log "  $PREFIX/bin/pocketparse"
[ -f "$PREFIX/include/pocketparse.h" ] && log "  $PREFIX/include/pocketparse.h"
for lib in "$PREFIX"/lib/libpocketparse.*; do
  [ -e "$lib" ] && log "  $lib"
done
[ -f "$PREFIX/share/man/man1/pocketparse.1" ] && log "  $PREFIX/share/man/man1/pocketparse.1"

case ":${PATH:-}:" in
  *":$PREFIX/bin:"*) : ;;
  *) log "hint: add $PREFIX/bin to your PATH, e.g.:"
     log "      export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac

log "done. Try: $PREFIX/bin/pocketparse version"
