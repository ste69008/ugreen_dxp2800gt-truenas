# Upstream source

`ug_201x.c` and `leds-sio-201x.c` in this directory are unmodified copies of
UGREEN's GPL kernel sources for the IT8987-based Super I/O chip used on the
DXP2800GT (fan/PWM control, temperature sensors and front-panel LEDs).

- Upstream repository: https://github.com/ugreen-opensource/kernel-6.12/tree/main/drivers/ugreen
- Files retrieved from:
  - https://raw.githubusercontent.com/ugreen-opensource/kernel-6.12/refs/heads/main/drivers/ugreen/ug_201x.c
  - https://raw.githubusercontent.com/ugreen-opensource/kernel-6.12/refs/heads/main/drivers/ugreen/leds-sio-201x.c
- Upstream commit at time of retrieval: `0e4d73a6893d2aaa9772345c85bcccb3798784c1` ("init commit linux 6.12", 2026-04-03) — the only commit that has ever touched these two files.
- Verified byte-identical against upstream on 2026-09-12.

These files are kept here **only as a pristine reference** and are not built
directly. `../ug_201x-hwmon.patch` adds a standard Linux `hwmon` interface on
top of `ug_201x.c` (see `../ug_201x-hwmon.c` for the already-patched result
that is actually compiled — see [INSTALL.md](../../INSTALL.md)).
`leds-sio-201x.c` is used as-is, unmodified, for the LED classdev registration.
