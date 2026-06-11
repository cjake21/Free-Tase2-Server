#!/usr/bin/env bash
set -Eeuo pipefail

# Install everything needed to build libIEC61850 and the FreeTASE2 Server tools,
# and to run the optional network-namespace capture lab.
sudo apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential cmake make gcc g++ git \
  libmbedtls-dev swig python3-dev python3-venv \
  tcpdump tshark wireshark wireshark-common \
  iproute2 iputils-ping net-tools jq lsof psmisc
