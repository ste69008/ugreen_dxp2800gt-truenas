#!/bin/bash
# Checks coolercontrold's REST API liveness (GET /handshake, unauthenticated).
# If the daemon is unreachable, it can no longer be told to do anything
# through its own API, so this writes a safe fixed fan speed directly to
# the ug-sio201 sysfs PWM channels, bypassing the container entirely.

set -uo pipefail

API_URL="http://localhost:11987/handshake"
TIMEOUT=5
FAILSAFE_PERCENT=60
PWM_MAX=255
PLATFORM_DEV="/sys/devices/platform/ug-sio201.0"

log() {
    logger -t coolercontrold-watchdog "$1"
}

http_code=$(curl -s -m "$TIMEOUT" -o /dev/null -w '%{http_code}' "$API_URL" 2>/dev/null)
http_code="${http_code:-000}"

if [ "$http_code" = "200" ]; then
    exit 0
fi

log "coolercontrold API unreachable (http=$http_code) - engaging fan failsafe at ${FAILSAFE_PERCENT}%"

HWMON_DIR=$(find "$PLATFORM_DEV/hwmon" -mindepth 1 -maxdepth 1 -name 'hwmon*' 2>/dev/null | head -n1)

if [ -z "$HWMON_DIR" ]; then
    log "ERROR: could not locate hwmon directory under $PLATFORM_DEV - cannot apply failsafe"
    exit 1
fi

PWM_VALUE=$(( PWM_MAX * FAILSAFE_PERCENT / 100 ))

applied=0
for pwm in "$HWMON_DIR"/pwm[0-9]; do
    [ -e "$pwm" ] || continue
    if echo "$PWM_VALUE" > "$pwm" 2>/dev/null; then
        applied=$((applied + 1))
    else
        log "ERROR: failed to write $PWM_VALUE to $pwm"
    fi
done

if [ "$applied" -gt 0 ]; then
    log "Failsafe applied: $applied fan(s) set to ${FAILSAFE_PERCENT}% ($PWM_VALUE/$PWM_MAX) via $HWMON_DIR"
else
    log "ERROR: no pwm channel could be written under $HWMON_DIR"
fi
