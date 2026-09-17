# RDD2

`rdd2` is the active multirotor platform folder in this repository.

Start with [spec/README.md](spec/README.md)
for platform rules.

V1 goals:

- `mr_vmu_tropic` only
- CRSF input only
- DSHOT output only
- ICM45686 IMU only
- one uninterrupted IMU → rate eFMU → DSHOT hot-path thread
- no dependency on the legacy `cerebri` module

Current implementation scope:

- CEP-0002 platform layout under `rdd2/`
- local FlexIO DSHOT driver vendored into this repo
- four Rumoca eFMUs generated into the build tree and deployed through four
  explicit process adapters
- planning -> guidance -> rate/allocation follows the periods and connections
  in `Vehicles.Rdd2.AvionicsSystem`
- the navigation estimator follows the 1 ms contract in
  `Avionics.PartialNavigationEstimator`
- `ACRO` and `AUTO_LEVEL` manual flight modes
- GNSS on the `gnss_fix` topic from either the onboard M10 read as UBX or a
  fix injected over the telemetry radio

## Repository layout

| Path | Ownership |
|---|---|
| `src/` | eFMU processes, their interfaces, and the composition root |
| `subsys/` | Zephyr lockstep, GNSS, and serial communication subsystems |
| `drivers/`, `include/`, `dts/` | Local Zephyr drivers, public driver API, and devicetree bindings |
| `boards/` | Board-specific Kconfig fragments and devicetree overlays |
| `snippets/` | Optional standard Zephyr build profiles such as `-S mocap-gnss` |
| `fastdyn/` | FastDyn firmware and rehosting configuration only |
| `xtask/` | Reproducible Rust host commands, FastDyn mission host, and host tests |
| `cmake/`, `nix/` | Build-tool and Nix/NixOS integration |
| `spec/`, `docs/` | Design requirements and operator documentation |
| `.github/`, `.cargo/`, `.vscode/` | CI, Cargo command aliases, and editor configuration |

There are no generic top-level `tests/`, `test_scripts/`, `scripts/`, or
`tools/` buckets; executable host workflows belong in `xtask/`.

## The architecture at a glance

The Modelica objects are the application. Zephyr schedules one process per
eFMU and maps its driver and ZROS boundaries:

```text
driver IMU ── 800 Hz rate thread ── control_imu ── 800 Hz navigation eFMU
                    ↑                                    │
                    │                         navigation_odometry + attitude
                    │                                    │
                    │       50 Hz planning eFMU ── trajectory_reference
                    │                                    │
                    └──── RateCommandData ── 200 Hz guidance eFMU
                    │
                    └──── RateControlAllocator eFMU ── direct DSHOT driver
```

The rate process owns Zephyr's calling/main thread. It waits directly on the
IMU data-ready source, advances `RateControlAllocator`, and writes DSHOT without
a scheduler handoff. Navigation, planning, and guidance each own a worker
thread. On coincident releases, priority order is rate, navigation, planning,
then guidance, so guidance sees the newly published plan just as it does in the
phase-zero ideal RTOS composition. Cross-thread values are bounded,
latest-value ZROS topics; there are no control queues.

See [`src/processes/README.md`](src/processes/README.md) for the complete
process/interface table. Controller equations remain vector and matrix
equations in Modelica; scalar lowering is a compiler concern, not handwritten
C.

## Choosing the GNSS source

One `gnss_fix` topic on the internal ZROS bus, one 64-byte catalog contract,
two possible producers. Whichever is selected is the topic's single registered
publisher, so the two are mutually exclusive by construction and a fix can
never have two origins:

| Source | Publisher | Radio direction | How |
|---|---|---|---|
| Onboard M10 read as UBX | `subsys/gnss_source` | outbound telemetry | default |
| Injected over the telemetry radio | the serial transport | inbound | `-S mocap-gnss` |

Consumers read the fix through `rdd2_topic_gnss_copy()` and cannot tell which
filled it. The default follows the devicetree: an enabled `gnss` node selects
the onboard source, and disabling it falls back to injection, so the node
status and the Kconfig choice cannot disagree. `zros topic echo gnss_fix`
shows the live fix whichever way it arrived.

