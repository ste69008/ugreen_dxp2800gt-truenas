#!/usr/bin/env bash
#
# preconfigure.sh - Creates the coolercontrold datasets before the first start.
#
#   <pool>/apps-coolercontrold           parent dataset
#   <pool>/apps-coolercontrold/config    -> /etc/coolercontrol (config, calibration, curves)
#
# Migration: if the old plain directory /mnt/<pool>/docker/coolercontrold (previous layout of this
# repo) exists and the new dataset is empty, its content is copied over (the old one is left as is).
#
# Usage (on the NAS, as root):
#   sudo bash preconfigure.sh [--pool <pool>]
#
set -euo pipefail

POOL="slow"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --pool) POOL="$2"; shift 2 ;;
    -h|--help) echo "Usage: sudo bash $0 [--pool <pool>]"; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done
[[ $EUID -eq 0 ]] || { echo "Run as root (sudo bash $0 ...)." >&2; exit 1; }

create_dataset() {
  local name="$1"
  if zfs list -H -o name "$name" >/dev/null 2>&1; then
    echo "[=] dataset ${name} already exists."
  elif [[ -e "/mnt/${name}" ]]; then
    echo "[!] /mnt/${name} exists as a plain directory (not a dataset): left as is."
  else
    echo "[+] creating dataset ${name}"
    midclt call pool.dataset.create "{\"name\": \"${name}\"}" >/dev/null
  fi
}

echo "=== coolercontrold preconfiguration (pool: ${POOL}) ==="
create_dataset "${POOL}/apps-coolercontrold"
create_dataset "${POOL}/apps-coolercontrold/config"

NEW="/mnt/${POOL}/apps-coolercontrold/config"
OLD="/mnt/${POOL}/docker/coolercontrold"
if [[ -d "$OLD" && -z "$(ls -A "$NEW")" ]]; then
  echo "[+] migrating ${OLD} -> ${NEW}"
  cp -a "${OLD}/." "${NEW}/"
fi
echo "=== Done ==="
