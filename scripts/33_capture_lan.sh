#!/usr/bin/env bash
set -Eeuo pipefail

# Capture TASE.2/ICCP traffic from REAL equipment acting as the client.
#
# Unlike 32_capture.sh (which runs in an isolated network namespace and drives
# the bundled client), this binds the server to a real NIC so physical lab gear
# can associate to it, and captures on that same NIC. There is no local client:
# YOUR equipment is the TASE.2 client. Runs until Ctrl-C.
#
#   sudo ./scripts/33_capture_lan.sh [output.pcap]
#
# Override via env:
#   IFACE     capture interface          (default: enp0s8, the lab NIC)
#   BIND_IP   server bind address        (default: 0.0.0.0, all interfaces)
#   PORT      listen port                (default: 102, the ICCP standard)
#   TASE2_DOMAIN / TASE2_BLT_ID / TASE2_INTEGRITY  as in 30_run_server.sh

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-/tmp/tase2_lab.pcap}"

IFACE="${IFACE:-enp0s8}"
BIND_IP="${BIND_IP:-0.0.0.0}"
PORT="${PORT:-102}"
DOMAIN="${TASE2_DOMAIN:-TestDomain}"
BLT_ID="${TASE2_BLT_ID:-TestBilTab}"
INTEGRITY="${TASE2_INTEGRITY:-30}"

SRV="$PROJECT/src/tase2_server"
[[ -x "$SRV" ]] || { echo "[ERR] build first: ./scripts/10_build.sh" >&2; exit 1; }
ip link show "$IFACE" >/dev/null 2>&1 || { echo "[ERR] no such interface: $IFACE" >&2; exit 1; }

teardown() {
  echo
  echo "[tase2] tearing down..."
  sudo pkill -x tase2_server 2>/dev/null || true
  sudo pkill -f "tcpdump.*-i $IFACE" 2>/dev/null || true
  sleep 1
  sudo chown "$USER:$USER" "$OUT" 2>/dev/null || true
  chmod 644 "$OUT" 2>/dev/null || true
  if [[ -s "$OUT" ]]; then echo "[OK] capture saved to: $OUT ($(du -h "$OUT" | cut -f1))"
  else echo "[WARN] $OUT is empty - did any equipment actually connect on $IFACE:$PORT?"; fi
}
trap teardown EXIT INT TERM

sudo rm -f "$OUT"
echo "[tase2] capturing on $IFACE -> $OUT  (filter: tcp port $PORT)"
sudo tcpdump -i "$IFACE" -nn -s 0 -w "$OUT" "tcp port $PORT" &
sleep 1
echo "[tase2] server listening on $BIND_IP:$PORT domain=$DOMAIN blt=$BLT_ID integrity=${INTEGRITY}s"
echo "[tase2] point your lab equipment (TASE.2 client) at this host, then Ctrl-C to stop."
sudo "$SRV" -i "$BIND_IP" -p "$PORT" -d "$DOMAIN" -b "$BLT_ID" -t "$INTEGRITY"
