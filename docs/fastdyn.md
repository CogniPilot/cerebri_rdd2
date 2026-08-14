# RDD2 binary-in-the-loop development

RDD2 owns its board configuration, rehosting configuration, lockstep adapter,
and mission acceptance test. FastDyn remains a generic QEMU rehosting runtime.
The firmware eFMI generator is the exact source-pinned Rumoca package in
`flake.nix`. Nix supplies its executable SHA-256 to every firmware and FastDyn
build, and CMake rejects a different binary even when its version text matches.

The repository-owned pieces are:

- `fastdyn/mr_vmu_tropic.toml`: rehosting and process configuration;
- `fastdyn/prj.conf` and `fastdyn/mr_vmu_tropic.overlay`: Zephyr build inputs;
- `fastdyn/comms.conf`: optional Ethernet/Zenoh side channel;
- `xtask`: compiled shared-memory and FMI 3 host;
- `rdd2-fastdyn-setup`: build the pinned FastDyn and patched QEMU runtime;
- `rdd2-fastdyn-ci`: Nix-shell convenience command for bounded mission
  orchestration, automatic prerequisite builds, and artifact checks;
- `rdd2-fastdyn-mission`: Nix-shell convenience command for invoking the FMI
  plant and firmware lockstep host directly.
- `rdd2-test-gps-lockstep`: mandatory native-firmware proof that GPS origin and
  odometry become observable before the one-shot mission reaches `PENDING`,
  followed by nine focused Zephyr suites including the real generated Guidance
  mode, hover-thrust, and altitude-response discriminator.

The convenience commands locate the application and its West workspace.
FastDyn CI requires a clean `modelica_models` checkout at the exact commit
pinned by `west.yml`; environment overrides do not replace that source
dependency. `rdd2-fastdyn-ci` is the make-like top-level entry point: it
synchronizes a missing or stale managed workspace and incrementally builds the
firmware, FastDyn runtime, FMI plant, and Rust mission host before running the
acceptance checks. Their low-level raw equivalents remain
`cargo xtask fastdyn-ci` and `cargo xtask fastdyn-mission`; those raw commands
assume their external artifacts already exist.

The mission host is designed to load the tensor-native
`Vehicles.Rdd2.Plant` FMI 3 Co-Simulation interface. It contains no
handwritten quadrotor equations. The same host and FMI plant are used for
host-native firmware and the rehosted ARM binary.

The plant retains checked parameter assertions. The required Rumoca revision
must execute those assertions in FMI initialization and before each step, while
preserving rollback on failure. The landing-contact branches are explicitly
`noEvent` and do not create zero crossings. Do not suppress the assertions to
make this test appear to pass; `fastdyn-ci` is the acceptance gate for the exact
pinned compiler, source FMU, and firmware combination.

## Standalone repository setup

The normal entry point is a single command:

```sh
nix run .#fastdyn-ci
```

It prepares and rebuilds prerequisites as needed. The commands below expose the
individual stages for debugging or development.

Initialize the isolated dependency workspace from this repository's West
manifest. This also fetches the FastDyn revision used by CI, then the setup
command prepares its Python environment, patched QEMU, plugin, and boardrunner
SDK:

```sh
nix run .#west-update
nix run .#fastdyn-setup
```

Build the ARM binary and mission host:

```sh
conf_file="$(realpath fastdyn/prj.conf)"
overlay="$(realpath fastdyn/mr_vmu_tropic.overlay)"
RDD2_BUILD_DIR="$PWD/build-mr_vmu_tropic-fastdyn" \
  nix run .#build -- -p always -- \
    -DCONF_FILE="$conf_file" \
    -DDTC_OVERLAY_FILE="$overlay"

cargo test --workspace --locked
cargo build --release --locked --package cerebri-rdd2-xtask
```

Run the native GPS qualification suite. This command builds the real native
firmware, runs the otherwise-ignored process integration test with the
executable supplied explicitly, then builds and executes all nine focused
Zephyr suites. It cannot pass by skipping the process or C tests:

```sh
nix run .#test-gps-lockstep
```

Export the named RDD2 FMI plant from the West-managed Modelica checkout, then
run the mission:

```sh
export RDD2_WORKSPACE_ROOT=/path/to/rdd2-west-workspace
export RDD2_MODELICA_MODELS_ROOT="$RDD2_WORKSPACE_ROOT/models/modelica_models"
MODELICA_MODELS_ROOT="$RDD2_MODELICA_MODELS_ROOT" \
  nix run "$RDD2_MODELICA_MODELS_ROOT#rdd2-export-plant"

export RDD2_FASTDYN_BUILD_DIR="$PWD/build-mr_vmu_tropic-fastdyn"
export RDD2_RUMOCA_PLANT_DESCRIPTION="$RDD2_MODELICA_MODELS_ROOT/artifacts/vehicles/rdd2/plant/Vehicles_Rdd2_Plant/modelDescription.xml"
export RDD2_RUMOCA_PLANT_LIBRARY=/path/to/the/compiled/source-FMU/binary
rdd2-fastdyn-ci
```

No sibling layout is assumed. The CogniPilot Devenv RDD2 profile supplies
these paths automatically for a multi-repository editable checkout.

## Timing and communications

The host advances the FMI plant and exchanges shared memory once per fixed
625 us firmware release. This preserves the 1,600 Hz rate loop and the
observed 1,000/200/50 Hz Navigation, Guidance, and Planning releases without
latest-value coalescing across a macro-step. Direct shared memory is the only
lockstep pacing path. Merge `fastdyn/comms.conf` when Ethernet, CSyn, and Zenoh
are also needed as an
asynchronous diagnostics channel. Host network provisioning is deliberately
external to `xtask`, so the mission runner never invokes privileged platform
commands.

The QEMU mission's default `0.05x` minimum is a bounded-progress floor for this
exact handshake, not evidence that the emulated Cortex-M7 meets physical
real-time deadlines. The former `3x` value was measured with a 20 ms macro-step
that coalesced Navigation, Guidance, and Planning work and is therefore not a
valid target for this profile. The mission still requires one response per
625 us release, exact observed 1,600/1,000/200/50/200 Hz generation deltas, and
every GPS, planning, navigation, flight, landing, and disarm oracle. Its
orchestrator allows up to 1,200 seconds so the speed floor, rather than a
shorter incidental timeout, remains authoritative. Physical real-time closure
requires the flight image's hardware timing/SystemView receipt.

The mission uses a conservative first-flight profile: a 0.5 m square at
0.1 m/s after an ATTITUDE takeoff and neutral settling window, followed by an
ATTITUDE landing and explicit disarm. It writes its report and log below
`artifacts/bil/`, plus the
canonical `work/mission-trajectory.csv` consumed by
`nix run .#trajectory-compare`. It verifies continuous GNSS readiness, GPS
origin ownership, mission admission and `RUNNING` state, advancing planner
references, ordered traversal of every square corner by both the reference and
plant truth, exact 10 Hz GNSS, current-status and exact observed controller
release generations, bounded GPS
navigation error, final disarm and landing, and execution speed. Plant ground
contact is intentionally expressed with `noEvent` branches.