`mocap-gnss` is the indoor configuration — position comes from an external
Synapse-compatible bridge over the radio, and the onboard driver is left out
entirely so `lpuart2` stays free.

The onboard reader lives in `subsys/gnss_source` and decodes UBX-NAV-PVT
directly rather than going through Zephyr's generic NMEA driver: the M10 on
this airframe streams UBX, so `gnss-nmea-generic` cannot read it. At boot the
reader sends one RAM-layer UBX-CFG-VALSET request that enables UBX input/output
on receiver UART1, disables NMEA input/output, selects airborne-2g dynamics,
and emits NAV-PVT at 10 Hz. The whole configuration is one transaction so all
three bounded retries are byte-identical and an ACK cannot ambiguously advance
part of the configuration. Each NAK or 1.1-second ACK timeout retries; three
unsuccessful attempts fail closed.
`current-speed` on `lpuart2` in `boards/mr_vmu_tropic.overlay` is 115200, which
was confirmed against the hardware by a port dump returning `b5 62 01 07`, a
UBX NAV-PVT header.

The onboard source reports a usable fix only after the configuration ACK and
five consecutive accepted NAV-PVT samples with 75--125 ms gaps. The latest
sample must be no more than 300 ms old, carry a 3D/DGNSS/RTK fix, and report
positive horizontal, vertical, and velocity accuracy within 10 m, 15 m, and
5 m/s respectively. Until every gate holds, after any gate drops, or when a
topic update fails, it publishes or retries `NoFix` with 65535 accuracy/DOP
sentinels. `gnss status` reports the configuration, stream, fix, accuracy, age,
rate, gap, and error evidence used by those gates.

NAV-PVT alone carries every field the `GnssFix` contract wants, including the
accuracy estimates NMEA has no way to express, so horizontal, vertical and
velocity accuracy are published as the receiver's own figures rather than the
65535 "unusable" sentinel once ready, and vertical velocity arrives with its
validity bit set. Course over ground is marked valid only above 150 mm/s of
ground speed, below which the receiver's heading of motion is noise rather than
a course, and the UTC timestamp only with both validDate and validTime and a
fix, since a timestamp that silently stops advancing is worse for a consumer
than none. Receiver yaw is absent with its validity bit clear — NAV-PVT does not
carry it.

See `docs/ground_station_telemetry.md` for the serial wire contract.

## Which bus carries what

ZROS is the internal bus. Every producer publishes there, every local consumer
reads there, and application subsystems do not depend on a network transport.

`synapse_fbs` owns generated topic IDs, keys, fixed payload layouts, C
headers, and bounded codecs. RDD2 consumes one explicit generated C package.

`subsys/synapse_wire` receives the strict embedded UDP/IPv6 topic subset,
validates carrier and payload state, and publishes accepted samples onto the
existing ZROS topics. The current receive set is optical flow and GNSS.

The SiK telemetry radio remains a separate bounded carrier implemented by
`subsys/zros_serial`. It uses the same generated topic IDs without depending
on the Ethernet carrier.

ROS 2 CDR mirrors are observational transport projections. A deployment can
enable a generated CDR projection for any supported topic without adding that
topic to the strict direct-wire set.

Deterministic lockstep uses direct shared memory for pacing. `native_sim` and
FastDyn exchange generated `synapse_fbs` payloads through
`subsys/lockstep`. Host-side network mirrors may run concurrently, but never
coordinate or pace the control loop.

CMake checks the Rumoca executable the flake input supplies against the
revision pinned by `cmake/RumocaLock.cmake`; without one it installs that
revision with `cargo install` into `~/.cache/cerebri_rdd2/rumoca/<rev>`, reuses
that cache on later builds, verifies the executable version, and generates eFMI Production
Code from
`Planning.Bezier.WaypointTrajectoryPlanner`,
`Vehicles.Rdd2.NavigationEstimator`, `Vehicles.Rdd2.GuidanceController`, and
`Vehicles.Rdd2.RateControlAllocator` in the `modelica_models` West project under
`${CMAKE_BINARY_DIR}/generated/rumoca`. The reusable quadrotor plant, RDD2
parameters, task composition, and model-level qualification mission all remain in
that common project. Generated C and `.efmu` containers are build outputs, not
committed source.

## SD flight logging

