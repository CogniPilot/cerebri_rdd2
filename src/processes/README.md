# eFMU processes

This directory is the complete Zephyr deployment layer for RDD2's generated
eFMUs. One source file owns one generated state object, one RTOS execution
context, and that object's driver/ZROS bindings. `src/main.c` is only the
composition root.

| Process | Release | Priority | Inputs | Outputs |
|---|---:|---:|---|---|
| `RateControlAllocator` | IMU data-ready, 800 Hz | 2 (main thread) | IMU and motor drivers; latest `rate_command` and `attitude_estimate` | DSHOT driver; `control_imu`, health, and loop metrics |
| `NavigationEstimator` | latest-value IMU, 800 Hz | 3 | `control_imu`, external odometry, GNSS | `navigation_odometry`, `attitude_estimate` |
| `WaypointTrajectoryPlanner` | estimator release, 50 Hz | 5 | waypoint-plan ingress and external-reference fallback | `trajectory_reference` |
| `GuidanceController` | estimator release, 200 Hz | 6 | manual/health, navigation, trajectory reference | `rate_command`, `attitude_command` |

Zephyr priorities are numerically ascending in execution precedence. Planner
therefore runs before guidance when their phase-zero releases coincide. The
rate process keeps IMU acquisition, eFMU execution, and DSHOT in one thread;
all actual thread crossings use latest-value ZROS topics rather than queues.

The waypoint-plan connector does not yet exist in the pinned Synapse catalog,
so `rdd2_waypoint_plan_t` is the fixed-capacity ingress contract for the future
mission transport adapter. The externally catalogued
`LocalPositionCommandData` path remains a fallback until that adapter is
present. It is intentionally isolated at the planner boundary instead of
leaking mission handling into the generated model or other processes.
