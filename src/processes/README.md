# eFMU processes

This directory is the complete Zephyr deployment layer for RDD2's generated
eFMUs. One source file owns one generated state object, one RTOS execution
context, and that object's driver/ZROS bindings. `src/main.c` is only the
composition root.

| Process | Release | Priority | Inputs | Outputs |
|---|---:|---:|---|---|
| `RateControlAllocator` | IMU data-ready, 800 Hz | 2 (main thread) | IMU and motor drivers; latest `rate_command` and `attitude_estimate` | DSHOT driver; `control_imu`, health, and loop metrics |
| `NavigationEstimator` | every `control_imu` sample, preintegrated; releases at 100 Hz | 3 | `control_imu`, external odometry, GNSS | `navigation_odometry`, `attitude_estimate`, gyroscope bias |
| `WaypointTrajectoryPlanner` | `control_imu` divided, 50 Hz | 5 | bounded waypoint-plan ingress | `trajectory_reference` |
| `GuidanceController` | `control_imu` divided, 100 Hz | 6 | manual/health, navigation, trajectory reference | `rate_command`, `attitude_command` |

Zephyr priorities are numerically ascending in execution precedence. Planner
therefore runs before guidance when their phase-zero releases coincide. The
rate process keeps IMU acquisition, eFMU execution, and DSHOT in one thread;
all actual thread crossings use latest-value ZROS topics rather than queues.

The waypoint-plan connector does not yet exist in the pinned Synapse catalog,
so `rdd2_waypoint_plan_t` is the fixed-capacity ingress contract owned by the
mission shell and lockstep GPS adapter. Only the generated planner publishes
`trajectory_reference`; there is no direct local-position-command ingress or
fallback around mission admission.
