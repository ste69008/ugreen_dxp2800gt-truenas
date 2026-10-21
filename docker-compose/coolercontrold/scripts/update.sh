#!/usr/bin/env bash
#
# update.sh - Updates the coolercontrold image on the NAS, with automatic rollback.
#
# Run FROM THE WORKSTATION (not on the NAS), in this repo:
#   docker-compose/coolercontrold/scripts/update.sh <nas-ssh-host> <version>
#   e.g. docker-compose/coolercontrold/scripts/update.sh truenas 5.1.2
#
# Steps:
#   1. checks the tag exists on Docker Hub, reads the version running on the NAS
#   2. recursive ZFS snapshot <pool>/apps-coolercontrold@before-<version> (coolercontrold migrates
#      its config on upgrade: an older version may not read it back)
#   3. pulls the new image BEFORE stopping anything (short downtime; the old image stays for rollback)
#   4. bumps COOLERCONTROL_TAG's default in this repo's docker-compose.yml, copies it to the NAS
#      (previous one kept as docker-compose.yml.bak), redeploys the TrueNAS app
#   5. waits for GET /handshake = 200 (what the fan watchdog checks), up to 2 minutes
#   6. on failure: previous compose file back, app stopped, snapshot rolled back, app started again
#      (meanwhile the watchdog holds the fans at 60 %); the repo file is restored too
#
# Requirements: SSH key + sudo on the NAS, the stack deployed as in INSTALL.md (step 4), and no
# COOLERCONTROL_TAG in the NAS .env (it would override the compose default: the script stops).
# Afterwards: commit docker-compose.yml; delete the snapshot once the new version is trusted.
#
set -euo pipefail

NAS="${1:-}"; VERSION="${2:-}"
[[ -n "$NAS" && -n "$VERSION" ]] || { sed -n '6,8p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Version must look like 5.1.2 (got: ${VERSION})." >&2; exit 1; }

STACK_LOCAL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_LOCAL="${STACK_LOCAL}/docker-compose.yml"
STACK_DIR="${STACK_DIR:-/mnt/slow/docker-compose/coolercontrold}" # stack directory on the NAS
APP="coolercontrold"
IMAGE="coolercontrol/coolercontrold"
remote() { ssh -n "$NAS" "$@"; }

echo "=== coolercontrold -> ${VERSION} on ${NAS} ==="

# --- 1. Checks ---
code="$(curl -s -m 10 -o /dev/null -w '%{http_code}' "https://hub.docker.com/v2/repositories/${IMAGE}/tags/${VERSION}" || true)"
case "$code" in
  200) echo "[=] tag ${VERSION} found on Docker Hub" ;;
  404) echo "[!] tag ${VERSION} does not exist on Docker Hub." >&2; exit 1 ;;
  *)   echo "[?] Docker Hub unreachable (http ${code:-000}): the pull will tell." ;;
esac

if remote "sudo grep -qE '^[[:space:]]*COOLERCONTROL_TAG=' '${STACK_DIR}/.env'"; then
  echo "[!] COOLERCONTROL_TAG is set in ${STACK_DIR}/.env and overrides the compose default:" >&2
  echo "    remove it (sudoedit ${STACK_DIR}/.env), then run again." >&2
  exit 1
fi
CURRENT="$(remote "sudo grep -oE 'COOLERCONTROL_TAG:-[^}]+' '${STACK_DIR}/docker-compose.yml'" | cut -d- -f2)"
RUNNING="$(remote "sudo docker ps --filter name=^${APP}\$ --format '{{.Image}}'" | sed 's/.*://')"
echo "[=] NAS compose: ${CURRENT:-?}, running: ${RUNNING:-not running}"
if [[ "$CURRENT" == "$VERSION" && "$RUNNING" == "$VERSION" ]]; then
  echo "[=] already on ${VERSION}: nothing to do."
  exit 0
fi
grep -q 'COOLERCONTROL_TAG:-' "$COMPOSE_LOCAL" || { echo "No COOLERCONTROL_TAG default in ${COMPOSE_LOCAL}." >&2; exit 1; }

APPS_ROOT="$(remote "sudo sed -n 's/^APPS_ROOT=//p' '${STACK_DIR}/.env'" | tr -d "\"' ")"
BIND="$(remote "sudo sed -n 's/^COOLERCONTROL_BIND=//p' '${STACK_DIR}/.env'" | tr -d "\"' ")"
APPS_ROOT="${APPS_ROOT:-/mnt/slow}"; BIND="${BIND:-0.0.0.0}"; [[ "$BIND" == "0.0.0.0" ]] && BIND=localhost
DATASET="$(remote "sudo zfs list -H -o name '${APPS_ROOT}/apps-coolercontrold'")"

