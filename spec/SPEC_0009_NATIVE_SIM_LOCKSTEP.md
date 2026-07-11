# SPEC_0009: Native Sim Lockstep Transport

## Status
ACCEPTED

## Summary
`native_sim` runs the same 1600 Hz flight-control loop as flight firmware. Its
simulator IO terminates in lockstep RC, IMU, and DSHOT devices using the same
generated `synapse_fbs` messages and shared direct sequencing
module as CUBS2. CSyn/Zenoh may run as a communications side-channel, but never
paces lockstep.

## Specification

**REQUIRED:**
- The `native_sim` target is for lockstep simulation and debug only.
- The controller still runs in the main 1600 Hz application thread on `native_sim`.
- Host simulator IO must stay out of the 1600 Hz controller thread.
- `src/main.c` must remain free of `native_sim`-specific control-path branches.
- The simulator boundary must terminate at board-selected `rc`, `imu0`, and `motors` devices, not at ad hoc app-level IO hooks.
- Inbound simulator data uses generated `ManualControlData` and
  `InertialSampleData` fixed-layout payloads.
- Outbound data uses generated `PwmSignalOutputsData`, `VehicleHealthData`,
  `AttitudeEstimateData`, `AttitudeCommandData`, and `ControlLoopMetricsData`
  fixed-layout payloads.
- CSyn owns the `synapse_fbs` release, topic catalog, canonical Zenoh keys,
  payload sizes, and generated codecs; RDD2 must not duplicate them.
- The transport stages only the latest inbound inertial payload instead of
  queueing per-sample work into the controller.
- Schema-shaped values must use generated FlatCC structs and accessors. Manual
  byte-offset decoding and handwritten wire mirrors are prohibited.
- Lockstep transport and staging live under `subsys/lockstep/`, not `src/`.
- Fake native-sim device adapters live under their driver-family directories.
- Board configuration and device selection live in `boards/native_sim.conf`
  and `boards/native_sim.overlay`.
- Request/response sequencing uses the transport- and payload-independent
  `cerebri_lockstep` west module. ZROS must not own or depend on simulation
  sequencing.
- CSyn/ZROS bridging and Ethernet/Zenoh must not be a lockstep coordinator.
  They may be enabled concurrently for realtime communications testing.

**PROHIBITED:**
- Blocking socket IO in the 1600 Hz control loop.
- A simulation-only control loop separate from `src/main.c`.
- Incompatible simulation messages that bypass CSyn's pinned `synapse_fbs`.
- Per-packet heap allocation in the hot path.
- Custom topic keys, payload-size tables, or FlatBuffer decoders in RDD2.

## Motivation

- Lockstep simulation is valuable for fast iteration, but it must exercise the
  same controller code as flight firmware.
- Driver-backed lockstep keeps the simulator boundary explicit without
  polluting the controller.
- Reusing CUBS2's generated message contract and the `cerebri_lockstep` module
  keeps firmware and tooling aligned without putting simulation policy or a
  communications transport in ZROS.

## References

- `../boards/native_sim.conf`
- `../boards/native_sim.overlay`
- `../src/main.c`
- `../src/rc_input.c`
- `../subsys/lockstep/lockstep_input.c`
- `../subsys/lockstep/lockstep_transport.h`
- `../subsys/lockstep/lockstep_transport.c`
- `../subsys/lockstep/lockstep_direct.c`
- `../subsys/lockstep/lockstep_fastdyn.c`
- `../drivers/input/lockstep_rc.c`
- `../drivers/sensor/lockstep_imu.c`
- `../drivers/nxp_flexio_dshot/lockstep_dshot.c`
- `SPEC_0002_LATENCY_DRIVEN_ARCHITECTURE.md`
- `SPEC_0004_TROPIC_HARDWARE_SCOPE.md`
- `SPEC_0007_FLATBUFFER_TOPICS.md`