`subsys/flight_log` records the internal ZROS flight bus to the onboard microSD
card as a self-contained `synapse/1` MCAP file, one session file per boot named
`flightNNNN.mcap`. It is off by default and is enabled with
`CONFIG_RDD2_FLIGHT_LOG=y`, which pulls in the usdhc block device, the FAT
filesystem, and the card-detect disk driver. The passive communications image
leaves it disabled so it stays lean.

Two threads and one bounded ring separate the flight bus from the card. A
capture thread (priority 4, between navigation and planning) subscribes to the
logged topics with per-topic rate limits and copies each accepted sample into a
128 KiB ring, never blocking the bus: when the ring is full it drops the frame
and counts it. A writer thread (priority 8, below every control thread) drains
the ring into the constant-memory MCAP writer, emits a `TimeReference` record
at 10 Hz plus logger-status and direct-wire-stats records at 1 Hz, and flushes
and syncs on a fixed cadence so a power loss costs at most one flush window plus
the at most 511 bytes the flush holds back to keep the file offset sector-aligned
(the sub-sector tail goes out with the next flush, or in full at close).

The card is mounted on first use with its existing FAT volume. An absent or
unformatted card is a clean no-op that never blocks bring-up and never wipes a
card that holds anything: logging simply stays off, the `Logging` health bit
stays clear, and a low-rate retry starts a session if a card is inserted after
boot. Size-based rotation opens the next index at
`CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES` (256 MiB default; FAT32 and the 32-bit
FatFs file size cap a single file at 4095 MiB).

The card is brought up before anything else at boot. The writer thread starts
with no delay and raises itself to priority 1 for its first mount attempt, so the
roughly 1.3 s of card identification (and, on a blank card, the format) runs
ahead of main and the flight threads; it drops back to its normal priority 8
before the session opens. On the bench that puts the mount about 1.25 s after the
kernel banner and the open session about 1.14 s after the mount, against 2.5 s
and a further second before. Everything the open needs about the card comes from
one directory pass, so a boot is one directory read, one rename, and one file
open.

Card preparation matters for sync latency. What stalls an `fsync` on a consumer
card is the controller's own garbage collection, triggered when a write lands
across two of its internal allocation units or the card has to read-modify-write
a partly used one. The target geometry keeps that out of the write path: FAT32
with 32 KiB clusters (the largest cluster every host FAT32 tool accepts), a data
area aligned to 4 MiB so each cluster write stays inside one allocation unit, two
FATs, and no partition table. A mounted FAT volume whose root directory holds no
content is reformatted to that geometry automatically when its own geometry
differs, and the format erases every free cluster afterwards, so a formatted
card starts out as known-erased space. Only one entry does not count as content:
the `System Volume Information` folder, which Windows creates on every insert
and which holds nothing but indexing metadata, so a card blanked on Windows
still auto-formats. A card with any other file or folder is never reformatted:
the mismatch is logged once at mount and the card is used as it is. `sd format`
does the same thing from the bench on an empty card and `sd format force`
formats a card that has files on it, both still refusing while a session is
active; `sd info` reports the cluster size and whether the geometry matches.

Host FAT drivers do not trim SD cards, so a card whose flights were copied off
and deleted on a host shows free clusters to FatFs while the controller still
holds those blocks as live data, and streaming into that space is exactly what
makes it erase and collect garbage under the writer. What the logger erases is
therefore the space it is about to write, as it reserves it: the session extent
right after its `f_expand`, and each reservation growth step right after the step
lands, both by walking that file's own cluster chain in the FAT and erasing its
clusters. Written space is always erased space, a reservation renamed into a
session is erased by construction, and boot does no FAT scan at all. Freeing space
writes no erase at all: the close-time truncate of an unused reservation tail,
the unlink of a reservation, and a host deleting flights only return clusters to
the FAT, so that space is erased again when it is next reserved, and a `flightlog
stop` or `flightlog rotate` therefore never stalls the writer on an erase. `sd
trim` remains the manual full pass over every free cluster on the card, for a
card that is about to be filled or one whose history is unknown; it runs after
`flightlog stop`, logs the size it is about to erase and its running total every
1 GiB, and takes minutes on a large card. To prepare a card off-vehicle on a
Linux host, the equivalent is:

