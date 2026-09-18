# RDD2 binary-in-the-loop development

RDD2 owns its board configuration, rehosting configuration, lockstep adapter,
and mission acceptance test. FastDyn remains a generic QEMU rehosting runtime.
The firmware eFMI generator revision is locked in `cmake/RumocaLock.cmake` and
supplied by the `rumoca` flake input. FastDyn's optional standalone Rumoca build
is a separate tool pinned transitively by the exact FastDyn revision in
`west.yml`.

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
  odometry become observable before the one-shot mission reaches `PENDING`.

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

This path currently stops at a deliberate compiler capability boundary: the
plant retains checked parameter assertions, while Rumoca's source-FMU profile
currently rejects the resulting action partition. The landing-contact branches
are explicitly `noEvent` and do not create zero crossings. The export therefore
fails closed today. Do not suppress the assertions to make this test appear to
pass; the workflow becomes runnable only after Rumoca's FMI kernel preserves
them and its conformance suite covers them.

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

Run the native GPS ingress lifecycle proof. This command builds the real
native firmware and runs the otherwise-ignored process integration test with
the executable supplied explicitly; it cannot pass by skipping the process:

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
export RDD2_RUMOCA_PLANT_DESCRIPTION="$RDD2_MODELICA_MODELS_ROOT/artifacts/vehicles/rdd2/plant/modelDescription.xml"
export RDD2_RUMOCA_PLANT_LIBRARY=/path/to/the/compiled/source-FMU/binary
rdd2-fastdyn-ci
```

No sibling layout is assumed. The CogniPilot Devenv RDD2 profile supplies
these paths automatically for a multi-repository editable checkout.

## Timing and communications

The default 20 ms plant macro-step advances all 16 controller ticks at
800 Hz. Direct shared memory is the only lockstep pacing path. Merge
`fastdyn/comms.conf` when the native target also needs its network stack for
diagnostics. ROS 2 CDR mirrors run on the host. Host network provisioning is
external to `xtask`, so the mission runner never invokes privileged platform
commands.

The mission writes its report and log below `artifacts/bil/`, plus the
canonical `work/mission-trajectory.csv` consumed by
`nix run .#trajectory-compare`. It verifies continuous GNSS readiness, GPS
origin ownership, mission admission and `RUNNING` state, advancing planner
references, ordered traversal of every square corner by both the reference and
plant truth, exact 10 Hz GNSS and current-status generations, bounded GPS
navigation error, final disarm and landing, and execution speed. Plant ground
contact is intentionally expressed with `noEvent` branches.

## Execution speed

The speed check (`RDD2_FASTDYN_MIN_SPEEDUP`, 3x real time by default) guards
the environment, and two things dominate how fast the rehosted image runs.
Every system-control register access (SysTick, MPU, PendSV) leaves QEMU's
translated code and is served by the FastDyn device-model plugin, and the
Zephyr timer driver makes three or four such accesses per time query or
timeout change. The pinned FastDyn writes its per-access `io.log` only for
twintrace and probe runs or when `FASTDYN_IO_LOG` is set; with that log on,
the mission runs at about real time. `fastdyn/prj.conf` also builds the
rehosted image without time slicing and without the MPU stack guard, because
each context switch otherwise reprograms both and the 800 Hz pipeline switches
several times per tick: on the CI host the GNSS mission runs 1.3x with them
and 3.7x without, and the GNSS-denied run 3.4x. Neither setting changes what
the flight processes compute. To see where guest time goes, sample the program
counter through the QEMU monitor on port 5555 (`info registers`, the `R15`
field) and resolve it against `build-mr_vmu_tropic-fastdyn/zephyr/zephyr.elf`.
`RDD2_FASTDYN_CONTROLLER_BENCHMARK_S=<s>` runs the firmware alone, without the
plant, for that many simulated seconds instead of the mission and prints
`RDD2_CONTROLLER_BENCHMARK` with its own speed ratio.

## GNSS-denied estimator check

`RDD2_FASTDYN_GNSS_DENIED=1 nix run .#fastdyn-ci` runs the same rehosted image
with the lockstep GNSS source emitting only unusable fixes. No origin is ever
established, the mission never starts, and the aircraft rests disarmed for the
whole run; the report then judges the navigation estimator alone: it must
report a usable estimate within 10 s, stay finite, and hold the resting
aircraft within 2 m horizontally and vertically and below 1 m/s, which is the
unaided condition an indoor flight puts it in. The plant is sub-stepped so its
internal integration step never exceeds 1.25 ms (four sub-steps per 5 ms
exchange interval, sixteen per 20 ms; `RDD2_FASTDYN_PLANT_SUBSTEPS` overrides
the count) so its landing-gear contact integrates cleanly. At one step per 5 ms
interval a resting aircraft chattered between free fall and two g on its
accelerometer, and at a 5 ms internal step a touchdown with a fraction of a
degree of tilt settled into a phase-locked limit cycle that the accelerometer
reported as a steady 5.5 m/s2 and the estimator integrated into metres of
drift.

