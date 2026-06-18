#!/usr/bin/env bash
set -Eeuo pipefail

# Run the SCADA HMI on loopback: start the TASE.2 server on a high port and the
# HMI bridge in front of it, then open http://127.0.0.1:8800. No sudo needed.
#
# The bridge drives two real ICCP clients against the server, so everything you
# do in the HMI is genuine TASE.2/MMS traffic. To capture it, point a capture at
# the loopback TCP port below, or use the namespace scripts (20/30/32) and run
# the bridge with TASE2_HOST/TASE2_PORT pointing at the server namespace.

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TASE2_PORT="${TASE2_PORT:-10502}"
TASE2_HOST="${TASE2_HOST:-127.0.0.1}"
DOMAIN="${TASE2_DOMAIN:-TestDomain}"
HTTP_PORT="${HTTP_PORT:-8800}"
INJECT_HOLD="${INJECT_HOLD:-30}"

SRV="$PROJECT/src/tase2_server"
AGENT="$PROJECT/src/tase2_hmi_agent"
for b in "$SRV" "$AGENT"; do
  [[ -x "$b" ]] || { echo "[ERR] build first: ./scripts/10_build.sh (then make tase2_hmi_agent)" >&2; exit 1; }
done

# Start the server only if nothing is already serving the chosen port.
SRV_PID=""
if ! ss -ltn "( sport = :$TASE2_PORT )" 2>/dev/null | grep -q ":$TASE2_PORT"; then
  echo "[hmi] starting TASE.2 server on $TASE2_HOST:$TASE2_PORT"
  "$SRV" -i "$TASE2_HOST" -p "$TASE2_PORT" -d "$DOMAIN" -t 30 -o "$INJECT_HOLD" &
  SRV_PID=$!
  sleep 1
else
  echo "[hmi] reusing TASE.2 server already on :$TASE2_PORT"
fi

cleanup() { [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

TASE2_HOST="$TASE2_HOST" TASE2_PORT="$TASE2_PORT" TASE2_DOMAIN="$DOMAIN" \
  python3 "$PROJECT/hmi/bridge.py" \
    --server-host "$TASE2_HOST" --server-port "$TASE2_PORT" --domain "$DOMAIN" \
    --http-port "$HTTP_PORT"
