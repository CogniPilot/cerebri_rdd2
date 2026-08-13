# SPEC_0002: Latency-Driven Architecture

## Status
ACCEPTED

## Summary
Minimal latency is a primary system driver. The IMU-paced 1600 Hz thread calls
the generated rate/allocation eFMU and DSHOT directly. Navigation, planning,
and guidance run as separate RTOS tasks at the periods declared by their
Modelica blocks and exchange latest-value messages over ZROS only where a
thread boundary actually exists.

## Specification

**REQUIRED:**
- The device hot path uses one thread: the rate process in
  `src/processes/rate_control_allocator.c`, running in Zephyr's main thread.
- On `mr_vmu_tropic`, the main body-rate loop is paced by the `ICM45686` data-ready interrupt at 1600 Hz.
- Every control-loop iteration consumes the latest decoded IMU sample,
  cross-thread navigation angular velocity, and `RateCommandData`; advances
  the generated rate/allocation eFMU; and triggers DSHOT in that one thread.
- The navigation estimator eFMU runs in its own thread at its modeled 1 ms
  period. It consumes the latest IMU and aiding data and publishes odometry and
  attitude/rate estimates through ZROS.
- The guidance eFMU runs at 200 Hz and publishes `RateCommandData` through
  ZROS. Its task wakes on the rate-limited estimator publication, so hardware
  and lockstep use the same release sequence. It never sits between the IMU
  interrupt and motor trigger.
- The planning eFMU runs at 50 Hz and publishes the trajectory reference
  consumed by guidance.
- Navigation, Guidance, and Planning derive their modeled release phases by
  integer phase accumulation from the preceding control-domain publication
  rate (`1600 -> 1000 -> 200/50 Hz`). ZROS subscription wall-clock rate limits
  may not define flight-process cadence, so accelerated lockstep and paused
  lockstep preserve the same releases as hardware.
- RTOS precedence is rate, navigation, planning, then guidance. Planning must
  execute before guidance on their coincident phase-zero releases so guidance
  observes the same reference update as the ideal RTOS composition.
- A lockstep coordinator runs below all four flight processes. It may wake the
  rate process with new input, but it must not sample/respond before ready
  Navigation, Planning, and Guidance releases have had scheduler precedence.
- Planning, navigation, guidance, and rate control communicate across their
  actual task boundaries with bounded latest-value ZROS payloads.
- Once IMU pacing is active on `mr_vmu_tropic`, the hot path does not add a second fixed-period sleep on top of that pacing source.
- RC handoff into the app is a bounded latest-sample update, not a queue.
- Hot-path publication to diagnostics uses one lockless publish step into a double-buffered latest-value store.
- Diagnostics readers consume the latest published data without blocking the control loop.
- Diagnostics publication may be decimated relative to the body-rate loop and must not gate motor output.
- Hot-path math uses `float`, not `double`.
- No heap allocation occurs after boot.
- Hot-path synchronization points stay explicit and few.
- If attitude correction exists, it must stay out of the 1600 Hz body-rate hot path and must not gate motor output.
- A Rate command is usable only after a new topic sample has been observed.
  Its publisher timestamp must be in the shared IMU-derived control-time domain
  and no more than `25 ms` old (five `200 Hz` Guidance release periods) when
  consumed by Rate. A future-dated command is unusable. Wall-clock time may not
  age a retained command while lockstep control ticks are paused.
- A disarmed-to-armed transition additionally requires a newly observed usable
  Rate command whose control timestamp is not earlier than the current
  low-to-high arm-switch request. Rate must first publish a disarmed
  `VehicleHealth` generation while the switch is low. If that acknowledgement
  was unavailable, the edge must publish it, discard the edge-cycle command
  for arming, and wait for a newly observed usable command on a later control
  tick. A retained command from before
  that request may remain usable for disarmed controller evaluation, but it
  must not arm or energize the normal motor-output path. This is the
  actuator-side completion of Guidance publication withholding and is
  independent of whether decimated Guidance observed a brief low switch.
