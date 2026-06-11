#!/usr/bin/env bash
set -Eeuo pipefail

# One-shot capture: start tcpdump + server in the server namespace, run the
# client in the client namespace, then tear down and leave a pcap.
#
#   ./scripts/32_capture.sh [output.pcap]
#
# Requires the namespace lab to be up (./scripts/20_netns_up.sh).

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-/tmp/tase2_iccp.pcap}"

SERVER_NS="${SERVER_NS:-tase2_srv}"
CLIENT_NS="${CLIENT_NS:-tase2_cli}"
SERVER_IP="${SERVER_IP:-10.20.0.10}"
SERVER_VETH="${SERVER_VETH:-veth-tase2-srv}"
PORT="${PORT:-102}"
DOMAIN="${TASE2_DOMAIN:-TestDomain}"
DURATION="${TASE2_DURATION:-20}"

SRV="$PROJECT/src/tase2_server"
CLI="$PROJECT/src/tase2_client"
for b in "$SRV" "$CLI"; do
  [[ -x "$b" ]] || { echo "[ERR] build first: ./scripts/10_build.sh" >&2; exit 1; }
done

teardown() {
  echo "[tase2] tearing down..."
  sudo pkill -x tase2_server 2>/dev/null || true
  sudo pkill -f "tcpdump.*$SERVER_VETH" 2>/dev/null || true
  sleep 1
  sudo chown "$USER:$USER" "$OUT" 2>/dev/null || true
  chmod 644 "$OUT" 2>/dev/null || true
  if [[ -s "$OUT" ]]; then echo "[OK] capture saved to: $OUT ($(du -h "$OUT" | cut -f1))"
  else echo "[WARN] $OUT is empty - did sudo authentication succeed?"; fi
}
trap teardown EXIT

sudo rm -f "$OUT"
echo "[tase2] capturing on $SERVER_NS/$SERVER_VETH"
sudo ip netns exec "$SERVER_NS" tcpdump -i "$SERVER_VETH" -nn -s 0 -w "$OUT" 'tcp port 102' &
sleep 1
echo "[tase2] starting server $SERVER_IP:$PORT (integrity 5s)"
sudo ip netns exec "$SERVER_NS" "$SRV" -i "$SERVER_IP" -p "$PORT" -d "$DOMAIN" -t 5 &
sleep 1
echo "[tase2] running client for ${DURATION}s"
sudo ip netns exec "$CLIENT_NS" "$CLI" "$SERVER_IP" "$PORT" "$DOMAIN" "$DURATION" || true
echo "[tase2] client finished"