```sh
sudo blkdiscard /dev/sdX
sudo mkfs.fat -F 32 -s 64 -S 512 -R 32 -n FLIGHT /dev/sdX
```

Note the whole device and no partition table: that matches the superfloppy layout
the vehicle writes.

Each session file holds a preallocated extent so the steady-state writes fill
reserved clusters in place and update no FAT allocation table. A card removed
mid-flight then no longer risks the periodic cluster-growth metadata writes that
ran several times a second before. A session on a card that carries no standing
reservation reserves its extent inline with a native `f_expand` (enabled through
`CONFIG_FS_FATFS_EXTRA_NATIVE_API`), sized to the rotation size or to the free
space above a small reserve on a tighter card. That open-time reservation scans
for a free run and writes the whole cluster chain to both FAT copies at once, a
burst of roughly a second. The reservation is sized to the largest run of
consecutive free clusters the card actually has, found by one pass over the FAT
before the `f_expand`, so a fragmented card is given a smaller contiguous extent
instead of a reservation it cannot satisfy. That matters because a grow-on-write
session interleaves its clusters with the pool's growth steps and
leaves the free space in pieces, so each fallback makes the next one likelier;
copying the flights off and running `sd format` restores a clean layout. If the
`f_expand` still finds no run that large, that answer cannot change while the
logger runs, so it is asked once per mount, logged once, and every later session
goes straight to grow-on-write until the card is remounted. At boot it lands
before arming and is harmless, but a mid-flight rotation (hours of streaming
apart at the measured flight data rate) cannot afford it: the burst would stall
the writer long enough to overflow the capture ring and drop frames.

So the extents are built ahead of time and kept standing on the card as a pool
of reservations: about `CONFIG_RDD2_FLIGHT_LOG_RESERVE_BYTES` (4 GiB default) of
pre-built, pre-erased space, which at the 256 MiB rotation size is 16 files,
`reserve00.pre` through `reserve15.pre`. While a session streams, the writer
grows the first free slot toward the rotation size in steps of
`CONFIG_RDD2_FLIGHT_LOG_PREALLOC_STEP_BYTES` (8 MiB default), one step per flush
cycle between drain batches under the card lock. Each step stretches that file's
cluster chain over a newly seeked range with the FatFs write-mode `f_lseek`
idiom, writing only a few FAT sectors and no file data, so it blocks draining
only briefly. At the 400 ms flush cadence the 32 steps of one 256 MiB reservation
complete in about 13 seconds on the bench (about 20 s with the per-step erase),
and the whole pool in a few minutes.

Only one is built at a time, and only when the pool is short: after four sessions
have consumed four reservations, twelve remain, and the writer builds back toward
sixteen, one reservation at a time in the background, whenever free space allows,
so the next boot always finds a ready reservation and opens by rename. A card too
small for the full pool simply keeps as many as fit: the build that would not fit
stops at the free-space gate, logs one line, and the pool stands at what it
reached (a 4 GB card holds the 256 MiB session and 13 reservations).

When the size threshold trips, rotation renames the lowest ready reservation into
the next `flightNNNN.mcap` index. FatFs rename is a directory-entry operation:
the new name inherits the pre-built cluster chain untouched, so the swap runs no
`f_expand` and no free-extent scan and the writer never stalls. The outgoing file
filled its whole reservation, so it is closed without truncation, and the
incoming file is reopened without `O_TRUNC` (which would free the pre-built
chain) and streamed from offset 0 over the reserved clusters. `sd ls` shows the
pool, and the slot being rebuilt climbing toward the rotation size, while a
session records.

