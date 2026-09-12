#!/bin/bash
# TrueNAS Init/Shutdown Script (POSTINIT).
#
# TrueNAS SCALE replaces / and /usr wholesale on every update, so the
# ug201x kernel module (which also needs rebuilding whenever the kernel
# version changes, since it's out-of-tree) and the LED activity systemd
# unit (installed under /etc, which is not guaranteed to survive every
# update either) don't survive on their own. This script restores both
# on every boot from the persistent copies under /mnt/slow/scripts.
set -uo pipefail

BASE_DIR="/mnt/slow/scripts/ug201x"
MODULE="$BASE_DIR/ug201x_full.ko"
KVER="$(uname -r)"
LED_UNIT_SRC="/mnt/slow/scripts/ugreen-led-activity.service"
LED_UNIT_DST="/etc/systemd/system/ugreen-led-activity.service"
BUILD_LOG="/tmp/ug201x-build.log"

log() { logger -t ug201x-boot "$1"; }

module_vermagic_matches() {
    modinfo "$MODULE" 2>/dev/null | grep vermagic | grep -qF "$KVER"
}

# --- kernel module ---------------------------------------------------
if lsmod | grep -q '^ug201x_full'; then
    log "ug201x_full already loaded"
else
    if [ ! -f "$MODULE" ] || ! module_vermagic_matches; then
        log "module missing or built for a different kernel ($KVER), rebuilding"
        if docker run --rm \
            -v "$BASE_DIR":/src \
            -v "/lib/modules/$KVER":"/lib/modules/$KVER" \
            -v /usr/src:/usr/src \
            debian:bookworm-slim sh -c "
                apt-get update -qq &&
                apt-get install -y -qq build-essential libelf-dev dwarves &&
                cd /src && make clean && make CONFIG_DEBUG_INFO_BTF= -j\$(nproc)
            " >"$BUILD_LOG" 2>&1; then
            log "rebuild succeeded"
        else
            log "rebuild FAILED - see $BUILD_LOG on the host"
        fi
    fi

    if [ -f "$MODULE" ] && module_vermagic_matches; then
        if insmod "$MODULE" 2>>"$BUILD_LOG"; then
            log "ug201x_full.ko loaded for kernel $KVER"
        else
            log "insmod failed - see $BUILD_LOG on the host"
        fi
    else
        log "no usable module for kernel $KVER - LEDs/fan control unavailable"
    fi
fi

# --- LED activity service --------------------------------------------
if [ ! -f "$LED_UNIT_DST" ] || ! cmp -s "$LED_UNIT_SRC" "$LED_UNIT_DST"; then
    cp "$LED_UNIT_SRC" "$LED_UNIT_DST"
    systemctl daemon-reload
    log "restored ugreen-led-activity.service unit"
fi
systemctl enable --now ugreen-led-activity.service
