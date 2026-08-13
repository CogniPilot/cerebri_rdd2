# SPEC_0005: GNSS Staging

## Status
ACCEPTED

## Summary
M10 GNSS support is staged after manual flight bring-up and must stay out of the rate-loop hot path.

## Specification

**REQUIRED:**
- GNSS stays outside the manual-flight inner loop.
- The preferred integration surface is Zephyr GNSS.
- M10 support is developed without adding control-path dependencies.
- If the generic path is insufficient, any dedicated M10 work remains isolated from the rate controller.
- The onboard M10 source configures receiver UART1 at `115200` with one
  RAM-layer `UBX-CFG-VALSET` transaction. The transaction enables UBX input and
  output, disables NMEA input and output, enables `UBX-NAV-PVT` on UART1 at one
  message per navigation solution, sets a `100 ms` measurement period with a
  navigation rate of one, selects automatic 2D/3D fixing and airborne-2g
  dynamics, and does not issue M10-unsupported UART1 RTCM3X protocol keys. A
  configuration attempt becomes accepted only on an exact ACK for
  `CFG-VALSET`; a NAK or an ACK timeout of `1.1 s` retries the same complete,
  byte-identical request, and three failed attempts leave the source failed
  closed until reboot.
- Onboard-source readiness requires the complete configuration ACK followed by
  five consecutive accepted `NAV-PVT` samples. Consecutive gaps must each be
  within `75--125 ms`, the most recent sample must be no more than `300 ms` old,
  the fix must be `Fix3d`, `Dgnss`, `RtkFloat`, or `RtkFixed`, and receiver
  horizontal, vertical, and speed accuracy must be positive and no greater
  than `10 m`, `15 m`, and `5 m/s`. The positive-accuracy rule is a stricter
  onboard producer reset/default defense; it does not change the downstream
  adapter rule that an injected UBX-derived zero is a real estimate raised to
  the covariance floor.
- Before onboard readiness, after any readiness gate drops, or after a topic
  publication failure, the onboard boundary exposes `NoFix` with `65535`
  accuracy and DOP sentinels and retries a failed invalidation. It closes the
  concurrency-safe readiness bit before publishing a rejected sample and opens
  it only after a usable topic update succeeds. Readiness queries self-age
  against current uptime so starvation of the lower-priority GNSS thread
  cannot leave a higher-priority consumer observing stale readiness.
- Navigation consumes a GNSS fix at most once, on a triggering IMU release in
  the shared boot-time domain. A fix up to `100 ms` in the future may remain
  pending until IMU time reaches it; a fix farther in the future or more than
  `500 ms` old is discarded. After origin capture, consumed fix timestamps are
  strictly increasing; duplicate, replayed, and out-of-order timestamps do not
  reach the estimator. A timestamp is recorded as consumed only after a valid
  finite measurement is constructed. A two-entry oldest-first pending queue
  retains the next newer publication while the oldest fix is still future, so
  a `10 Hz` stream neither hides the due fix behind its successor nor loses the
  successor while waiting.
- GPS position aiding accepts only `Fix3d`, `Dgnss`, `RtkFloat`, or `RtkFixed`
  fixes with latitude in `[-90, 90]` degrees, longitude in `[-180, 180]`
  degrees, MSL altitude in `[-1000, 20000] m`, horizontal accuracy at most
  `10 m`, and vertical accuracy at most `15 m`. Unknown saturated accuracy
  values are unusable. UBX-NAV-PVT defines zero as a real accuracy estimate,
  so zero is accepted and raised to the covariance floor.
- The local frame origin is the first acceptable fresh GPS publication whose
  receipt is associated with an already observed, causal `VehicleHealthData`
  sample no more than `25 ms` old that is neither armed nor failsafe. A fix
  received while that predicate is false may not later become an origin merely
  because the vehicle disarms. A future-dated pending fix records eligibility
  at receipt and must satisfy the current health predicate again when due.
