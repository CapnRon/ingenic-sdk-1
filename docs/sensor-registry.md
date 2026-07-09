# The sensor registry: `/proc/jz/sensor` without per-driver code

## What this replaces

Until now, every sensor driver that wanted to appear under `/proc/jz/sensor`
carried a hand-written `static struct sensor_info` block, calls to
`sensor_common_init()/_exit()` (and on some platforms `_update()` or
`sensor_update_actual_fps()`), and a copy of `sensor-info.c` linked into its
module. That approach had structural problems that no amount of wiring PRs
could fix:

- **It never converged.** After years of "wire the remaining drivers" PRs,
  3.10 coverage was still partial (t23: 20/106, t30: 4/48, t40/t41: 0), and
  every vendor SDK re-import reset the treadmill.
- **The data was hand-copied and drifted.** Width/height/fps were typed
  literals duplicating the driver's own `sensor_win_sizes[]` table; several
  disagreed with it. `mclk = 1` was pasted into 94 files whether true or not.
- **Multi-sensor could not be represented.** One static struct per module,
  and a name-keyed proc directory, cannot describe two instances of the same
  sensor model — a real product configuration (dual sc2336).
- **Duplicate proc trees.** `sensor-info.c` was linked into *each* sensor
  module. On 3.10, procfs happily registers duplicate names, so two loaded
  sensor modules (or one old-style module plus anything else owning the
  directory) produce two `/proc/jz/sensor` entries, one shadowing the other.
- **Use-after-free on rmmod.** Proc entries were never removed; their fops
  pointed into unloaded module text. `sensor_common_exit()` was empty.
- **Stale values.** The static block was visible from insmod onward, showing
  the driver author's defaults rather than reality, and `width`/`height`
  never tracked the actually-configured mode.

## The design

One registry, compiled into each family's `tx-isp` module
(`<kver>/isp/common/tx-isp-sinfo.c`), owns `/proc/jz/sensor`. Sensor drivers
carry **zero registry code**. They are wired automatically by a small hook
block at the tail of each family's `sensor-common.h` — a header every sensor
driver already includes — which wraps the two calls every driver already
makes:

```
driver load   private_i2c_add_driver()   -> tx_isp_sinfo_driver_add()
              (i2c_add_driver on T10/T20)   name + default i2c address
driver probe  tx_isp_subdev_init()       -> tx_isp_sinfo_sensor_bind()
              (v4l2_i2c_subdev_init on      live struct tx_isp_sensor
               T10/T20)
```

`SENSOR_I2C_ADDRESS` is expanded at the driver's own call site, so each
driver's address flows through without the driver knowing. A driver without
that macro fails to compile — which is the enforcement mechanism for future
imports.

### Two-stage registration

Registration is two-stage because on these platforms the i2c client (and
therefore probe) does not exist until userspace calls `IMP_ISP_AddSensor` —
the ISP core creates the client from the register_info it is handed. A
streamer therefore needs the sensor's name and i2c address *before* any
probe has run:

