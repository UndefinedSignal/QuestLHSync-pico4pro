# QuestLHSync on the PICO 4 Pro

An unofficial community port of QuestLHSync to the **PICO 4 Pro** (A8110). It
keeps SteamVR's lighthouse trackers, controllers and base stations lined up
with the PICO's own inside-out tracking, with no tracker on your head and no
SpaceCalibrator. The PC side is QuestLHSync's SteamVR driver with PICO support
added. The headset side is a Magisk module that reads the PICO's tracking
cameras.

**Status: experimental.** Tested on one PICO 4 Pro (PICO OS 5.13.3S, streamed
with Virtual Desktop) in a room with three 2.0 base stations. In that room it
acquired in 1-2 minutes. It then held for 10+ minutes with a 0.2-0.4 deg median
sighting error, about 1 cm and 0.15 deg of wander, and it picked up its saved
alignment again after a restart. The PICO 4 (non-Pro) and other OS builds are
untested.

## Requirements

- A **PICO 4 Pro rooted with [Magisk](https://github.com/topjohnwu/Magisk)**.
  Rooting means unlocking the bootloader, for example with
  [more-picohaxx-tool](https://github.com/chaixshot/more-picohaxx-tool).
  It can brick the headset and void its warranty. Back up first.
- **SteamVR base stations** (tested with 2.0) and at least one lighthouse device
  switched on: a tracker or an Index controller.
- **Windows with SteamVR**, the PICO streamed to it (tested with Virtual
  Desktop), both on the **same local network**.
- To build: Python 3 with `pip install ziglang` for the module. Visual Studio
  2022 or newer with C++ for the driver.

## Install

### 1. Headset module

Build it, or take `QuestLHSync-pico-module-<version>.zip` from this fork's
releases when there is one:

```
pip install ziglang
python pico/build_module.py        -> out/QuestLHSync-pico-module-<version>.zip
```

Install the zip in the Magisk app (**Modules > Install from storage**) and
reboot. Or over adb:

```
adb push out/QuestLHSync-pico-module-<version>.zip /sdcard/Download/
adb shell su -c "magisk --install-module /sdcard/Download/QuestLHSync-pico-module-<version>.zip"
adb reboot
```

The installer checks that it's a PICO 4 Pro and that the camera calibration is
in `/mnt/vendor/persist/pvr/mav0`. After the reboot `lhsyncd` waits for the PC
on TCP 47280, with discovery on UDP 47281.

### 2. PC driver

In a clone of this fork (branch `pico-4-pro`), with SteamVR closed:

```
build.bat
powershell -executionpolicy bypass -file pico\register_driver.ps1
```

`register_driver.ps1` finds SteamVR's `vrpathreg.exe` itself, whichever Steam
library it is in. `-remove` unregisters the driver. If PowerShell drops capital
letters or `$` when you paste a command, type it instead: the commands and
paths are case-insensitive.

### 3. Turn off SpaceCalibrator

Also turn off OpenVR-SpaceSync and anything else that moves lighthouse devices.

## First run

1. Start SteamVR with the PICO streaming. On the QuestLHSync dashboard page you
   should see the headset (`A8110 at <ip>: connected` in the log) and its
   cameras.
2. Wear your trackers. They tell the right alignment from its mirror image.
3. Stand in your play space and **slowly look at each base station in turn**,
   for 20-30 s each. Take a step now and then. The log says
   `acquired: yaw ... support ...` when it has the alignment.
4. It saves the alignment per headset
   (`%LOCALAPPDATA%\QuestLHSync\headsets\A8110-...\state.json`). The next start
   picks it up in seconds (`seeded from saved stations`).

Why slowly: the PICO's cameras catch a base station's laser only a few times a
minute per station, many times fewer than a Quest's. So the PICO acquires from
the last 5 minutes of sightings instead of 1. Sightings while the head is still,
or turns faster than 100 deg/s, are skipped.

## What's different from the Quest

- **Cameras.** All four tracking cameras (OV9282, global shutter, 640x480) are
  read inside `pvrtrackingservice` with Frida, from its ring of 32 dmabufs (8
  per camera). Each frame's footer carries its exposure time and frame number,
  so frames are timed exactly, with no polling lag. The cameras run at 50 fps,
  alternating long and short exposures. Only the short ones are searched for
  laser dots.
- **Calibration.** The PICO's own factory calibration (Kalibr `omni` +
  `radtan`, camera-to-IMU extrinsics) is fitted to the driver's Fisheye62 model,
  at 0.03-0.07 px RMS.
- **Steady lights.** The PICO's short frames saturate small LED lamps just like
  the lasers. A spot is taken for a lamp if it was there in the camera's
  previous short frame, or if it starts a run of three, because a laser hardly
  ever shows twice in a row. The lamps' sightings are kept as evidence: a fit
  that puts a base station where they point counts no support for that
  station. With lamps on, acquiring takes longer (about 3 minutes in the test
  room). Switching bright spot lights off speeds it up.
- **Camera map.** `cammap.txt` in the module folder maps the four buffer rings
  to camera ids (default `3,2,1,0`, found by covering the cameras one at a
  time). An update keeps a map you changed.

## Troubleshooting

- **Driver log:** `%LOCALAPPDATA%\QuestLHSync\questlhsync.log`. Headset log:
  `/dev/.questlhsync/lhsyncd.log` (`adb shell su -c cat /dev/.questlhsync/lhsyncd.log`).
- **Never acquires:** look at the `acquiring, last 60 s: ...` lines.
  - Mostly `steady lights`: lamps.
  - Mostly `while it turned fast` or `while the headset was still`: move slower,
    or keep moving.
  - Few bright spots at all: face the stations.
- **Record a session** (dashboard: **Record session**) and attach
  `%LOCALAPPDATA%\QuestLHSync\recordings\qlhs-*.txt` to an issue.
  `build_replay.bat` builds `qlhs_replay`, which runs a recording through the
  driver offline. A recording holds your base stations' and devices' serial
  numbers and positions, but no images.

## Not done yet

- The head origin in the IMU frame (`kPicoHeadInImu`) is an estimate. An error
  in it shifts the alignment by about as much, a few centimetres at most.
- The timing (exposure to SteamVR pose) is learned only from sightings taken
  while the head turns fast. Those are rare on the PICO, so it usually stays at
  the 25 ms default. That is fine at ordinary head speeds.
- No installer and no release builds for the PICO yet.

## Credits

QuestLHSync is by [CreoleVR](https://github.com/CreoleVR/QuestLHSync). This port
only adds the PICO side and the few driver changes it needed.
