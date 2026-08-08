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
- RTOS precedence is rate, navigation, planning, then guidance. Planning must
  execute before guidance on their coincident phase-zero releases so guidance
  observes the same reference update as the ideal RTOS composition.
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

**ALLOWED:**
- Existing Zephyr driver threads that already belong to subsystems such as CRSF.
- One low-priority diagnostics thread outside the flight hot path when required by `SPEC_0006`.

**PROHIBITED:**
- A thread handoff between IMU acquisition, rate/allocation, and DSHOT.
- A ZROS publish/subscribe hop between stages in that same fast thread.
- Queues between RC state, estimator, controllers, allocator, and motor output.
- Mutexes, semaphores, or workqueues in the hot-path publish path.
- Synchronous IMU bus reads from the flight control loop on `mr_vmu_tropic`.
- Periodic shell or log output from the 1600 Hz body-rate loop.
- Kalman or measurement-correction steps in the IMU-paced rate thread.
- Adding latency-oriented abstractions without measured justification on `mr_vmu_tropic`.

## Motivation

- Threading and queueing cost latency and jitter.
- The repo is meant to optimize manual-flight responsiveness first.
- Measurements, not abstraction preference, decide future complexity.

## References

- `SPEC_0003_RATE_MODE_CONTROL_SCOPE.md`
- `SPEC_0006_CODE_SIZE_AND_DEBUG_SHELL.md`
