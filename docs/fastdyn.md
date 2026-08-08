# RDD2 binary-in-the-loop development

RDD2 owns its board configuration, rehosting configuration, lockstep adapter,
and mission acceptance test. FastDyn remains a generic QEMU rehosting runtime.
The firmware eFMI generator release and artifact hashes are locked in
`cmake/RumocaLock.cmake`. FastDyn's optional standalone Rumoca build is a
separate tool pinned transitively by the exact FastDyn revision in `west.yml`.

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
`Vehicles.Rdd2.PlantAdapter` FMI 3 Co-Simulation interface. It contains no
handwritten quadrotor equations. The same host and FMI plant are used for
host-native firmware and the rehosted ARM binary.

This path currently stops at a deliberate compiler capability boundary: the
plant contains eventful landing contact, while Rumoca's source-FMU profile
rejects event/action partitions. The export therefore fails closed today.
Do not substitute an event-free plant or suppress touchdown relations to make
this test appear to pass; the workflow becomes runnable only after Rumoca's FMI
kernel preserves those events and its conformance suite covers them.

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

Export the named RDD2 FMI plant from the West-managed Modelica checkout, then
run the mission:

```sh
export RDD2_WORKSPACE_ROOT=/path/to/rdd2-west-workspace
export RDD2_MODELICA_MODELS_ROOT="$RDD2_WORKSPACE_ROOT/models/modelica_models"
MODELICA_MODELS_ROOT="$RDD2_MODELICA_MODELS_ROOT" \
  nix run "$RDD2_MODELICA_MODELS_ROOT#rdd2-export-plant"

export RDD2_FASTDYN_BUILD_DIR="$PWD/build-mr_vmu_tropic-fastdyn"
export RDD2_RUMOCA_PLANT_DESCRIPTION="$RDD2_MODELICA_MODELS_ROOT/artifacts/vehicles/rdd2/plant/Vehicles.Rdd2.PlantAdapter/modelDescription.xml"
export RDD2_RUMOCA_PLANT_LIBRARY=/path/to/the/compiled/source-FMU/binary
rdd2-fastdyn-ci
```

No sibling layout is assumed. The CogniPilot Devenv RDD2 profile supplies
these paths automatically for a multi-repository editable checkout.

## Timing and communications

The default 20 ms plant macro-step advances all 32 controller ticks at
1,600 Hz. Direct shared memory is the only lockstep pacing path. Merge
`fastdyn/comms.conf` when Ethernet, CSyn, and Zenoh are also needed as an
asynchronous diagnostics channel. Host network provisioning is deliberately
external to `xtask`, so the mission runner never invokes privileged platform
commands.

The mission writes its report and log below `artifacts/bil/`, plus the
canonical `work/mission-trajectory.csv` consumed by
`nix run .#trajectory-compare`. It verifies arming, takeoff, roll and pitch
response, eventful landing contact, final disarm, and execution speed.
