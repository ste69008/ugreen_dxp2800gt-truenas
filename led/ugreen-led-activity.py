#!/usr/bin/env python3
"""Drives the front-panel LEDs (power/disk1/disk2/network_stat) through the
existing leds-sio-201x kernel driver's /sys/class/leds interface, based on
system activity.

Deliberately does not use the driver's blink trigger integration: it expects
a custom "timer2" LED trigger name that nothing on this system registers, so
writing the standard kernel "timer" trigger silently does nothing. Instead
this polls activity counters and toggles brightness directly - the same
brightness_set() path the driver already serializes through its ug_201x_lock
mutex, so it stays safely synchronized with the fan/hwmon driver sharing that
chip. Do not run ugreen_leds_cli (the cs201x userspace tool) alongside this:
it writes the same EC registers directly, bypassing the driver's cached
state and desynchronizing it (observed in testing - see project notes).
"""

import time

LED_DIR = "/sys/class/leds"
POLL_INTERVAL = 0.5   # seconds
MIN_ON_TIME = 0.3     # seconds an activity LED stays on after a blip, so it's visible

DISK_DEVICES = {"disk1": "sda", "disk2": "sdb"}
NET_INTERFACE = "enp1s0"

# led_color_table indices in leds-sio-201x.c: 1=white 2=orange 3=red 4=green 5=blue
COLOR_ORANGE = 2
COLOR_GREEN = 4
COLOR_BLUE = 5

LED_COLORS = {
    "power": COLOR_BLUE,
    "network_stat": COLOR_ORANGE,
    "disk1": COLOR_GREEN,
    "disk2": COLOR_GREEN,
}


def disk_io_counter(dev):
    with open(f"/sys/block/{dev}/stat") as f:
        fields = f.read().split()
    # fields[2] = sectors read, fields[6] = sectors written
    return int(fields[2]) + int(fields[6])


def net_io_counter(iface):
    with open(f"/sys/class/net/{iface}/statistics/rx_bytes") as f:
        rx = int(f.read())
    with open(f"/sys/class/net/{iface}/statistics/tx_bytes") as f:
        tx = int(f.read())
    return rx + tx


def set_led(name, on):
    # The driver's do_brightness_set() only special-cases the exact values 0
    # and 1 as "set mode off/on" (ArgbModeSet); any other value only adjusts
    # the light level via ArgbLightSet and does nothing if the LED isn't
    # already in "on" mode. Stick to 0/1 so every write actually toggles it.
    with open(f"{LED_DIR}/{name}/brightness", "w") as f:
        f.write("1" if on else "0")


def set_color(name, color):
    with open(f"{LED_DIR}/{name}/color", "w") as f:
        f.write(str(color))


def prime_led(name, on):
    # The driver only writes hardware when the requested state differs from
    # its cached belief. Force a real write regardless of that cache by
    # always passing through the opposite state first. brightness_set()
    # defers the actual hardware write to a workqueue (schedule_work()); a
    # second write arriving before that work item runs overwrites the
    # pending value instead of producing two real writes, so this needs a
    # delay well past normal workqueue scheduling latency, not just enough
    # to separate the two write() calls.
    set_led(name, not on)
    time.sleep(0.5)
    set_led(name, on)
    time.sleep(0.5)


def main():
    activity_leds = list(DISK_DEVICES.keys()) + ["network_stat"]
    for led in activity_leds:
        prime_led(led, False)
    prime_led("power", True)

    for led, color in LED_COLORS.items():
        set_color(led, color)

    disk_counters = {led: disk_io_counter(dev) for led, dev in DISK_DEVICES.items()}
    net_counter = net_io_counter(NET_INTERFACE)

    led_state = {led: False for led in activity_leds}
    last_active_at = {led: 0.0 for led in activity_leds}

    while True:
        time.sleep(POLL_INTERVAL)
        now = time.monotonic()

        for led, dev in DISK_DEVICES.items():
            try:
                counter = disk_io_counter(dev)
            except OSError:
                continue
            if counter != disk_counters[led]:
                last_active_at[led] = now
            disk_counters[led] = counter

        try:
            counter = net_io_counter(NET_INTERFACE)
            if counter != net_counter:
                last_active_at["network_stat"] = now
            net_counter = counter
        except OSError:
            pass

        for led in activity_leds:
            want_on = (now - last_active_at[led]) < MIN_ON_TIME
            if want_on != led_state[led]:
                set_led(led, want_on)
                led_state[led] = want_on


if __name__ == "__main__":
    main()
