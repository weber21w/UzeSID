#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DB_SIZE=${UZESID_DB_SIZE:-128M}
exec python3 "$SCRIPT_DIR/build_custom_database.py" \
  "$SCRIPT_DIR/sids" \
  --output "$SCRIPT_DIR/Songlengths_writable.bin" \
  --tail-size "$DB_SIZE" \
  --reserve 0 \
  --prebuilt-header "$SCRIPT_DIR/prebuilt_sids.inc" \
  "$@"
