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
driver IMU ── 1600 Hz rate thread ── control_imu ── 1000 Hz navigation eFMU
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
this airframe streams UBX, so `gnss-nmea-generic` cannot read it. The reader is
receive-only — it sends the module nothing and configures nothing — so it works
at whatever output rate the receiver happens to be set to. `current-speed` on
`lpuart2` in `boards/mr_vmu_tropic.overlay` must still match the receiver's
actual serial rate; it is set to 115200, confirmed against the hardware by a
port dump returning `b5 62 01 07`, a UBX NAV-PVT header.

NAV-PVT alone carries every field the `GnssFix` contract wants, including the
accuracy estimates NMEA has no way to express, so horizontal, vertical and
velocity accuracy are published as the receiver's own figures rather than the
65535 "unusable" sentinel, and vertical velocity arrives with its validity bit
set. Course over ground is marked valid only above 150 mm/s of ground speed,
below which the receiver's heading of motion is noise rather than a course, and
the UTC timestamp only with both validDate and validTime and a fix, since a
timestamp that silently stops advancing is worse for a consumer than none.
Receiver yaw is absent with its validity bit clear — NAV-PVT does not carry it.

See `docs/ground_station_telemetry.md` for the serial wire contract.

## Which bus carries what

ZROS is the internal bus. Every producer on the vehicle publishes there, every
consumer reads there, and it is the only bus a subsystem needs to know about.

CSyn is the external face for Ethernet: it carries the same topics to Zenoh
over the network stack, and `csyn_zros_bridge` in the CSyn module mirrors them
between the two buses. Nothing on the vehicle publishes into CSyn directly.

The SiK telemetry radio is served by `subsys/zros_serial`, which reads and
publishes ZROS topics and does not link CSyn at all. What it does share with
CSyn is the `synapse_fbs` catalog: the `topic_id` in each frame is a catalog
TopicId, because the ground-side peer decodes by that id. That is schema, not
transport, so both links can carry the same topics without either depending on
the other.

GNSS is the one topic that lives only on ZROS. The radio carries it in both
directions, so nothing mirrors it onto CSyn and a fix does not appear on the
Ethernet/Zenoh side.

RDD2 uses the same pinned CSyn module as CUBS2. CSyn owns the `synapse_fbs`
release, generated C headers, topic catalog, canonical Zenoh keys, payload
sizes, and transport bridge; RDD2 does not carry a second schema-fetch or
decoder path.

Deterministic lockstep never uses CSyn, ZROS bridging, Zenoh, or Ethernet for
pacing. `native_sim` and FastDyn select direct shared-memory backends in the
same `subsys/lockstep` module and exchange only generated `synapse_fbs`
payloads. FastDyn resolves the shared block from the ELF instead of relying on
a fixed firmware address. The normal Ethernet stack remains available, and a
lockstep communications build may enable CSyn/ZROS plus Zenoh as an
asynchronous side-channel without changing the direct lockstep coordinator.
Performance builds may omit the unused network stack; communications builds
retain ENET and enable CSyn/Zenoh independently of lockstep pacing.

CMake installs the Rumoca release pinned by `cmake/RumocaLock.cmake` into the
build tree, verifies the installer, executable version, and platform binary
hash, and generates eFMI Production Code from
`Planning.Bezier.WaypointTrajectoryPlanner`,
`Vehicles.Rdd2.NavigationEstimator`, `Vehicles.Rdd2.GuidanceController`, and
`Vehicles.Rdd2.RateControlAllocator` in the `modelica_models` West project under
`${CMAKE_BINARY_DIR}/generated/rumoca`. The reusable quadrotor plant, RDD2
parameters, task composition, and model-level qualification mission all remain in
that common project. Generated C and `.efmu` containers are build outputs, not
committed source.

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
source .venv/activate/bin
pip install west
west init -l cerebri_rdd2
west update
west packages pip --install
west sdk install -t arm-zephyr-eabi
west build -p -b mr_vmu_tropic cerebri_rdd2
```

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
nix run .#build-native-sim
nix run .#console
nix run .#systemview
nix run .#systemview-capture
nix run .#trajectory-compare
nix run .#flash
```

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