- In lockstep, a newly observed health publication is authoritatively produced
  by the rate thread after processing the triggering IMU control tick. The
  Navigation wrapper associates that publication with the IMU control
  timestamp instead of comparing its wall-clock payload timestamp to simulated
  control time.
- The origin uses MSL altitude and stays immutable until reboot. Origin capture
  holds the Navigation estimator reset and suppresses retained mocap until the
  wrapper observes a valid, finite initialized estimate, so the GPS position at
  local ENU zero owns initialization even when the first generated reset step
  fails wrapper validation. In the radio/mocap build, that ownership expires
  after `100 ms` so persistently invalid initialization cannot suppress live
  mocap forever; onboard-GNSS ownership does not expire.
- A build using the onboard GNSS source disables external-odometry/mocap input
  to Navigation so a retained external ENU frame cannot reframe the outdoor GPS
  ENU estimate. The `mocap-gnss` radio-source build retains mocap input.
- A lockstep GNSS source has the same immutable-origin and external-odometry
  suppression semantics as the onboard source. It is distinct from radio
  injection, publishes only host-provided `GnssFixData`, and derives readiness
  from fix usability and simulated-time age rather than receiver ACK state.
  Fix loss or an age greater than `300 ms` closes readiness before the next
  Guidance or Planning release may use it.
- Geodetic fixes use the `modelica_models/Geodesy.geodeticToLocalEnu`
  convention: spherical Earth radius `6378137 m`, great-circle distance from
  the captured origin, and local East-North-Up output. Integer `deg_e7`
  latitude and longitude are differenced in `int64_t` before conversion to
  float so centimeter-scale local deltas are preserved.
- The spherical projection has a known local WGS84 scale bias at the flight
  latitude of approximately `+0.24%` north and `-0.15%` east. It is approved
  only for a short local pattern: at a planned `40 m` component displacement,
  this budgets about `0.10 m` north and `0.06 m` east error. Longer patterns
  require a post-flight ellipsoidal projection change and new reference tests.
- A projected GPS position is usable only while each ENU component is within
  `10 km` of the captured origin. A larger jump is treated as a coordinate or
  sign glitch and is not consumed.
- Position covariance is diagonal ENU covariance derived from reported
  horizontal and vertical one-sigma accuracy, with a `0.5 m` standard-
  deviation floor. Off-diagonal terms are zero.
- Horizontal velocity is reconstructed from ground speed and clockwise course
  from true north. GPS velocity aiding is valid only when both `CourseValid`
  and `VelocityUpValid` are set, course is below `360 deg`, and reported
  velocity accuracy is at most `5 m/s`; otherwise GPS remains position-only.
  Velocity covariance is diagonal with a `0.1 m/s` standard-deviation floor.
  The generated estimator does not latency-compensate `gps.timestamp_s`; GPS
  aiding is therefore qualified only for low-speed hold and bench walking,
  not aggressive flight. At hover, the onboard producer intentionally clears
  `CourseValid`, so position-only GPS aiding is the expected mode. Even when
  vertical velocity is valid, the current generated ABI has one validity bit
  for the entire three-axis velocity correction; firmware therefore discards
  vertical velocity rather than falsely presenting unobserved horizontal zero
  as a measurement. Per-axis or separate vertical validity is post-flight
  generated-ABI work.

**CURRENT DIRECTION:**
- Keep the flight image tolerant of GNSS being absent.
- Avoid noisy placeholder GNSS configurations in the default bench image.
- Bring GNSS online only after CRSF, DSHOT, and IMU behavior are stable.

**PROHIBITED:**
- GNSS polling inside the 1600 Hz control loop.
- GNSS-specific worker threads justified only by convenience.
- Holding first manual flight hostage to GNSS completion.

## Motivation

- GNSS is not required for first manual flight.
- GNSS debugging should not destabilize the core flight stack.
- Staging reduces bring-up risk.

## References

- `SPEC_0002_LATENCY_DRIVEN_ARCHITECTURE.md`
- `SPEC_0004_TROPIC_HARDWARE_SCOPE.md`