- **Stage 1 — module load.** A slot appears as `/proc/jz/sensor/sensorN/`
  publishing `name` (from `drv->driver.name`), `i2c_addr` (from the
  driver's `SENSOR_I2C_ADDRESS`), `status = loaded`, and per-SoC
  board-wiring defaults where the family supports them.
- **Stage 2 — probe (inside AddSensor).** The slot binds the live
  `struct tx_isp_sensor`. From then on every read reports actual state:
  real client address and adapter, `attr->chip_id`, configured mbus
  geometry, current fps, the register_info values userspace actually
  passed. `status = active`. Unbind (remove/DelSensor) reverts the slot to
  stage 1; driver unload removes it.

### The honesty contract

A value that is not yet knowable reads as an **empty file**, never a
placeholder. Consumers treat unparseable/empty as "fall back to
config/defaults", whereas a fake `0` or `1` parses as valid and gets used.
A value that is present is either a driver constant (stage 1) or observed
truth (stage 2) — never a guess. `width`/`height`/`fps` mean *the currently
configured mode*: they are empty before the ISP opens the sensor and track
WDR/crop boots and runtime fps changes afterwards. (This subsumes the old
`actual_fps` feature, which required a per-driver call that 208 drivers
carried; the registry reads `sensor->video.fps` live instead.)

## The /proc ABI

```
/proc/jz/sensor/
  count                  number of registered sensors
  sensor0/               index-keyed: dual-same-model safe
    name chip_id i2c_addr i2c_adapter width height fps status
    min_fps max_fps                    (T41-class only)
    mclk boot video_interface          (T40/T41-class only)
    rst_gpio pwdn_gpio                 (T40/T41-class only)
  sensor1/ ...
```

`sensorN` is a **stable slot number, not a dense index**. A slot is claimed
when a driver registers and released when it unregisters, so removing the
module behind `sensor0` while `sensor1` is still loaded leaves `sensor1` at
index 1 and reports `count = 1`. This is deliberate: renumbering the
survivor would silently change the identity of a sensor a consumer is
already talking to. Enumerate by globbing `sensorN/`, not by iterating
`0..count-1`.

Per-family key sets follow the structs: T31-class register_info has no
wiring fields, so those files simply do not exist there; `min_fps`/`max_fps`
live in `tx_isp_video_in` only on T41. Board-wiring defaults (mclk 1,
i2c adapter, boot 0, interface 0) are per-SoC constants in the registry —
they are a board property, not a sensor property, which is why hand-pasting
them into 700 drivers was always wrong.

There is deliberately **no flat-file compatibility layer**: the old flat
paths could only ever describe one sensor. Known consumers were updated in
lockstep (thingino `sensor` CLI, webui CGIs, raptor's rvd, whose procfs
fallback is now per-index — giving multi-sensor zero-config for free).

## Family support matrix

| family | kver | core | subdev flavor | keys | status |
|---|---|---|---|---|---|
| t20 (t10 is a symlink) | 3.10 | open | v4l2 | 8 | HW: Wyze V2 / jxf23, zero-config |
| t21 | 3.10 | blob | tx-isp | 8 | compile-gated |
| t23 | 3.10 | blob | tx-isp | 8 | HW: Cinnado D1 / sc2336, zero-config; dual firmware with 2 slots |
| t30 | 3.10 | open | tx-isp | 8 | HW: Wyze VDB1 / sc4236, full gauntlet |
| t31 | 3.10 | blob | tx-isp | 8 | HW: Z55 / gc4653 |
| t41 | 3.10 | blob | tx-isp | 15 | hooked, unverified (no profile targets it) |
| t31 (c100 is a symlink) | 4.4 | blob | tx-isp | 8 | hooked; reached only via the single c100 profile, not yet compile-gated |
| t40 | 4.4 | blob | tx-isp | 13 | compile-gated (imx307) |
| t41 | 4.4 | blob | tx-isp | 15 | HW: T41NQ / gc4023, zero-config; 2 slots + simultaneous binds |

Not covered, and why: **t32/t33** have no working stack yet. **t41zrt** (both trees) and
**t40 on 3.10** ship sensor drivers but no ISP Kbuild or module wrapper, so no build
path reaches them; their drivers were swept but not hooked. **a1** has no ISP or
sensors. Every family that `build.sh` accepts and that can link a `tx-isp` module is
hooked.

Validated = stage-1 at insmod, stage-2 live under a streaming rvd,
teardown/reload cycles, and a 2000-read cat race against rmmod (the
use-after-free regression test — fops now live in tx-isp, which sensor
modules symbol-depend on, so the kernel's refcounting makes the old crash
structurally impossible).

## What was removed

Commit-series summary: `sensor-info.c` and `sensor-info.h` deleted from both
kernel trees; 582 driver files swept of 581 includes, 411 static blocks,
~800 init/exit/update calls, 776 `sensor_update_actual_fps` calls and 252
runtime `sensor_info.*` assignments (net −8,578 lines). 82 drivers that
carried their i2c address only as a `.cbus_device` literal gained a
`SENSOR_I2C_ADDRESS` define single-sourced from that value.

The sweep is not cosmetic: because 3.10 procfs registers duplicate names,
any unswept driver's `sensor_common_init()` creates a second
`/proc/jz/sensor` directory that shadows the registry (observed on t23).
Old-style and new-style must not coexist within an image.

## Porting notes for a new family

1. Add `$(KERNEL_VERSION)/isp/common/tx-isp-sinfo.c` to the family's isp
   Kbuild and call `tx_isp_sinfo_init()/_exit()` from the module wrapper
   (or the open core's `tx_isp_init/exit`).
2. Append the hook block to the family's `sensor-common.h` (copy from t31;
   T10/T20-style v4l2 cores use the v4l2 variant from t20).
3. If the family's structs differ, extend the `SINFO_HAVE_*` guards in
   `tx-isp-sinfo.c` — key presence is decided at compile time per family.
4. Sensors need `SENSOR_I2C_ADDRESS`; nothing else.
