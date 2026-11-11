#!/bin/bash
# Fan watchdog, run every 2 minutes by a TrueNAS cron job.
#
# 1. Failsafe: checks coolercontrold's REST API liveness (GET /handshake,
#    unauthenticated). If the daemon is unreachable it can no longer be told
#    to do anything through its own API, so this writes a safe fixed fan
#    speed directly to the ug-sio201 sysfs PWM channels, bypassing the
#    container entirely. This happens on the first failed check.
# 2. Monitoring: checks the driver (hwmon device present) and both fan
#    tachometers.
#
# Alerting: whatever this script prints on stdout is e-mailed by TrueNAS (the
# cron job must not hide stdout). A condition is reported only once it has
# been seen on CONFIRM_RUNS consecutive runs (2 runs = 2-4 minutes, which
# filters transient readings such as a 0 rpm blip while the app restarts),
# then once, again every REMIND_SECONDS while it lasts, and with a
# "RECOVERED" line when it ends. Nothing is printed when everything is fine.
# Everything is also logged to syslog (tag coolercontrold-watchdog).

set -uo pipefail

# Listen address from the stack's .env (COOLERCONTROL_BIND), same as the published port.
STACK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COOLERCONTROL_BIND="$(sed -n 's/^COOLERCONTROL_BIND=//p' "$STACK_DIR/.env" 2>/dev/null | tr -d '"'"'"' ')"
API_HOST="${COOLERCONTROL_BIND:-0.0.0.0}"
[ "$API_HOST" = "0.0.0.0" ] && API_HOST="localhost"

# The variables with a ${VAR:-default} can be overridden from the environment, e.g. to run the script
# against a fake sysfs tree and a fake API.
API_URL="${API_URL:-http://${API_HOST}:11987/handshake}"
PLATFORM_DEV="${PLATFORM_DEV:-/sys/devices/platform/ug-sio201.0}"
STATE_DIR="${STATE_DIR:-/run/coolercontrold-watchdog}" # tmpfs: a reboot forgets the state
TIMEOUT=5
FAILSAFE_PERCENT=60
PWM_MAX=255

# Below these speeds a fan counts as stalled. Calibration of this unit: the CPU fan turns >= 929 rpm
# and the system fan >= 247 rpm once started; at the 25 % curve floor they read ~2000 and ~620 rpm.
FAN1_MIN_RPM="${FAN1_MIN_RPM:-800}"
FAN2_MIN_RPM="${FAN2_MIN_RPM:-400}"
CONFIRM_RUNS="${CONFIRM_RUNS:-2}"
REMIND_SECONDS="${REMIND_SECONDS:-21600}" # 6 h

HOST="$(hostname)"

log() {
    logger -t coolercontrold-watchdog "$1"
}

stamp() { date '+%Y-%m-%d %H:%M:%S'; }

mkdir -p "$STATE_DIR" 2>/dev/null

# report KEY BAD(0|1) BAD_MESSAGE OK_MESSAGE
report() {
    local key=$1 bad=$2 bad_msg=$3 ok_msg=$4
    local count_f="$STATE_DIR/$key.count" alert_f="$STATE_DIR/$key.alert"
    local count now since last
    now=$(date +%s)

    if [ "$bad" = 1 ]; then
        count=$(cat "$count_f" 2>/dev/null); count=$(( ${count:-0} + 1 ))
        echo "$count" > "$count_f"
        [ "$count" -ge "$CONFIRM_RUNS" ] || return 0
        if [ ! -f "$alert_f" ]; then
            echo "$now $now" > "$alert_f"
            log "ALERT: $bad_msg"
            echo "[$HOST $(stamp)] ALERT: $bad_msg"
        else
            read -r since last < "$alert_f"
            if [ $(( now - last )) -ge "$REMIND_SECONDS" ]; then
                echo "$since $now" > "$alert_f"
                log "REMINDER: $bad_msg"
                echo "[$HOST $(stamp)] REMINDER (since $(date -d "@$since" '+%Y-%m-%d %H:%M:%S')): $bad_msg"
            fi
        fi
    else
        if [ -f "$alert_f" ]; then
            read -r since _ < "$alert_f"
            log "RECOVERED: $ok_msg"
            echo "[$HOST $(stamp)] RECOVERED: $ok_msg (it had been failing since $(date -d "@$since" '+%Y-%m-%d %H:%M:%S'))"
        fi
        rm -f "$count_f" "$alert_f"
    fi
}

# read_int FILE: prints the file's content if it is a non-negative integer
read_int() {
    local v
    v=$(cat "$1" 2>/dev/null) || return 1
    [[ "$v" =~ ^[0-9]+$ ]] || return 1
    echo "$v"
}

HWMON_DIR=$(find "$PLATFORM_DEV/hwmon" -mindepth 1 -maxdepth 1 -name 'hwmon*' 2>/dev/null | head -n1)

# --- 1. coolercontrold liveness and fan failsafe -------------------------------------------------------
http_code=$(curl -s -m "$TIMEOUT" -o /dev/null -w '%{http_code}' "$API_URL" 2>/dev/null)
http_code="${http_code:-000}"

api_bad=0
failsafe_note=""
if [ "$http_code" != "200" ]; then
    api_bad=1
    log "coolercontrold API unreachable (http=$http_code) - engaging fan failsafe at ${FAILSAFE_PERCENT}%"

    if [ -z "$HWMON_DIR" ]; then
        log "ERROR: could not locate hwmon directory under $PLATFORM_DEV - cannot apply failsafe"
        failsafe_note="the failsafe could NOT be applied (no hwmon device)"
    else
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
            failsafe_note="$applied fan(s) forced to ${FAILSAFE_PERCENT}%"
        else
            log "ERROR: no pwm channel could be written under $HWMON_DIR"
            failsafe_note="the failsafe could NOT be applied (no pwm channel writable)"
        fi
    fi
fi

report api "$api_bad" \
    "coolercontrold API unreachable (http=$http_code); $failsafe_note. Fan curves are not being applied." \
    "coolercontrold API reachable again"

# --- 2. driver and fan tachometers ----------------------------------------------------------------------
if [ -z "$HWMON_DIR" ]; then
    report driver 1 \
        "no ug201x hwmon device under $PLATFORM_DEV (kernel module not loaded?): fans and sensors cannot be read or controlled." \
        ""
    exit 0
fi
report driver 0 "" "ug201x hwmon device present again"

check_fan() { # KEY LABEL RPM_FILE PWM_FILE MIN_RPM
    local key=$1 label=$2 min=$5 rpm duty bad=0 detail
    rpm=$(read_int "$3") || rpm=""
    duty=$(read_int "$4") && duty=$(( duty * 100 / PWM_MAX )) || duty="?"
    if [ -z "$rpm" ]; then
        bad=1; detail="speed unreadable"
    elif [ "$rpm" -lt "$min" ]; then
        bad=1; detail="${rpm} rpm, below the ${min} rpm threshold"
    else
        detail="${rpm} rpm"
    fi
    report "$key" "$bad" \
        "$label fan stalled or too slow: $detail (duty ${duty}%)." \
        "$label fan back to $detail (duty ${duty}%)"
}

check_fan fan1 "CPU" "$HWMON_DIR/fan1_input" "$HWMON_DIR/pwm1" "$FAN1_MIN_RPM"
check_fan fan2 "System" "$HWMON_DIR/fan2_input" "$HWMON_DIR/pwm2" "$FAN2_MIN_RPM"

exit 0