If no reservation is ready at rotation, an early manual `flightlog rotate` on a
fresh card, or a card too full to hold a live session plus one more reservation,
rotation falls back to the inline `f_expand` path with its known burst and logs
one warning. A manual rotate of a session far short of the
rotation size also pays a one-time truncate burst (freeing the unused tail of
the reservation walks the same amount of allocation table as building it,
roughly a second with counted ring drops). This is inherent to reclaiming the
space and acceptable because manual rotation is a stationary bench and
retrieval action. The size-triggered rotation in flight closes a file that
consumed its whole reservation, so it truncates nothing and swaps to the
prepared reservation with no burst. Stop truncates the active file back to the
streamed byte count and leaves the whole pool in place: those are the standing
reservations for the next boot, so the first session after a restart opens by the
same rename and no boot after the first one ever scans the FAT for a free run. A
reservation still being built at stop is removed instead, because a partial chain
left by a card yank cannot be trusted and rebuilding one is cheap; a partial file
left in a slot by a reset is removed the next time the builder reaches that slot.
The pool persists across runs on purpose and shows up as `reserve00.pre` and up
when the card is read on a host; deleting them there is harmless and costs only
rebuilds, one inline reservation at worst on the next boot. The names are ignored
by the session-index scan and live under the `/SD:` mount, so the mcumgr
file-access confinement still covers them.

While a preallocated extent streams, the flush no longer calls `f_sync` at all.
The directory entry already carries the reserved size, the timestamp is a fixed
constant with no RTC, the chain is fully built, and the flush hands FatFs only
whole sectors, so FatFs holds no dirty file data and an `f_sync` would do nothing
but rewrite an unchanged directory sector every 400 ms. The flush instead issues
a disk sync, which waits for the card to finish the last write, the only
durability step left. Close and rotate still go through `f_sync` and `f_close`,
which is where the true file size lands; the grow-on-write fallback keeps
`f_sync` per flush because its FAT chain really does change.

The residual surprise-removal exposure of an active session is the
up-to-one-flush-window (400 ms) of unsynced data at the tail, and card-internal
remapping the host cannot see. The active file's chain is fully built, so
streaming writes no FAT allocation metadata: the only FAT-allocation writes
during a session are the pool growth steps, and those land entirely on the
disposable file being built. A card pulled during a growth step can leave that
file's chain inconsistent, but never the recording, whose clusters are all
already reserved.
If the card is pulled mid-session, the stale full-size directory entry with a
garbage tail is harmless because the MCAP reader stops at the first invalid
record.

Bench operators use two shell command groups. `sd` mounts, unmounts, lists,
reports free space and geometry, formats an empty card (`sd format`), and trims
the free space (`sd trim`).
`flightlog` shows logger state and counters (`flightlog status`, including the
last and worst flush sync latency as `sync_last_ms` and `sync_max_ms`, and the
pool as `reservations=<ready>/<slots> build=<idle|building|given-up>`) and
controls sessions (`flightlog start`, `flightlog stop`, `flightlog rotate`). The
`flightlog` name avoids the Zephyr logging subsystem's own `log` command.
`sd unmount` is refused while a session is open, so stop logging before pulling
the volume.

FatFs is built non-reentrant on this target, so a single card lock serializes
every in-process filesystem access: the writer batch and every `sd`/`flightlog`
command that reaches the volume. The mcumgr retrieval path cannot hold that lock,
so it is gated differently. Session files are retrieved over the network with the
mcumgr filesystem group, restricted by a file-access hook to the `/SD:` mount
point. That same hook refuses file access with a busy error while a logging
session is open, because the mcumgr read happens later in the mcumgr handler
where the card lock cannot cover it. To retrieve a file, run `flightlog stop`
first (or `flightlog rotate` to close the current file and continue a new one),
then download the closed files. Do not restart logging while a download is in
progress, since the mcumgr download keeps the file open across chunks and only
re-checks the hook when the target changes.

## Raw Zephyr Build 