- An unusable Rate command, nonzero Rate `ErrorSignalStatus`, or nonfinite Rate
  output must disarm and zero the normal flight-output path. If the arm switch
  was high when the fault occurred, that fault remains latched until the switch
  is observed low in a valid, fresh manual-control sample; recovery may not
  reactivate motors without pilot
  acknowledgement and the ordinary throttle-low arming check.
- `VehicleHealthData.Failsafe` mirrors that Rate control-fault latch, including
  its high-switch persistence and valid low-switch acknowledgement.
- The Navigation, Guidance, and Rate firmware wrappers must sample their generated eFMU
  `ErrorSignalStatus` immediately after `DoStep`. A nonzero status or a
  nonfinite value at a publication or actuator boundary must fail closed:
  Navigation publishes a finite estimate marked invalid, Guidance withholds
  its command publication, and Rate disarms and commands zero motor output.
- These checks run inline in the existing process threads and may not add a
  queue, worker thread, blocking call, heap allocation, or extra control-cycle
  delay.
- Navigation publications carry the triggering IMU control timestamp, and
  Guidance propagates the validated Navigation timestamp into the commands it
  publishes. Generated floating-point time values are not converted back into
  firmware topic timestamps.
- A trajectory reference is usable for `POSITION` only after a new reference
  sample has been observed. Its timestamp must be in the same IMU-derived
  control-time domain as Navigation, may not be future dated, and may be no
  more than `100 ms` old when Guidance consumes it. Its coordinate frame and
  type mask must match the supported local-ENU position interface, and every
  position, velocity, acceleration, yaw, and yaw-rate value must be finite.
- Planning may publish a current-position hold while an accepted mission is
  pending. Every such reference and every generated mission reference carries
  the current Navigation control timestamp; wall-clock time may not be used to
  establish reference freshness in lockstep.
- The bounded local mission ingress uses fixed-capacity ZROS data and runs in
  the existing Planning and shell contexts. It may not add a queue, heap
  allocation, network dependency, or control-cycle worker.

**ALLOWED:**
- Existing Zephyr driver threads that already belong to subsystems such as CRSF.
- One low-priority diagnostics thread outside the flight hot path when required by `SPEC_0006`.
- An explicitly activated shell motor test may bypass the normal flight-control
  fault latch for bench work. Normalized test values must be finite, raw DSHOT
  test values remain driver-bounded, and neither test path clears a latched
  flight-control fault.

**PROHIBITED:**
- A thread handoff between IMU acquisition, rate/allocation, and DSHOT.
- A ZROS publish/subscribe hop between stages in that same fast thread.
- Queues between RC state, estimator, controllers, allocator, and motor output.
- Mutexes, semaphores, or workqueues in the hot-path publish path.
- Synchronous IMU bus reads from the flight control loop on `mr_vmu_tropic`.
- Periodic shell or log output from the 1600 Hz body-rate loop.
- Kalman or measurement-correction steps in the IMU-paced rate thread.
- Adding latency-oriented abstractions without measured justification on `mr_vmu_tropic`.
- Reusing a retained trajectory reference after its freshness budget expires.
- Allowing a stale, future-dated, nonfinite, unsupported-frame, or
  unsupported-mask reference to enable `POSITION` control.

## Motivation

- Threading and queueing cost latency and jitter.
- The repo is meant to optimize manual-flight responsiveness first.
- Measurements, not abstraction preference, decide future complexity.

## References

- `SPEC_0003_RATE_MODE_CONTROL_SCOPE.md`
- `SPEC_0006_CODE_SIZE_AND_DEBUG_SHELL.md`
- `../tests/process_control_safety/`
- `../tests/process_wrapper_fault_injection/`
- `../tests/generated_navigation_fault_injection/`
- `../tests/waypoint_mission_ingress/`
- `../tests/mission_shell/`
