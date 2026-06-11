#!/usr/bin/env bash
set -Eeuo pipefail

# Quick no-sudo smoke test on loopback: start the server on a high port, run the
# probe and the full client driver, then stop the server. Proves Block 1/2/5
# without needing root or network namespaces.

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-10502}"
DOMAIN="${TASE2_DOMAIN:-TestDomain}"

SRV="$PROJECT/src/tase2_server"
CLI="$PROJECT/src/tase2_client"
PRB="$PROJECT/src/tase2_probe"
for b in "$SRV" "$CLI" "$PRB"; do
  [[ -x "$b" ]] || { echo "[ERR] build first: ./scripts/10_build.sh" >&2; exit 1; }
done

"$SRV" -i 127.0.0.1 -p "$PORT" -d "$DOMAIN" -t 5 &
SRV_PID=$!
trap 'kill $SRV_PID 2>/dev/null || true' EXIT
sleep 1

echo "######## PROBE (read-only object dump) ########"
"$PRB" 127.0.0.1 "$PORT" "$DOMAIN" || true
echo
echo "######## CLIENT DRIVER (Block 1/2/5) ########"
"$CLI" 127.0.0.1 "$PORT" "$DOMAIN" 12 || true
