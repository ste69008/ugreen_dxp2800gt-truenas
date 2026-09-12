#!/bin/bash
# Backs up the coolercontrold data directory (config, calibrations,
# password/session files) to a timestamped zip archive. Run manually,
# not scheduled.

set -euo pipefail

DEST_DIR="/mnt/slow/backup"
SOURCE_DIR="/mnt/slow/docker/coolercontrold"

if [ ! -d "$SOURCE_DIR" ]; then
    echo "ERROR: source directory not found: $SOURCE_DIR" >&2
    exit 1
fi

mkdir -p "$DEST_DIR"

TIMESTAMP=$(date +%Y%m%d-%H%M%S)
ARCHIVE_PATH="$DEST_DIR/coolercontrold-${TIMESTAMP}.zip"

7z a -tzip "$ARCHIVE_PATH" "$SOURCE_DIR" > /dev/null

# The archive contains .passwd/.session_key, so keep it root-only readable.
chmod 600 "$ARCHIVE_PATH"

echo "Backup created: $ARCHIVE_PATH"
