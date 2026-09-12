# ugreen_dxp2800gt-truenas_fan

Fan and front-panel LED control for the **UGREEN DXP2800GT** NAS running
**TrueNAS SCALE**.

## Why this exists

The DXP2800GT's fans, temperature sensors and front-panel LEDs (power,
disk1, disk2, network) are all driven by an IT8987-family Super I/O chip
that TrueNAS SCALE has no built-in driver for. UGREEN publishes a GPL kernel
driver for it, but it isn't packaged for TrueNAS and isn't wired into any
standard Linux fan-control tooling out of the box. This repo compiles that
driver out-of-tree, exposes it through the standard `hwmon` kernel
interface, and layers ordinary Linux tooling (`coolercontrold`, a small
Python daemon, systemd, cron) on top to get:

- Temperature-based fan curves with a web UI.
- A failsafe that keeps the fans running at a safe speed if that control
  software ever goes down.
- Front-panel LEDs that reflect real disk/network activity.
- All of the above surviving TrueNAS reboots **and** TrueNAS updates —
  which is not automatic, see [Persistence](#persistence) below.

See [INSTALL.md](INSTALL.md) for how to actually deploy this on a NAS.

## Architecture — fans

```
ug201x_full.ko  →  /sys/class/hwmon/hwmonX (chip "ug201x")  →  coolercontrold  →  watchdog (cron, every 2 min)
   (kernel)              fan1/fan2, temp1/temp2,                (fan curves,        (falls back to a fixed
                          pwm1/pwm2                               web UI :11987)      PWM % if the API dies)
```

- **`driver/`** — the out-of-tree kernel module. `ug_201x.c` (patched with a
  `hwmon` interface, see below) talks to the Super I/O chip over its EC
  ports and implements `GetFanRpm()`/`SetFanDuty()`/`GetNTCTemperature()`.
  `leds-sio-201x.c` registers the LED classdevs (see the LED section).
  Both are compiled into a single module, `ug201x_full.ko`, and loaded with
  `insmod` — it is **not** installed into `/lib/modules` or auto-loaded via
  `depmod`, because `/usr` (where `depmod`'s search path lives) is read-only
  on TrueNAS SCALE. See [Kernel driver](#kernel-driver) below for why it's
  patched, and [INSTALL.md](INSTALL.md) for how it's built and loaded.

  Once loaded, the module registers hwmon chip `ug201x` with two fan
  channels (`fan1`=CPU fan, `fan2`=system fan), two temperature channels
  (`temp1`=CPU, `temp2`=board) and two writable PWM channels (`pwm1`,
  `pwm2`, 0-255). It also keeps UGREEN's original `/proc/it86/{temp,fan}`
  interface working side by side.

- **`docker-compose/coolercontrold/`** — [CoolerControl](https://gitlab.com/coolercontrol/coolercontrol)'s
  daemon, installed as a TrueNAS **Custom App** (Apps → Discover Apps →
  Custom App → Install via YAML, using `docker-compose.yml` as-is) bound to
  the `ug201x` hwmon device. TrueNAS runs it as container
  `ix-coolercontrold-coolercontrold-1`, but that name is an implementation
  detail — nothing else in this repo depends on it, since the watchdog only
  talks to it over HTTP and reads/writes sysfs directly. It applies
  temperature→PWM curves per fan and exposes a web UI on port `11987` to
  edit them. Its calibration/curves are persisted under
  `/mnt/slow/docker/coolercontrold`, a plain host-path bind mount kept
  outside the app's own managed storage so it survives the app being
  deleted and recreated.

  - `scripts/coolercontrold-watchdog.sh` — runs every 2 minutes via a
    TrueNAS cron job. If `coolercontrold`'s API doesn't answer, it writes a
    fixed 60% duty cycle directly to `pwm1`/`pwm2` via sysfs, bypassing the
    container entirely, so the fans never get stuck wherever they happened
    to be (or worse, off) if the container crashes or is being updated.
  - `scripts/coolercontrold-backup.sh` — manual (not scheduled) backup of
    the CoolerControl data directory (config, curves, session files) to a
    timestamped zip.

## Architecture — LEDs

```
leds-sio-201x.c (same ug201x_full.ko)  →  /sys/class/leds/{power,disk1,disk2,network_stat}  →  ugreen-led-activity.py (systemd)
```

- The same kernel module also registers four LED classdevs. `power` is set
  once at startup and left on. `disk1`/`disk2`/`network_stat` are activity
  indicators with no built-in trigger wired up on this system (see the
  script's docstring for why the kernel `timer` trigger doesn't help here).
- **`led/ugreen-led-activity.py`** — polls `/sys/block/{sda,sdb}/stat` and
  `/sys/class/net/enp1s0/statistics/*` twice a second and toggles each LED's
  `brightness` (0/1) directly through the driver whenever it sees new I/O,
  so the LEDs blink on real activity. It sets each LED's color once at
  startup (`disk1`/`disk2`=green, `network_stat`=orange, `power`=blue) via
  the driver's `color` sysfs attribute.
- **`led/ugreen-led-activity.service`** — the systemd unit that keeps the
  script running (`Restart=always`).

  **Do not run `ugreen_leds_cli`** (the community `cs201x` userspace tool)
  at the same time as this service: it writes the same EC registers
  directly, bypassing the driver's cached state, and will desynchronize the
  two (observed in testing).

## Kernel driver

`driver/upstream/` holds **unmodified** copies of UGREEN's GPL sources
(see `driver/upstream/SOURCE.md` for exactly where they came from). Using
them as-is only gets you the legacy `/proc/it86/{temp,fan}` interface —
useful for a quick check, useless to `coolercontrold` or `lm-sensors`.

`driver/ug_201x-hwmon.patch` adds a standard `hwmon` interface (temp/fan/pwm
sysfs attributes) on top of `upstream/ug_201x.c`, so any hwmon-aware tool
can use it without knowing anything about this driver. `driver/ug_201x-hwmon.c`
is that patch already applied — it's the file actually compiled (as
`ug_201x.c`) into `ug201x_full.ko`. `leds-sio-201x.c` is used unmodified
from `upstream/`.

## Persistence

TrueNAS SCALE mounts `/` and `/usr` **read-only** and replaces them wholesale
on every update — anything placed under `/usr/local/sbin`, `/lib/modules`,
etc. does not survive, and a compiled kernel module is tied to an exact
kernel build (`vermagic`) that changes even on minor updates. `/etc` is
writable but not guaranteed to survive either. Only pool datasets
(`/mnt/<pool>/...`) are guaranteed persistent across any update, including a
full reinstall.

So every file this project needs at runtime lives under `/mnt/slow/scripts/`,
and **`boot/ug201x-boot.sh`** is registered as a TrueNAS **Init/Shutdown
Script** (`POSTINIT`, stored in TrueNAS's own config database — not the
filesystem) that runs on every boot to:

1. Load `ug201x_full.ko` if it isn't already loaded.
2. If the module is missing, or was built for a different kernel than the
   one currently running (`vermagic` mismatch — e.g. after a TrueNAS update
   that bumped the kernel), rebuild it on the spot from
   `/mnt/slow/scripts/ug201x/` using the same Docker-based build described
   in [INSTALL.md](INSTALL.md), then load it.
3. Restore `/etc/systemd/system/ugreen-led-activity.service` from the
   persistent copy if it's missing, and (re-)enable the service.

`coolercontrold` needs no equivalent script: as a TrueNAS Custom App it's
started by TrueNAS's own app framework on every boot (its app definition
lives in TrueNAS's config database, not on the read-only filesystem). Its
watchdog is a TrueNAS **Cron Job** (also config-database-backed), not a
plain crontab entry.

See [INSTALL.md](INSTALL.md) for exact setup commands.

## Repository layout

```
driver/
  upstream/            unmodified UGREEN GPL sources (reference)
  ug_201x-hwmon.patch  adds a hwmon interface to upstream/ug_201x.c
  ug_201x-hwmon.c      that patch already applied (what actually gets built, as ug_201x.c)
  Makefile             out-of-tree module build rules
boot/
  ug201x-boot.sh        POSTINIT script: (re)loads the module, restores the LED service
led/
  ugreen-led-activity.py       activity-LED daemon
  ugreen-led-activity.service  its systemd unit
docker-compose/coolercontrold/
  docker-compose.yml           coolercontrold container (installed as a TrueNAS Custom App)
  scripts/coolercontrold-watchdog.sh   fan failsafe, run every 2 min via cron
  scripts/coolercontrold-backup.sh     manual config backup
```

## License

[GPL-2.0-only](LICENSE), matching the `SPDX-License-Identifier: GPL-2.0-only`
header UGREEN ships on `driver/upstream/ug_201x.c` and
`driver/upstream/leds-sio-201x.c`, which this project's own driver patch and
build files (`driver/ug_201x-hwmon.patch`, `driver/ug_201x-hwmon.c`,
`driver/Makefile`) are derived from and must stay under. The rest of this
repo (LED daemon, watchdog, boot script, docs) is released under the same
license for simplicity.
