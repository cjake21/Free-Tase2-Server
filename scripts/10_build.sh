#!/usr/bin/env bash
set -Eeuo pipefail

# Clone + patch + build libIEC61850, then compile the FreeTASE2 Server tools.
#
# libIEC61850 has no TASE.2 server. We reuse its MMS engine, but it needs two
# non-default changes that this script applies automatically:
#   1. CONFIG_MMS_SUPPORT_VMD_SCOPE_NAMED_VARIABLES=1  (TASE.2 reads
#      TASE2_Version / Supported_Features at VMD scope; off by default).
#   2. A one-line fix to the VMD-scope read path in mms_read_service.c, which is
#      never compiled by default and otherwise fails to build once (1) is on.
# Optionally it also adds FreeTase2 client helper wrappers to the pyiec61850
# Python binding.

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="${DEPS_DIR:-$PROJECT/deps}"
LIB="$DEPS_DIR/libiec61850"

mkdir -p "$DEPS_DIR"

if [[ ! -d "$LIB" ]]; then
  git clone https://github.com/mz-automation/libiec61850.git "$LIB"
else
  git -C "$LIB" pull --ff-only || true
fi

# --- patch 1: enable VMD-scope named variables ---
CMAKE_CFG="$LIB/config/stack_config.h.cmake"
if grep -q '#define CONFIG_MMS_SUPPORT_VMD_SCOPE_NAMED_VARIABLES 0' "$CMAKE_CFG"; then
  echo "[patch] enabling CONFIG_MMS_SUPPORT_VMD_SCOPE_NAMED_VARIABLES"
  sed -i 's/#define CONFIG_MMS_SUPPORT_VMD_SCOPE_NAMED_VARIABLES 0/#define CONFIG_MMS_SUPPORT_VMD_SCOPE_NAMED_VARIABLES 1/' "$CMAKE_CFG"
fi

# --- patch 2: fix VMD-scope read path (missing 7th argument) ---
READSVC="$LIB/src/mms/iso_mms/server/mms_read_service.c"
if grep -Pzoq 'MmsServer_getDevice\(connection->server\), nameIdStr,\s*\n\s*values, connection, alternateAccess\);' "$READSVC"; then
  echo "[patch] fixing addNamedVariableToResultList VMD-scope call"
  perl -0pi -e 's/(MmsServer_getDevice\(connection->server\), nameIdStr,\s*\n\s*values, connection, alternateAccess)\)/$1, variableCount == 1)/' "$READSVC"
fi

# --- patch 3 (optional): FreeTase2 client helper wrappers for pyiec61850 ---
SWIG_IF="$LIB/pyiec61850/iec61850.i"
WRAP="$PROJECT/bindings/pyiec61850_tase2_wrappers.i"
if [[ -f "$SWIG_IF" && -f "$WRAP" ]] && ! grep -q 'pyiec61850_tase2_wrappers.i' "$SWIG_IF"; then
  echo "[patch] adding FreeTase2 helper wrappers to pyiec61850 binding"
  printf '\n%%include "%s"\n' "$WRAP" >> "$SWIG_IF"
fi

# --- build libIEC61850 (Python bindings optional but cheap) ---
cd "$LIB"
cmake -S . -B build -DBUILD_PYTHON_BINDINGS=ON
cmake --build build -j"$(nproc)"

# --- build FreeTASE2 Server tools ---
make -C "$PROJECT/src" LIB61850_HOME="$LIB"

echo
echo "[OK] Built:"
ls -l "$PROJECT/src/tase2_server" "$PROJECT/src/tase2_client" "$PROJECT/src/tase2_probe"