# --- 2. Snapshot ---
SNAP="before-${VERSION}"
remote "sudo zfs list -H -t snapshot '${DATASET}@${SNAP}'" >/dev/null 2>&1 && SNAP="${SNAP}-$(date +%Y%m%d-%H%M%S)"
remote "sudo zfs snapshot -r '${DATASET}@${SNAP}'"
echo "[+] snapshot ${DATASET}@${SNAP} (recursive)"

# --- 3. Pull ---
remote "sudo docker pull -q '${IMAGE}:${VERSION}'" >/dev/null
echo "[+] image ${IMAGE}:${VERSION} pulled"

# --- 4. Compose file + redeploy ---
cp "$COMPOSE_LOCAL" "${COMPOSE_LOCAL}.orig"
trap 'rm -f "${COMPOSE_LOCAL}.orig"' EXIT
sed -i -E "s/(COOLERCONTROL_TAG:-)[^}]+/\1${VERSION}/" "$COMPOSE_LOCAL"
remote "sudo cp '${STACK_DIR}/docker-compose.yml' '${STACK_DIR}/docker-compose.yml.bak'"
ssh "$NAS" "sudo sh -c 'cat > \"${STACK_DIR}/docker-compose.yml\"'" < "$COMPOSE_LOCAL"
echo "[+] docker-compose.yml copied (previous one: docker-compose.yml.bak); redeploying..."
remote "sudo midclt call -j app.redeploy '${APP}' >/dev/null 2>&1" || true

# --- 5. Health check ---
wait_handshake() { # up to 2 minutes, on the NAS (same check as the fan watchdog)
  remote "for i in \$(seq 1 24); do
            [ \"\$(curl -s -m 5 -o /dev/null -w '%{http_code}' http://${BIND}:11987/handshake)\" = 200 ] && exit 0
            sleep 5
          done; exit 1"
}
# The new image must be the one answering (a failed redeploy can leave the old container up).
if wait_handshake && [[ "$(remote "sudo docker ps --filter name=^${APP}\$ --format '{{.Image}}'")" == "${IMAGE}:${VERSION}" ]]; then
  RUNNING="$(remote "sudo docker ps --filter name=^${APP}\$ --format '{{.Image}} ({{.Status}})'")"
  echo "[+] OK: ${RUNNING}, /handshake = 200"
  remote "h=\$(grep -l ug201x /sys/class/hwmon/hwmon*/name 2>/dev/null | head -1 | xargs -r dirname);
          [ -n \"\$h\" ] && grep -H . \$h/pwm[0-9] \$h/fan[0-9]_input 2>/dev/null | sed 's|.*/||; s|^|    |'" || true
  echo "    Check the curves in the web UI (http://${BIND}:11987), then commit docker-compose.yml."
  echo "    Rollback snapshot kept: ${DATASET}@${SNAP} (sudo zfs destroy -r ${DATASET}@${SNAP} once trusted)."
  echo "=== Done ==="
  exit 0
fi

# --- 6. Rollback ---
echo "[!] ${IMAGE}:${VERSION} not running or /handshake not answering after 2 minutes: rolling back to ${CURRENT:-previous version}" >&2
remote "sudo docker logs --tail 15 '${APP}' 2>&1 | sed 's/^/    /'" >&2 || true
remote "sudo cp '${STACK_DIR}/docker-compose.yml.bak' '${STACK_DIR}/docker-compose.yml'"
cp "${COMPOSE_LOCAL}.orig" "$COMPOSE_LOCAL"
remote "sudo midclt call -j app.stop '${APP}' >/dev/null 2>&1" || true
remote "for ds in \$(sudo zfs list -H -o name -r '${DATASET}'); do sudo zfs rollback -r \"\$ds@${SNAP}\"; done"
echo "[+] ${DATASET} rolled back to @${SNAP}" >&2
remote "sudo midclt call -j app.start '${APP}' >/dev/null 2>&1" || true
if wait_handshake; then
  echo "[+] back on ${CURRENT:-previous version}, /handshake = 200" >&2
else
  echo "[!] still no /handshake after rollback: check Apps > ${APP} (the watchdog keeps fans at 60 %)." >&2
fi
exit 1
