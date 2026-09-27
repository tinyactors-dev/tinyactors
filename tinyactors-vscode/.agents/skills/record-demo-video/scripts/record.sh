#!/bin/bash
# Records a demo video of VS Code driving a debug session, on a virtual X
# display: Xvfb, the program to debug, ffmpeg's x11grab, and VS Code running
# a director script in its extension host. See ../SKILL.md.
#
#   scripts/record.sh [output.mp4]
#
# Environment (all optional):
#   DEMO_DIRECTOR   the director directory (default: scripts/director)
#   DEMO_WORKSPACE  the folder VS Code opens (default: tinyactors-napi)
#   DEMO_SERVER     the command that serves the debug session, run in the
#                   workspace (default: napi's tools/dap-server.js on 4711)
#   DEMO_SIZE       the display size (default: 1600x900)
#   VSCODE_VERSION  the VS Code release (default: 1.139.1)
#   DEMO_TRIM       seconds cut from the start, VS Code starting (default: 3)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../../.." && pwd)
EXTENSION=$REPO/tinyactors-vscode
OUT=${1:-$PWD/demo.mp4}
DIRECTOR=${DEMO_DIRECTOR:-$HERE/director}
WORKSPACE=${DEMO_WORKSPACE:-$REPO/tinyactors-napi}
SERVER=${DEMO_SERVER:-"mise exec -- node tools/dap-server.js --port 4711"}
SIZE=${DEMO_SIZE:-1600x900}
VERSION=${VSCODE_VERSION:-1.139.1}
TRIM=${DEMO_TRIM:-3}
export PATH=$HOME/.local/bin:$PATH LANG=C.UTF-8 LC_ALL=C.UTF-8

for tool in Xvfb ffmpeg xdotool curl; do
  command -v $tool >/dev/null || { echo "record.sh: needs $tool (apt-get install -y xvfb ffmpeg xdotool curl)" >&2; exit 1; }
done

# VS Code, downloaded once into the extension's ignored build/.
CODE=$EXTENSION/build/vscode-$VERSION/VSCode-linux-x64/code
if [ ! -x "$CODE" ]; then
  mkdir -p "$EXTENSION/build/vscode-$VERSION"
  curl -fsSL "https://update.code.visualstudio.com/$VERSION/linux-x64/stable" | tar xz -C "$EXTENSION/build/vscode-$VERSION" || exit 1
fi

# VS Code's IPC socket lives in the user data directory, and a Unix socket
# path may not be longer than 107 bytes: keep this directory's path short.
DATA=$(mktemp -d /tmp/vsd.XXXX)
mkdir -p "$DATA/User" && cp "$HERE/settings.json" "$DATA/User/settings.json"
WORK=$(mktemp -d)

DISPLAY_NUMBER=:77
Xvfb $DISPLAY_NUMBER -screen 0 ${SIZE}x24 -nolisten tcp >/dev/null 2>&1 & XVFB=$!
sleep 1
(cd "$WORKSPACE" && exec $SERVER) 2>"$WORK/server.log" & SERVER_PID=$!
sleep 2

ffmpeg -loglevel error -y -f x11grab -video_size $SIZE -framerate 15 -i $DISPLAY_NUMBER \
  -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p "$WORK/raw.mp4" < /dev/null & FFMPEG=$!

DISPLAY=$DISPLAY_NUMBER DEMO_WIDTH=${SIZE%x*} DEMO_HEIGHT=${SIZE#*x} timeout 600 "$CODE" \
  --no-sandbox --disable-gpu --disable-workspace-trust --skip-welcome --skip-release-notes \
  --user-data-dir "$DATA" --extensions-dir "$DATA/extensions" \
  --extensionDevelopmentPath="$EXTENSION" --extensionTestsPath="$DIRECTOR" \
  "$WORKSPACE" > "$WORK/code.log" 2>&1
STATUS=$?

kill -INT $FFMPEG; wait $FFMPEG 2>/dev/null
kill $SERVER_PID $XVFB 2>/dev/null; wait 2>/dev/null
ffmpeg -loglevel error -y -ss "$TRIM" -i "$WORK/raw.mp4" -c:v libx264 -preset slow -crf 22 -pix_fmt yuv420p \
  -movflags +faststart "$OUT"
rm -rf "$DATA"
echo "record.sh: VS Code exited $STATUS; video $OUT; logs in $WORK (server.log, code.log, raw.mp4)"
[ $STATUS -eq 0 ]
