#!/usr/bin/env bash
set -Eeuo pipefail

# Run the FreeTASE2 client driver (Block 1/2/5) from the client namespace.
PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT_NS="${CLIENT_NS:-tase2_cli}"
SERVER_IP="${SERVER_IP:-10.20.0.10}"
PORT="${PORT:-102}"
DOMAIN="${TASE2_DOMAIN:-TestDomain}"
DURATION="${TASE2_DURATION:-30}"

BIN="$PROJECT/src/tase2_client"
[[ -x "$BIN" ]] || { echo "[ERR] build first: ./scripts/10_build.sh" >&2; exit 1; }

echo "[tase2] client in ns=$CLIENT_NS -> $SERVER_IP:$PORT for ${DURATION}s"
sudo ip netns exec "$CLIENT_NS" "$BIN" "$SERVER_IP" "$PORT" "$DOMAIN" "$DURATION"
