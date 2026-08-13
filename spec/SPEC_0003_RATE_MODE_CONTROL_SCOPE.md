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
- When the onboard or lockstep GNSS source is configured, a `POSITION` request
  while configured-source readiness and `VehicleHealth.Armed` are both false
  must not arm
  in `POSITION`. A valid, active high arm switch in that state disarms the
  generated Guidance input, withholds both Guidance command publications, and
  latches a control fault. Only a successful current manual-input update
  carrying valid, active, low arm-switch state may acknowledge that latch.
- If `VehicleHealth.Armed` is true while `POSITION` is requested and the
  configured GNSS readiness is false, Guidance must run and publish the
  `ATTITUDE` mode
  command instead without latching a GNSS-readiness fault. This applies both to
  an in-flight `POSITION` request and to readiness loss in `POSITION`. GNSS
  readiness must not gate `ACRO` or `ATTITUDE`, and it must not affect builds
  using the radio/motion-capture source. The FastDyn profile is a GNSS source
  and is subject to these gates.
- A usable fresh local-ENU trajectory reference is part of the `POSITION`
  capability. A disarmed high arm switch with no usable reference follows the
  same block, withholding, and valid-current-low acknowledgement contract as
  unavailable onboard GNSS.
- Rate may transition from disarmed to armed only after consuming a newly
  published usable Guidance command at or after the current low-to-high
  arm-switch request, but only after a disarmed `VehicleHealth` generation was
  successfully published while the switch was low. If the edge must make that
  announcement itself, its command cannot arm. It must not arm from a retained
  command that predates or coincides with that edge announcement,
  including during the interval before a blocked `POSITION` request is visible
  through `VehicleHealth`.
- If the vehicle is already armed when `POSITION` capability is lost because
  the trajectory reference or onboard GNSS becomes unusable, Guidance must
  continue publishing an `ATTITUDE` command. It must latch that effective-mode
  fallback until the pilot requests a mode other than `POSITION`; capability
  recovery alone may not re-enter the position controller.
- The built-in demo mission ingress accepts only a bounded, fixed-capacity,
  local relative current-altitude plan while the vehicle is disarmed, manual
  control is current and valid with the arm switch low, Navigation is current
  and valid, and the configured position source is ready. It may not initiate
  takeoff or landing.
- An accepted demo plan remains pending through manual `ATTITUDE` takeoff. On
  the transition to armed `POSITION`, Planning rebases the complete relative
  plan to the then-current ENU estimate, makes its first waypoint that current
  position, and issues the plan to the generated trajectory planner exactly
  once.
- Shell publication is only a mission request, not evidence that Planning
  accepted it. Planning is the sole admission and lifecycle authority at its
  current control release. A request loaded while disarmed with the arm switch
  low may remain pending in `ACRO` or `ATTITUDE`; armed `ACRO` aborts it, and
  execution remains restricted to armed `POSITION`.
- A running demo mission aborts on disarm, exit from `POSITION`, invalid or
  stale manual control, invalid or stale Navigation, or loss of configured
  position-source readiness. An aborted plan may not resume when the condition
  recovers; another valid disarmed load is required.
- The demo shell supports only a bounded current-altitude square and explicit
  cancellation. It is an ingress and validation adapter, not a handwritten
  trajectory generator or flight controller; the generated Planning eFMU
  remains the sole trajectory-law implementation.
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
- Automatic takeoff or landing from the built-in demo mission ingress.
- Resuming a previously aborted mission on mode, arm, navigation, manual, or
  position-source recovery.

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
- `../tests/waypoint_mission_ingress/`
- `../tests/mission_shell/`
