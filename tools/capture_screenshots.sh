#!/usr/bin/env bash
# Produces docs/screenshots/*.png from REAL application state:
# starts the local test server, launches Haul with an isolated config/data folder,
# adds real downloads (one finishes, two are still running, one fails) and saves each page.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-$ROOT/build/linux-debug/src/haul}"
OUT="${2:-$ROOT/docs/screenshots}"
WORK="$(mktemp -d)"
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -rf "$WORK"' EXIT

python3 -I "$ROOT/tests/server/test_server.py" > "$WORK/server.txt" &
SERVER_PID=$!
for _ in $(seq 50); do grep -q PORT "$WORK/server.txt" 2>/dev/null && break; sleep 0.1; done
PORT=$(awk '/PORT/{print $2}' "$WORK/server.txt")
BASE="http://127.0.0.1:$PORT"

export XDG_CONFIG_HOME="$WORK/config" XDG_DATA_HOME="$WORK/data" HOME="$WORK/home"
mkdir -p "$HOME/Downloads"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

"$APP" \
  --add "$BASE/file/4500000?name=ubuntu-notes.pdf" \
  --add "$BASE/file/12?status=404&name=missing.iso" \
  --add "$BASE/file/90000000?rate=1200000&ranges=0&name=lecture-recording.mp4" \
  --add "$BASE/file/250000000?rate=6000000&name=dataset-archive.tar.gz" \
  --screenshots "$OUT" --screenshot-delay 6000
echo "Screenshots written to $OUT"
