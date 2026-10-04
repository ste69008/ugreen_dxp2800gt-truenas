# Install

Assumes a DXP2800GT running TrueNAS SCALE, SSH access with a sudo-capable
user, and a data pool mounted at `/mnt/slow` (substitute your own pool name
throughout — it doesn't have to be called `slow`).

Everything under `/mnt/<pool>/scripts` below is placed there specifically
because `/` and `/usr` are read-only and get replaced on every TrueNAS
update — see [README.md § Persistence](README.md#persistence) for why. Do
**not** shortcut this by putting files under `/usr/local/sbin` or similar;
they will silently disappear on the next update.

## 1. Copy the driver sources to the NAS

```bash
ssh <nas> "sudo mkdir -p /mnt/slow/scripts/ug201x"
scp driver/Makefile driver/ug_201x-hwmon.c driver/upstream/leds-sio-201x.c <nas>:/tmp/
```

Then on the NAS, note the **rename**: the patched source must land as
`ug_201x.c` (the Makefile and module both expect that name):

```bash
sudo mv /tmp/Makefile /tmp/leds-sio-201x.c /mnt/slow/scripts/ug201x/
sudo mv /tmp/ug_201x-hwmon.c /mnt/slow/scripts/ug201x/ug_201x.c
```

## 2. Build the kernel module

Kernel headers must already be present for the running kernel
(`/lib/modules/$(uname -r)/build` should exist — this is standard on
TrueNAS SCALE). The build itself runs inside a throwaway Debian container
so no compiler needs to be installed on the NAS itself:

```bash
sudo docker run --rm \
  -v /mnt/slow/scripts/ug201x:/src \
  -v /lib/modules/$(uname -r):/lib/modules/$(uname -r) \
  -v /usr/src:/usr/src \
  debian:bookworm-slim sh -c "
    apt-get update -qq
    apt-get install -y -qq build-essential libelf-dev dwarves
    cd /src
    make clean
    make CONFIG_DEBUG_INFO_BTF= -j\$(nproc)
"
```

This produces `/mnt/slow/scripts/ug201x/ug201x_full.ko`. Sanity-check it
was built for the kernel you're actually running:

```bash
modinfo /mnt/slow/scripts/ug201x/ug201x_full.ko | grep vermagic
uname -r
```

## 3. Load it and check

```bash
sudo insmod /mnt/slow/scripts/ug201x/ug201x_full.ko
ls /sys/class/leds/                       # power, disk1, disk2, network_stat
cat /proc/it86/temp /proc/it86/fan        # legacy interface, still works
for d in /sys/class/hwmon/hwmon*; do cat $d/name; done | grep ug201x
```

If `insmod` fails with an unknown-symbol or invalid-module-format error, the
vermagic doesn't match the running kernel — rebuild (step 2) against the
current `uname -r` before continuing.

## 4. Deploy coolercontrold

Same layout as any other stack on this NAS:

- the stack directory (compose file, `.env`, scripts) lives in
  `/mnt/slow/docker-compose/coolercontrold/`;
- its data (config, calibration, curves) lives in the dataset
  `slow/apps-coolercontrold/config`, created by `preconfigure.sh` — it survives
  the app being deleted and recreated, and TrueNAS updates;
- the TrueNAS **Custom App** `coolercontrold` only *includes* the compose file
  (`include:` + `services: {}`), so the definition stays in this repo, not in
  TrueNAS's config database. TrueNAS still starts it on every boot and shows
  it under **Apps**.

Copy the stack and create its `.env` (set `COOLERCONTROL_BIND` to the NAS
management IP to keep the web UI off the other interfaces/VLANs, and
`APPS_ROOT` if your pool isn't `slow`):

```bash
ssh <nas> "mkdir -p /tmp/cc && sudo mkdir -p /mnt/slow/docker-compose/coolercontrold"
scp -r docker-compose/coolercontrold/{docker-compose.yml,.env.example,preconfigure.sh,scripts} <nas>:/tmp/cc/
ssh <nas> "
  sudo cp -r /tmp/cc/. /mnt/slow/docker-compose/coolercontrold/ && rm -rf /tmp/cc
  cd /mnt/slow/docker-compose/coolercontrold
  sudo cp -n .env.example .env && sudo chmod 600 .env
  sudo chmod +x preconfigure.sh scripts/*.sh
"
ssh -t <nas> "sudoedit /mnt/slow/docker-compose/coolercontrold/.env"   # COOLERCONTROL_BIND, APPS_ROOT
```

Create the datasets, then the app:

```bash
ssh <nas> "sudo bash /mnt/slow/docker-compose/coolercontrold/preconfigure.sh --pool slow"
ssh <nas> "sudo bash /mnt/slow/docker-compose/coolercontrold/scripts/app-install.sh"
```

`app-install.sh` creates the app, or — if an app named `coolercontrold`
already exists, e.g. one installed by pasting the full YAML — switches it to
the include in place. Coming from the previous layout of this repo (data in
`/mnt/slow/docker/coolercontrold`), `preconfigure.sh` copies that data into
the new dataset if it's still empty; delete the old directory once the app
runs fine.

To change the stack later (image tag, port binding): edit the files in
`/mnt/slow/docker-compose/coolercontrold/`, then **Apps → coolercontrold →
Redeploy** (or `midclt call -j app.redeploy coolercontrold`).

The web UI is on `http://<COOLERCONTROL_BIND>:11987`; set up your fan curves
there once the `ug201x` device shows up in it.

## 5. Deploy the fan watchdog

The watchdog and backup scripts were copied with the stack in step 4
(`scripts/`); they read `COOLERCONTROL_BIND` / `APPS_ROOT` from its `.env`.

Register it as a **TrueNAS Cron Job** (System Settings → Advanced → Cron
Jobs in the UI, or via the API as below) — not a plain `crontab`, so it
survives updates:

```bash
midclt call cronjob.create '{
  "command": "/mnt/slow/docker-compose/coolercontrold/scripts/coolercontrold-watchdog.sh",
  "schedule": {"minute": "*/2", "hour": "*", "dom": "*", "month": "*", "dow": "*"},
  "user": "root",
  "enabled": true,
  "stdout": false,
  "stderr": true,
  "description": "coolercontrold healthcheck -> fan failsafe 60%"
}'
```

`coolercontrold-backup.sh` is intentionally *not* scheduled — run it by hand
before anything risky (e.g. before a TrueNAS update). It writes a zip of
`apps-coolercontrold/config` to `/mnt/slow/backup/`.

## 6. Deploy the LED activity service

The script's `DISK_DEVICES` (`sda`/`sdb`) and `NET_INTERFACE` (`enp1s0`) are
hardcoded — check they match your NAS (`lsblk -d`, `ip -brief link`) before
copying, and edit `led/ugreen-led-activity.py` if they don't.

```bash
ssh <nas> "sudo mkdir -p /mnt/slow/scripts"
scp led/ugreen-led-activity.py led/ugreen-led-activity.service <nas>:/tmp/
ssh <nas> "
  sudo install -m 755 /tmp/ugreen-led-activity.py /mnt/slow/scripts/ugreen-led-activity.py
  sudo install -m 644 /tmp/ugreen-led-activity.service /mnt/slow/scripts/ugreen-led-activity.service
  sudo install -m 644 /tmp/ugreen-led-activity.service /etc/systemd/system/ugreen-led-activity.service
  rm /tmp/ugreen-led-activity.py /tmp/ugreen-led-activity.service
  sudo systemctl daemon-reload
  sudo systemctl enable --now ugreen-led-activity.service
"
```

Note both the `/etc/systemd/system` copy (what systemd actually runs) and
the `/mnt/slow/scripts` copy (the persistent source the boot script in the
next step restores from if `/etc` gets wiped by an update).

## 7. Set up persistence across reboots and updates

This is what makes steps 3 and 6 survive a reboot or a TrueNAS update
without redoing them by hand.

```bash
scp boot/ug201x-boot.sh <nas>:/tmp/
ssh <nas> "sudo install -m 755 -o root -g root /tmp/ug201x-boot.sh /mnt/slow/scripts/ug201x-boot.sh && rm /tmp/ug201x-boot.sh"
```

Register it as a **TrueNAS Init/Shutdown Script**, `POSTINIT` (System
Settings → Advanced → Init/Shutdown Scripts in the UI, or via the API):

```bash
midclt call initshutdownscript.create '{
  "type": "SCRIPT",
  "script": "/mnt/slow/scripts/ug201x-boot.sh",
  "when": "POSTINIT",
  "enabled": true,
  "timeout": 300,
  "comment": "Reload ug201x driver + LED activity service (survives updates)"
}'
```

`timeout: 300` gives it enough headroom for the (rare) case where it has to
rebuild the module for a new kernel, which involves `apt-get update` inside
the build container — normally the whole script runs in well under a
second (module already valid → plain `insmod`).

## 8. Verify

After deploying everything, reboot the NAS and check:

```bash
lsmod | grep ug201x_full
ls /sys/class/leds/ | grep -E 'power|disk|network'
for d in /sys/class/hwmon/hwmon*; do cat $d/name; done | grep ug201x
systemctl is-active ugreen-led-activity.service
sudo docker ps --filter name=coolercontrold   # container "coolercontrold", project ix-coolercontrold
journalctl -t ug201x-boot --no-pager -n 20
```

To test the fan watchdog specifically, stop the app and watch `pwm1`/`pwm2`
under the `ug201x` hwmon device jump to `153` (60%) within 2 minutes:

```bash
midclt call app.stop coolercontrold
watch cat /sys/class/hwmon/hwmonX/pwm1   # replace X
midclt call app.start coolercontrold      # restores normal curve control
```
