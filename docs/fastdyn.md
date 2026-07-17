# RDD2 binary-in-the-loop development

RDD2 owns its board configuration, rehosting configuration, lockstep adapter,
and mission acceptance test. FastDyn remains a generic QEMU rehosting runtime.

The repository-owned pieces are:

- `fastdyn/mr_vmu_tropic.toml`: rehosting and process configuration;
- `fastdyn/prj.conf` and `fastdyn/mr_vmu_tropic.overlay`: Zephyr build inputs;
- `fastdyn/comms.conf`: optional Ethernet/Zenoh side channel;
- `tools/fastdyn_mission`: compiled shared-memory and FMI 3 host;
- `fastdyn/run_mission.sh`: bounded mission and artifact checks.

The mission host dynamically loads `Vehicles.Rdd2.AvionicsPlant`. It contains
no handwritten quadrotor equations. The same host and FMI plant are used for
host-native firmware and the rehosted ARM binary.

## Standalone repository setup

Initialize the isolated dependency workspace from this repository's West
manifest:

```sh
nix run .#west-update
```

Build the ARM binary and mission host:

```sh
conf_file="$(realpath fastdyn/prj.conf)"
overlay="$(realpath fastdyn/mr_vmu_tropic.overlay)"
RDD2_BUILD_DIR="$PWD/build-mr_vmu_tropic-fastdyn" \
  nix run .#build -- -p always -- \
    -DCONF_FILE="$conf_file" \
    -DDTC_OVERLAY_FILE="$overlay"

cargo test --locked --manifest-path tools/fastdyn_mission/Cargo.toml
cargo build --release --locked --manifest-path tools/fastdyn_mission/Cargo.toml
```

Export the named RDD2 FMI plant from the West-managed Modelica checkout, then
run with any FastDyn installation:

```sh
export RDD2_WORKSPACE_ROOT=/path/to/rdd2-west-workspace
export RDD2_MODELICA_MODELS_ROOT="$RDD2_WORKSPACE_ROOT/models/modelica_models"
MODELICA_MODELS_ROOT="$RDD2_MODELICA_MODELS_ROOT" \
  nix run "$RDD2_MODELICA_MODELS_ROOT#rdd2-export-plant"

export FASTDYN_ROOT=/path/to/FastDyn
export RDD2_FASTDYN_BUILD_DIR="$PWD/build-mr_vmu_tropic-fastdyn"
export RDD2_RUMOCA_PLANT_DESCRIPTION="$RDD2_MODELICA_MODELS_ROOT/artifacts/vehicles/rdd2/plant/modelDescription.xml"
export RDD2_RUMOCA_PLANT_LIBRARY="$RDD2_MODELICA_MODELS_ROOT/artifacts/vehicles/rdd2/plant/binaries/x86_64-linux/Vehicles_Rdd2_AvionicsPlant.so"
fastdyn/run_mission.sh
```

No sibling layout is assumed. The CogniPilot Devenv RDD2 profile supplies
these paths automatically for a multi-repository editable checkout.

## Timing and communications

The default 20 ms plant macro-step advances all 32 controller ticks at
1,600 Hz. Direct shared memory is the only lockstep pacing path. Merge
`fastdyn/comms.conf` when Ethernet, CSyn, and Zenoh are also needed as an
asynchronous diagnostics channel, and set `FASTDYN_RDD2_NETWORK_SETUP=true`
for that profile.

The mission writes its report and log below `artifacts/bil/`, plus the
canonical `work/mission-trajectory.csv` consumed by
`nix run .#trajectory-compare`. It verifies arming, takeoff, roll and pitch
response, eventful landing contact, final disarm, and execution speed.
