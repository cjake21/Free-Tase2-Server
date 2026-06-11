#!/usr/bin/env bash
set -Eeuo pipefail

# Create two network namespaces (server + client) joined by a veth pair, so the
# FreeTASE2 Server and a client can talk over TCP/102 on an isolated link that
# tcpdump can capture.

SERVER_NS="${SERVER_NS:-tase2_srv}"
CLIENT_NS="${CLIENT_NS:-tase2_cli}"
SERVER_IP="${SERVER_IP:-10.20.0.10}"
CLIENT_IP="${CLIENT_IP:-10.20.0.20}"
SERVER_VETH="${SERVER_VETH:-veth-tase2-srv}"
CLIENT_VETH="${CLIENT_VETH:-veth-tase2-cli}"

sudo ip netns delete "$SERVER_NS" 2>/dev/null || true
sudo ip netns delete "$CLIENT_NS" 2>/dev/null || true
sudo ip link delete "$SERVER_VETH" 2>/dev/null || true

sudo ip netns add "$SERVER_NS"
sudo ip netns add "$CLIENT_NS"
sudo ip link add "$SERVER_VETH" type veth peer name "$CLIENT_VETH"
sudo ip link set "$SERVER_VETH" netns "$SERVER_NS"
sudo ip link set "$CLIENT_VETH" netns "$CLIENT_NS"
sudo ip netns exec "$SERVER_NS" ip addr add "$SERVER_IP/24" dev "$SERVER_VETH"
sudo ip netns exec "$CLIENT_NS" ip addr add "$CLIENT_IP/24" dev "$CLIENT_VETH"
sudo ip netns exec "$SERVER_NS" ip link set lo up
sudo ip netns exec "$CLIENT_NS" ip link set lo up
sudo ip netns exec "$SERVER_NS" ip link set "$SERVER_VETH" up
sudo ip netns exec "$CLIENT_NS" ip link set "$CLIENT_VETH" up

sudo ip netns exec "$CLIENT_NS" ping -c 2 "$SERVER_IP"
echo "[OK] namespace lab up: $SERVER_NS($SERVER_IP) <-> $CLIENT_NS($CLIENT_IP)"