To bootstrap a fresh minimal workspace from this repo's manifest, you must first
install Zephyr's dependencies to the [getting started guide]
(https://docs.zephyrproject.org/latest/develop/getting_started/index.html), 
then check out this repo at `<workspace>/cerebri_rdd2` and initialize west from the
workspace root:

```sh
sudo apt-get install --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget \
  python3-dev python3-pip python3-setuptools python3-tk python3-wheel xz-utils file \
  make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1
mkdir -p /tmp/cerebri-ws
git clone <repo-url> /tmp/cerebri-ws/cerebri_rdd2
cd /tmp/cerebri-ws
python -m venv .venv
source .venv/bin/activate
pip install west
west init -l cerebri_rdd2
west update
west packages pip --install
west sdk install -t arm-zephyr-eabi
cerebri_rdd2/scripts/synapse_fbs_package
west build -p -b mr_vmu_tropic cerebri_rdd2
```

`west sdk install` registers the SDK in the CMake package registry
(`~/.cmake/packages/Zephyr-sdk`), so the build finds it without
`ZEPHYR_SDK_INSTALL_DIR`. An SDK installed by hand is registered by running its
`setup.sh -c` once.

`scripts/synapse_fbs_package` run from the workspace root writes the generated
Synapse C package to `<workspace>/synapse_fbs-build/synapse_fbs-c`, which the
build picks up as the default `RDD2_SYNAPSE_FBS_ROOT`. Pass a different output
directory to the script and export `RDD2_SYNAPSE_FBS_ROOT` to override it.

The first build compiles the pinned Rumoca revision from source, so `cargo`
(rustup's default location or `CARGO_HOME`) must be installed; the result is
cached in `~/.cache/cerebri_rdd2/rumoca/<rev>` and reused by later builds,
including `west build -p`.

## Nix / NixOS

This repo includes a flake for repeatable Zephyr host tooling on NixOS and
other Linux systems with Nix:

> First-time Nix setup:
>Install Nix using the install script from nixos.org https://nixos.org/download/
> 
>Add a config file:
>```sh
>mkdir -p ~/.config/nix
>```

Allow experimental features needed to run the next commands:

```sh
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Build the workspace:

```sh
nix develop
rdd2-west-update
rdd2-build
```

### Editor completion

`nix develop` includes `clangd` and links the native Zephyr compilation
database to `./compile_commands.json`. Run `rdd2-build-native-sim` once to
create or refresh it, then start either editor from that shell:

```sh
nvim .
# or
code .
```

Neovim's clangd client discovers the root database automatically. The checked-in
VS Code settings configure both clangd and the Microsoft C/C++ extension to use
the same database; the workspace recommends the clangd extension. Set
`RDD2_COMPILE_COMMANDS` to a specific database file before `nix develop` to
select a non-default build, or set `RDD2_BUILD_DIR` to its build directory.
The native database is the default because clangd understands its host-compiler
flags while still receiving all Zephyr, generated devicetree, module, and eFMU
include paths.
Re-run the appropriate build after changing boards, snippets, or Kconfig so
generated Zephyr and devicetree include paths stay current.

Host-side development commands use the checked-in Cargo xtask workspace:

```sh
cargo xtask fmt --check
cargo xtask fmt
cargo test --workspace --locked
rdd2-fastdyn-setup
rdd2-fastdyn-ci
rdd2-fastdyn-mission --help
```

The `rdd2-fastdyn-*` commands are provided by `nix develop` and configure the
repository and West workspace paths. `rdd2-fastdyn-ci` is the top-level,
incremental target: it updates a missing or stale managed West workspace,
builds the dedicated firmware image, prepares a missing or stale FastDyn/QEMU
runtime, exports and compiles the FMI plant, builds the release mission host,
and flies the acceptance mission. `rdd2-fastdyn-setup` and
`rdd2-fastdyn-mission` remain useful lower-level targets. See
`docs/fastdyn.md` for the complete rehosted mission workflow.

On NixOS, add the SEGGER udev rule to the configuration of each development
host so logged-in users can access J-Link USB probes:

```nix
{
  services.udev.extraRules = ''
    SUBSYSTEM=="usb", ATTR{idVendor}=="1366", MODE="0660", GROUP="dialout", TAG+="uaccess"
  '';

  users.users.your-user.extraGroups = [ "dialout" ];
}
```

Run `sudo nixos-rebuild switch --flake .#your-host`, then reconnect the probe.
The `rdd2-flash` command detects an inaccessible J-Link and reports this setup
requirement before invoking West.

Common commands are also exposed as flake apps:

```sh
nix run .#west-update
nix run .#build
nix run .#build-comms-stub
nix run .#build-native-sim
nix run .#test-gps-lockstep
nix run .#console
nix run .#systemview
nix run .#systemview-capture
nix run .#trajectory-compare
nix run .#flash
```

### Non-flyable communications bench image

Build the dedicated ZROS/direct-wire communications image with one command:

```sh
nix run .#build-comms-stub
```

The command always performs a pristine Zephyr configure in
`build-mr_vmu_tropic-comms-stub` using `comms_stub.conf`. This profile bypasses
`src/efmi.cmake`; it does not invoke Rumoca or compile generated flight-control
containers. It keeps the real GNSS, IMU, RC, ZROS, direct-wire, serial
telemetry, and
shell paths, while publishing invalid navigation, permanently asserting
failsafe/disarmed health, and replacing motor output with a hard-zero publisher.
It is a bench image and must not be flown.

To flash the exact completed build without rebuilding it:

```sh
RDD2_BUILD_DIR="$PWD/build-mr_vmu_tropic-comms-stub" \
  nix run .#flash -- --skip-rebuild
```

After opening `nix run .#console`, use `stub`, `gnss status`, `zros_serial
status`, and the `zros topic` inspection commands to capture live generations,
timestamps, rates, and transport counters.

The minimum bench capture is:

```text
stub
gnss status
zros_serial status
wire status
zros topic list
zros topic echo gnss_fix
zros topic hz gnss_fix 10000
zros topic hz control_imu 2000
zros topic echo vehicle_health
zros topic echo pwm_signal_outputs
zros topic echo control_loop_metrics
top once
kernel thread stacks
```

Only one asynchronous `zros topic hz` measurement runs at a time; use
`zros topic stop` before starting the next one.

`rdd2-console` opens a serial console at 115200 baud using stable
`/dev/serial/by-id` names. When multiple adapters are connected, it asks which
one to use and remembers the selection. Run `rdd2-console --select` to choose
again, or override it with `--device PATH` and `--baud RATE`.

`rdd2-systemview` starts SEGGER SystemView with the MIMXRT1064 SWD settings
and reads the RTT control-block address from the current firmware ELF. Build
and flash the firmware first, then run `rdd2-systemview`. Set
`RDD2_JLINK_SERIAL` when more than one probe is connected and
`RDD2_JLINK_SPEED_KHZ` to override the default 4000 kHz SWD speed.

`rdd2-systemview-capture` starts an interactive recording. Accept the SFL
dialog, then press Enter in the terminal to start recording. Exercise the
system for as long as needed and press Enter again to stop, export, and close
SystemView. It writes a timestamped
`.SVDat` recording plus event and context CSV exports under `traces/`. Set
`RDD2_TRACE_DIR` to change the output directory.

`trajectory-compare` reads the pure Modelica mission log plus the canonical SIL
and BIL logs, renders full overlays under `artifacts/trajectory-comparison/`,
and exits nonzero when the vehicle-owned error budget is exceeded. Run the
three mission producers first. Set `RDD2_MODELICA_MODELS_ROOT` when the
Modelica checkout is not at the default West path; all repositories may live
independently.

The shell defaults to the `gnuarmemb` Zephyr toolchain for
`mr_vmu_tropic`. `rdd2-build-native-sim` overrides this to the host toolchain
for lockstep builds and uses `native_sim/native/64` by default to avoid multilib
requirements on NixOS. Set `RDD2_NATIVE_SIM_BOARD=native_sim` if you need
Zephyr's 32-bit native simulator variant. The Nix shell includes x86 multilib
host support on `x86_64-linux`, so raw `west build -b native_sim` also works.
Use separate build directories when switching boards:

```sh
west build -b mr_vmu_tropic -d build
west build -b native_sim/native/64 -d build-native_sim
west build -b native_sim -d build-native_sim32
```

The Nix helpers already keep the common build outputs separate:
`rdd2-build` defaults to `build-mr_vmu_tropic`, while
`rdd2-build-native-sim` defaults to `build-native_sim`.

The app assumes the west workspace layout documented above:
`<workspace>/cerebri_rdd2`, `<workspace>/zephyr`, and `<workspace>/modules`.

The Nix commands use an isolated RDD2 West workspace under
`.devenv/state/west/` by default. Set `RDD2_WEST_WORKSPACE=/path/to/workspace`
to choose its location explicitly; the selected workspace is governed only by
this repository's `west.yml`.

Important assumptions:

- RC channel map is AETR on CRSF channels 1-4, arm is channel 5, and flight
  mode is channel 6.
- Mixer order is the local default in `src/main.c` and must be verified against
  the airframe wiring before flight.
- Rumoca-generated control artifacts are build outputs and should not be
  committed or hand-edited; update the Modelica source and regenerate instead.
