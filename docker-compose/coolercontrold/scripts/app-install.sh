#!/usr/bin/env bash
#
# app-install.sh - Creates (or updates) the TrueNAS Custom App "coolercontrold" as an include of
# the compose file deployed next to this script's parent directory.
#
#   include:
#     - /mnt/<pool>/docker-compose/coolercontrold/docker-compose.yml
#   services: {}
#
# Usage (on the NAS, as root, after preconfigure.sh and with .env in place):
#   sudo bash /mnt/<pool>/docker-compose/coolercontrold/scripts/app-install.sh
#
# Converting an existing app installed from a pasted YAML: it is updated in place (same app name),
# the data directory being a bind mount outside the app, nothing is lost.
#
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run as root (sudo bash $0)." >&2; exit 1; }
STACK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE="${STACK_DIR}/docker-compose.yml"
APP="coolercontrold"
[[ -f "$COMPOSE" ]] || { echo "${COMPOSE} not found." >&2; exit 1; }
[[ -f "${STACK_DIR}/.env" ]] || { echo "${STACK_DIR}/.env not found (copy .env.example)." >&2; exit 1; }

YAML="$(printf 'include:\n  - %s\nservices: {}\n' "$COMPOSE")"

if midclt call app.query "[[\"name\",\"=\",\"${APP}\"]]" | grep -q '"name"'; then
  echo "[=] app ${APP} exists: updating its YAML to the include"
  midclt call -j app.update "$APP" "$(jq -cn --arg y "$YAML" '{custom_compose_config_string: $y}')" >/dev/null
else
  echo "[+] creating app ${APP}"
  midclt call -j app.create "$(jq -cn --arg n "$APP" --arg y "$YAML" '{app_name: $n, custom_app: true, custom_compose_config_string: $y}')" >/dev/null
fi
midclt call app.query "[[\"name\",\"=\",\"${APP}\"]]" \
  | jq -r '.[0] | "state: \(.state)", (.active_workloads.container_details[]? | "  \(.service_name): \(.state)")'
