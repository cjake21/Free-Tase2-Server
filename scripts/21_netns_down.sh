#!/usr/bin/env bash
set -Eeuo pipefail

SERVER_NS="${SERVER_NS:-tase2_srv}"
CLIENT_NS="${CLIENT_NS:-tase2_cli}"
SERVER_VETH="${SERVER_VETH:-veth-tase2-srv}"

sudo ip netns delete "$SERVER_NS" 2>/dev/null || true
sudo ip netns delete "$CLIENT_NS" 2>/dev/null || true
sudo ip link delete "$SERVER_VETH" 2>/dev/null || true
echo "[OK] namespace lab torn down."
