#!/usr/bin/env bash
set -Eeuo pipefail

# Run the FreeTASE2 Server in the server namespace on 10.20.0.10:102.
PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_NS="${SERVER_NS:-tase2_srv}"
SERVER_IP="${SERVER_IP:-10.20.0.10}"
PORT="${PORT:-102}"
DOMAIN="${TASE2_DOMAIN:-TestDomain}"
BLT_ID="${TASE2_BLT_ID:-TestBilTab}"
INTEGRITY="${TASE2_INTEGRITY:-30}"

BIN="$PROJECT/src/tase2_server"
[[ -x "$BIN" ]] || { echo "[ERR] build first: ./scripts/10_build.sh" >&2; exit 1; }

echo "[tase2] server in ns=$SERVER_NS on $SERVER_IP:$PORT domain=$DOMAIN"
sudo ip netns exec "$SERVER_NS" "$BIN" -i "$SERVER_IP" -p "$PORT" -d "$DOMAIN" -b "$BLT_ID" -t "$INTEGRITY"
