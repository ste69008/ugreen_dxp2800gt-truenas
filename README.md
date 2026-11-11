# ugreen_dxp2800gt-truenas

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
  daemon, bound to the `ug201x` hwmon device. It applies temperature→PWM
  curves per fan and exposes a web UI on port `11987` to edit them. Laid out
  like any other stack on the NAS: the directory is deployed to
  `/mnt/slow/docker-compose/coolercontrold/` with a local `.env`
  (`.env.example`: pool path, listen address), its data
  (config/calibration/curves) lives in the `slow/apps-coolercontrold/config`
  dataset (`preconfigure.sh`), and the TrueNAS **Custom App** `coolercontrold`
  only `include:`s the compose file (`scripts/app-install.sh`) — the
  definition stays in this repo, the data survives the app being deleted and
  recreated. Nothing depends on the container name: the watchdog talks to it
  over HTTP and reads/writes sysfs directly.

  - `scripts/coolercontrold-watchdog.sh` — runs every 2 minutes via a
    TrueNAS cron job. If `coolercontrold`'s API doesn't answer, it writes a
    fixed 60% duty cycle directly to `pwm1`/`pwm2` via sysfs, bypassing the
    container entirely, so the fans never get stuck wherever they happened
    to be (or worse, off) if the container crashes or is being updated.
    It also **e-mails alerts** (see [Alerts](#alerts)): coolercontrold
    unreachable, a stalled fan, or the `ug201x` driver missing.
  - `scripts/coolercontrold-backup.sh` — manual (not scheduled) backup of
    the CoolerControl data directory (config, curves, session files) to a
    timestamped zip.

### Fan curves

The curves are **not versioned in this repo**: they live in CoolerControl's
own config, `/mnt/slow/apps-coolercontrold/config/config.toml` (edited in the
web UI), so `scripts/coolercontrold-backup.sh` is what protects them.

As configured on 2026-11-11 (temperature °C → fan duty %, linear between
points):

| Fan | Profile | Temperature source | Points |
|---|---|---|---|
| `fan1` CPU | `CPU` | custom sensor `CPU (Moyenne)` (`sensor1`): average of the CPU die sensor (`k10temp`) and the EC's `cpu_temp` | 0→25, 40→30, 65→50, 80→80, 88→100 |
| `fan2` system | `SYSFAN-Boîtier (max)`: *Mix / Max* of the two below, the higher duty wins | | |
| | ↳ `SYSFAN` | EC board NTC (`ug201x` `temp2`) | 0→25, 40→30, 65→50, 85→75, 100→100 |
| | ↳ `SYSFAN-Disques` | custom sensor `Disques (max)` (`sensor2`): hottest of the two drives' `drivetemp` | 30→25, 40→30, 45→40, 50→60, 55→80, 60→100 |

Why they look like this:

- **25 % minimum.** The kernel driver writes whatever it is given —
  `SetFanDuty()` has no floor, and `pwm=0` stops the fan — so the curves are
  the only protection. CoolerControl's own calibration of this unit
  (2026-09-09) measured: both fans start at 12 % duty, keep spinning down to
  10 % and are stopped at 8 %; at 25 % the system fan turns ~620 rpm and the
  CPU fan ~2050 rpm. A 25 % floor is therefore about twice the stall point,
  and keeps the system fan comfortably above the 400 rpm alert threshold
  (it reads ~475 rpm at 20 %, ~250 rpm at 12 %). The
  [ugreen-nas-control](#related-projects) project uses the same 25 % floor
  for its DXP2800 GT, assuming a stall near 20 %, which this unit's
  measurements don't show.
- **The system fan follows the drives**, not only the board sensor: in a
  2-bay NAS the disks are the main heat source, and the board NTC barely moves
  during a scrub or a long SMART test. The drive curve reaches 100 % at 60 °C,
  the drives' rated maximum.
- **The CPU curve reaches 100 % at 88 °C** of the *averaged* sensor. The
  average damps the die temperature (the EC sensor reads noticeably cooler
  under load), so the die itself will be hotter than 88 °C at that point.
  Switching `sensor1` from *Avg* to *Max* would make the curve follow the die.

Notes for editing them:

- A custom sensor refers to a drive by CoolerControl's device UID, which for a
  drive is derived from its SCSI WWID (`sha256("Hwmon" + wwid)`). Replacing a
  disk changes the UID, so `sensor2` must then be re-pointed to the new drive.
- Don't lower the system fan curve below ~17 % duty: its speed would drop
  under the 400 rpm alert thresholds below.
- ZFS snapshots of the `slow/apps-coolercontrold` dataset
  (`zfs list -t snapshot`) are the quick way back from a bad edit; stop the
  app before restoring one.

### Alerts

Two independent layers:

- **CoolerControl alert "Alerte de panne - sysfan1"** — a *channel alert* on
  the RPM of `fan2`: it fires when the reading leaves 400–10000 rpm for more
  than 5 s (warm-up), clears as soon as it is back in range, does not repeat
  and does not shut the machine down. Its notification is a "desktop
  notification", which the daemon only broadcasts to connected CoolerControl
  clients (web UI, Qt app): with none open nobody is told, so it mostly serves
  the UI's alert log and state. CoolerControl has no e-mail channel.
- **E-mail from the watchdog** (`scripts/coolercontrold-watchdog.sh`, every
  2 minutes). Whatever the script prints is e-mailed by TrueNAS: the cron job
  mails its standard output to the e-mail address of the `root` user (subject
  "Cron Job Run"), which needs TrueNAS's mail settings to be configured and
  the job's *Hide Standard Output* left unchecked. The script prints nothing
  when everything is fine.

  | Condition | Threshold |
  |---|---|
  | coolercontrold's API doesn't answer (`/handshake`) | the fans are forced to 60 % at the **first** failed check; the mail follows at the second |
  | CPU fan too slow or unreadable | below 800 rpm (`FAN1_MIN_RPM`) |
  | System fan too slow or unreadable | below 400 rpm (`FAN2_MIN_RPM`) |
  | `ug201x` hwmon device missing (module not loaded) | present / absent |

  A condition is reported only once it has been seen on **two consecutive
  runs** (`CONFIRM_RUNS`, so 2–4 minutes after it starts), which filters
  transient readings such as a 0 rpm blip while the app restarts. Then you get
  one `ALERT` mail, a `REMINDER` every 6 hours (`REMIND_SECONDS`) while it
  lasts, and a `RECOVERED` mail when it ends. The thresholds are constants at
  the top of the script; the messages are in English. The per-condition state
  lives in `/run/coolercontrold-watchdog` (tmpfs, forgotten at reboot), and
  every event is also logged to syslog under `coolercontrold-watchdog`.
  Limits: an alert is 2–4 minutes late by design, and a NAS that is down
  entirely can't mail anything — that needs monitoring from the outside.

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
  two (observed in testing). The same goes for the userspace tools of
  [ugreen-nas-control](#related-projects) (`ugreen-led-ctl`, `ugreen-nas-fan`):
  they reach the same registers through `/dev/port`, with a lock the kernel
  module doesn't share, and they only recognise UGREEN's own module names
  (`leds_sio_201x`, `ug_201x`, `ug_sio201`), not `ug201x_full`.

**Known limitation — no hardware blink.** The embedded controller can blink
and "breathe" an LED by itself (mode 2 = blink, 3 = breath, period and
on-time in 50 ms units). The stock driver only reaches that through LED
triggers named `timer2` and `breath`, which don't exist on TrueNAS's kernel
(checked on 25.10.7 / 6.12.105: the `trigger` list of the four LEDs has no
`timer`, `timer2`, `breath` or `netdev`, only `disk-*`, `cpu*`, `kbd-*`, ...).
That is why `ugreen-led-activity.py` switches the LEDs on and off in software
twice a second instead. Exposing the EC's own modes would take a small driver
patch (like the hwmon one); it would also allow states the script can't do
today, such as a breathing power LED.

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

So everything this project needs at runtime lives on the pool: the driver
sources, the built module, the LED daemon and the boot script under
`/mnt/slow/scripts/`, the coolercontrold stack under
`/mnt/slow/docker-compose/coolercontrold/` and its data in the
`slow/apps-coolercontrold` dataset (full table in
[INSTALL.md](INSTALL.md#where-everything-lives-on-the-nas)). The
**`boot/ug201x-boot.sh`** script is registered as a TrueNAS **Init/Shutdown
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
started by TrueNAS's own app framework on every boot (its app definition —
an include of the compose file on the pool — lives in TrueNAS's config
database, not on the read-only filesystem). Its
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
  docker-compose.yml           coolercontrold container (included by the TrueNAS Custom App)
  .env.example                 per-NAS settings (pool path, listen address) -> .env on the NAS
  preconfigure.sh              creates the apps-coolercontrold datasets (+ migrates old data)
  scripts/app-install.sh       creates/updates the Custom App as an include of the compose file
  scripts/coolercontrold-watchdog.sh   fan failsafe, run every 2 min via cron
  scripts/coolercontrold-backup.sh     manual config backup
```

## Related projects

- [ugreen-nas-control](https://github.com/ugreen-nas-control/ugreen-nas-control)
  (MIT) — an Unraid plugin that drives the same CS201x controller from
  userspace, and whose fan notes (floor, wiring) informed the
  [guidelines above](#fan-curves). It takes its register map from the same
  UGREEN driver this repo builds, so it adds no hardware knowledge beyond it.
  Its LED backend for the DXP2800 GT is marked untested on hardware; the one
  here has run on a real unit. It cannot be run alongside this driver (see
  [Architecture — LEDs](#architecture--leds)).
- [miskcoo/ugreen_leds_controller](https://github.com/miskcoo/ugreen_leds_controller)
  — the community LED protocol and tools for other UGREEN models.

## License

[GPL-2.0-only](LICENSE), matching the `SPDX-License-Identifier: GPL-2.0-only`
header UGREEN ships on `driver/upstream/ug_201x.c` and
`driver/upstream/leds-sio-201x.c`, which this project's own driver patch and
build files (`driver/ug_201x-hwmon.patch`, `driver/ug_201x-hwmon.c`,
`driver/Makefile`) are derived from and must stay under. The rest of this
repo (LED daemon, watchdog, boot script, docs) is released under the same
license for simplicity.
