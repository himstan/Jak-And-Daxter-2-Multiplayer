#!/usr/bin/env bash

set -euo pipefail

DEST=${1}
LIBRARY=${2}

STEAMWORKS_NET_TAG="2025.163.0"
BASE_URL="https://raw.githubusercontent.com/rlabrecque/Steamworks.NET/${STEAMWORKS_NET_TAG}/com.rlabrecque.steamworks.net/Plugins"

case "$LIBRARY" in
  steam_api64.dll)
    SOURCE_PATH="steam_api64.dll"
    EXPECTED_BLOB="f0a415440a89163c1d9ef2cf69fde329b73b5dba"
    ;;
  libsteam_api.so)
    SOURCE_PATH="libsteam_api.so"
    EXPECTED_BLOB="ce21a66970fe47fd2b95447ea22cfeabd3d89c1e"
    ;;
  libsteam_api.dylib)
    SOURCE_PATH="steam_api.bundle/Contents/MacOS/libsteam_api.dylib"
    EXPECTED_BLOB="0ad8ffc894d14ae93794b1f062bcf5994b7bd8ab"
    ;;
  *)
    echo "Unknown Steam library: $LIBRARY" >&2
    exit 1
    ;;
esac

mkdir -p "$DEST"
curl -fsSL --retry 3 -o "$DEST/$LIBRARY" "$BASE_URL/$SOURCE_PATH"

ACTUAL_BLOB=$(git hash-object "$DEST/$LIBRARY")
if [ "$ACTUAL_BLOB" != "$EXPECTED_BLOB" ]; then
  echo "Unexpected $LIBRARY contents: $ACTUAL_BLOB (expected $EXPECTED_BLOB)" >&2
  rm -f "$DEST/$LIBRARY"
  exit 1
fi
