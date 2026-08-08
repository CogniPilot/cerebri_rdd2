# SPEC_0003: Flight Control Scope

## Status
ACCEPTED

## Summary
RDD2 uses the same planning, navigation, guidance, rate-control, and allocation
definitions in Modelica and firmware. Rumoca generates one Production Code
eFMU per deployable block; the ideal RTOS model and embedded application
therefore execute the same control laws at the same task boundaries.

## Specification

**REQUIRED:**
- `Planning.Bezier.WaypointTrajectoryPlanner`,
  `Vehicles.Rdd2.NavigationEstimator`, `Vehicles.Rdd2.GuidanceController`, and
  `Vehicles.Rdd2.RateControlAllocator` are the deployable eFMU boundaries.
- `Vehicles.Rdd2.AvionicsSystem` is the executable ideal-RTOS routing contract
  for planning, guidance, and rate control; it must not contain a second
  implementation of those laws.
- Rumoca-generated eFMI Production Code is the embedded controller
  implementation. Handwritten C only binds fixed-layout messages, scheduling,
  safety interlocks, and motor devices to the generated public interface.
- One persistent generated state object advances in each eFMU process at that
  block's modeled sample rate. Firmware must not reconstruct generated state
  or mutate generated implementation details.
- The explicit pilot-selectable modes are `ACRO`, `ATTITUDE`, and `POSITION`.
- `CH5` uses three ranges: low selects `ACRO`, middle selects `ATTITUDE`, and
  high selects `POSITION`.
- `ACRO` commands body angular velocity directly from pilot sticks.
- `ATTITUDE` commands roll and pitch attitude while retaining direct pilot yaw
  rate and collective-thrust control.
- `POSITION` consumes the latest ENU navigation estimate and trajectory
  reference and runs the log-linear geometric outer loop.
- All three modes use the same body-rate and multirotor allocation stages.
- The mode is an explicit controller input, not hidden controller state.
- Controller vectors remain arrays across the Modelica and C boundary.
- Control-path state is fixed-size and explicit.
- Control code consumes body rates in `FLU` body axes and world references in
  `ENU`.
- Motor order at the control layer is front-right, rear-left, front-left,
  rear-right, matching the shared plant and physical DSHOT channel contract.
**PROHIBITED:**
- `double` in the control path.
- Handwritten PID, attitude-control, position-control, or mixer equations in
  the firmware wrapper.
- A controller implementation that differs between ideal-RTOS tests,
  native-sim/FastDyn, and flight firmware.
- Scalarized roll, pitch, yaw, or motor fields at the generated controller
  boundary.
- Extra controller modes used only to manage integrators or transitions.
- Navigation fusion in the rate-loop hot path.
- Attitude correction or other estimator update steps that block the 1600 Hz body-rate loop.

## Motivation

- A single executable control definition prevents the simulator and embedded
  vehicle from silently diverging.
- Explicit arrays and a small causal interface keep the controller auditable
  and suitable for later Lyapunov, contraction, and reachable-set analysis.
- A single externally selected mode is the minimum unavoidable hybrid state;
  additional internal modes would expand the verification state space.

## References

- `SPEC_0002_LATENCY_DRIVEN_ARCHITECTURE.md`
- `SPEC_0004_TROPIC_HARDWARE_SCOPE.md`
