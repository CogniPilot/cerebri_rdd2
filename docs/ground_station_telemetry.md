# Ground station interface: RDD2 telemetry link

Interface contract for software on the other end of the RDD2 telemetry radio.
Everything here is derived from the pinned `synapse_fbs` **0.7.0** catalog
(schema set hash `2fd857effb6c7558d6869f4307a5354c`) and from what
`src/synapse_messages.c` actually populates.

Vehicle side is `subsys/csyn_serial/`. A working reference implementation of
this document lives in `tools/synapse_serial/synapse_serial.py` — read it
before writing a new decoder.

## Link

A transparent serial link, normally a SiK radio pair.

| | |
|---|---|
| Default baud | 57600 8N1 |
| Byte order | little-endian throughout |
| Vehicle port | whatever the `csyn-serial` devicetree alias names |

There is **no handshake, no heartbeat and no request/response**. The vehicle
streams unsolicited frames; a ground station that wants a liveness signal
should treat "a decoded frame in the last N seconds" as the link being up.

## Frame format

Bare fixed-layout payload structs inside the compact framing that
`fbs/transport.fbs` prescribes for constrained byte streams. No FlatBuffers
table encoding is used on this link — the payload is the raw struct image.

```
off  size  field
 0     2   sync      0x53 0x59  ('S','Y')
 2     2   len       u16, payload byte count
 4     2   topic_id  u16, synapse catalog TopicId
 6     1   seq       u8, wraps at 256, increments per frame sent
 7     1   flags     u8, reserved, currently always 0
 8     N   payload   the struct image, exactly `len` bytes
8+N    2   crc16     over bytes [2, 8+N) — header after sync, plus payload
```

Overhead is 10 bytes per frame.

**CRC-16/CCITT-FALSE**: polynomial `0x1021`, init `0xFFFF`, no input or output
reflection, no final XOR. Check value for `"123456789"` is `0x29B1`.

```python
def crc16_ccitt_false(data: bytes, seed: int = 0xFFFF) -> int:
    crc = seed
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc
```

### Decoder requirements

- **Validate the CRC before acting on any field.** The vehicle does, and a
  ground station that does not will eventually act on line noise.
- **On a rejected candidate, rescan its bytes rather than discarding them.**
  A corrupted `len` makes the parser over-read across a frame boundary, so
  the bytes it consumed may contain the start of a real frame. A decoder that
  discards instead will silently lose the frame behind every corrupted one.
  The vehicle does this too, but bounds the rescan to one level — a candidate
  rejected *during* a rescan is dropped rather than rescanned again, so that
  a single corrupted byte cannot cause unbounded work. A ground station has
  no such constraint and may rescan unconditionally; the two then differ only
  on doubly-corrupted input.
- **Reject any `len` above 128** (`CONFIG_RDD2_CSYN_SERIAL_MAX_PAYLOAD`).
  Accepting more is not permissive: the vehicle rejects an over-long length
  immediately and recovers the frame behind it, so a decoder that waits for
  the bogus payload swallows that frame instead.
- **Do not assume frame alignment with read boundaries.** Feed a byte stream
  to an incremental parser.
- `seq` is a single counter across all topics, so a gap means the link
  dropped a frame, not that one topic went quiet.

## Downlink: what the vehicle sends

Six topics, in both build variants, each rate-limited to **5 Hz**
(`CONFIG_RDD2_CSYN_SERIAL_TX_MIN_INTERVAL_MS=200`). A topic is only
transmitted when its value has changed, so a topic that never updates is
never sent.

| Topic | id | Payload | Frame | Notes |
|---|---|---|---|---|
| `VehicleHealth` | 1 | 48 B | 58 B | arming, mode, RC link |
| `GnssFix` | 8 | 64 B | 74 B | the onboard fix, or the injected one echoed back — see "Build variants" |
| `AttitudeEstimate` | 11 | 40 B | 50 B | estimated attitude and rates |
| `AttitudeCommand` | 19 | 48 B | 58 B | desired attitude and rates |
| `PwmSignalOutputs` | 25 | 48 B | 58 B | motor outputs |
| `ControlLoopMetrics` | 26 | 24 B | 34 B | hot-path timing |

Total ≈ **1.7 KB/s**, about 29% of a 57600 link.

Unknown `topic_id` values must be skipped gracefully — the vehicle's topic
list is application-owned and may grow.

## Payload layouts

Offsets are generated from the firmware's own structs, padding included.

### VehicleHealth — id 1, 48 bytes

```
off  size  type    field
  0     8  u64     timestamp_us              monotonic boot time
  8     4  u32     sensors_present           SensorComponentFlags bitmask
 12     4  u32     sensors_enabled           SensorComponentFlags bitmask
 16     4  u32     sensors_health            SensorComponentFlags bitmask
 20     4  u32     sensors_present_ext       (not populated)
 24     4  u32     sensors_enabled_ext       (not populated)
 28     4  u32     sensors_health_ext        (not populated)
 32     2  u16     load_dpermille            (not populated)
 34     2  u16     voltage_battery_cv        (not populated)
 36     2  i16     current_battery_da        (not populated)
 38     2  u16     drop_rate_comm_cpercent   (not populated)
 40     2  u16     errors_comm               (not populated)
 42     1  i8      battery_remaining_pct     (not populated)
 43     1  u8      vehicle_type              (not populated)
 44     1  u8      flight_mode               0 = ACRO, 1 = AUTO_LEVEL
 45     1  u8      system_state              (not populated)
 46     1  u8      link_quality_pct          RC link quality, 0..100
 47     1  u8      flags                     VehicleHealthFlags bitmask
```

`SensorComponentFlags`: `Gyro=1`, `Accel=2`, `Mag=4`, `AbsolutePressure=8`,
`DifferentialPressure=16`, `Gnss=32`, `OpticalFlow=64`, `VisionPosition=128`,
`Rangefinder=256`, `RadioControl=512`, `MotorOutputs=1024`, `Battery=2048`,
`Estimator=4096`, `Logging=8192`, `CommandLink=16384`, `Terrain=32768`.

RDD2 reports `Gyro | Accel | RadioControl | MotorOutputs | Estimator` as
present and enabled. In `sensors_health`, `MotorOutputs` and `Estimator` are
always set; `Gyro|Accel` are set only while the IMU is delivering samples, and
`RadioControl` only while RC is valid and not stale. **This bitmask is the
authoritative failsafe indicator.**

`VehicleHealthFlags`: `Armed=1`, `Failsafe=2`. RDD2 sets `Armed` only.

### GnssFix — id 8, 64 bytes

```
off  size  type    field
  0     8  u64     timestamp_us              monotonic boot time
  8     8  u64     time_unix_us              valid iff TimeValid
 16     4  i32     latitude_deg_e7           WGS-84 degrees x 1e7
 20     4  i32     longitude_deg_e7          WGS-84 degrees x 1e7
 24     4  i32     altitude_msl_mm           above mean sea level
 28     4  i32     altitude_ellipsoid_mm     above WGS-84 ellipsoid
 32     2  u16     horizontal_accuracy_mm    1-sigma; 65535 = unusable
 34     2  u16     vertical_accuracy_mm      1-sigma; 65535 = unusable
 36     2  u16     velocity_accuracy_mm_s    1-sigma; 65535 = unusable
 38     2  u16     yaw_accuracy_cdeg         (not populated, stays 0)
 40     2  u16     hdop_centi                HDOP x 100
 42     2  u16     vdop_centi                VDOP x 100
 44     2  u16     ground_speed_cm_s
 46     2  u16     course_over_ground_cdeg   0 at true north, CW; iff CourseValid
 48     2  u16     yaw_cdeg                  0 at true north, CW; iff YawValid
 50     2  i16     velocity_up_cm_s          positive up; iff VelocityUpValid
 52     1  u8      flags                     GnssFixFlags bitmask
 53     1  u8      fix_type                  GnssFixType
 54     1  u8      satellites_used
 55     1  u8      satellites_visible
 56     1  u8      id                        receiver instance
 57     7  --      padding
```

`GnssFixFlags`: `TimeValid=1`, `CourseValid=2`, `YawValid=4`,
`VelocityUpValid=8`.

`GnssFixType`: `0 NoFix`, `1 TimeOnly`, `2 Fix2d`, `3 Fix3d`, `4 Dgnss`,
`5 RtkFloat`, `6 RtkFixed`, `7 DeadReckoning`.

**There is no position-valid flag — `fix_type` is the only signal, and the
vehicle publishes samples while the receiver has no lock.** A `NoFix` or
`TimeOnly` sample carries meaningless latitude, longitude and altitude, often
zero, which plots as a position off West Africa. Treat any sample with
`fix_type < 2` as having no position at all; the flags in `flags` say nothing
about it. Publishing those samples is deliberate: it distinguishes "receiver
alive, still acquiring" from "receiver silent".

**The three position/velocity accuracy fields are `65535` when produced by the
onboard receiver** — `horizontal_accuracy_mm`, `vertical_accuracy_mm` and
`velocity_accuracy_mm_s`. The generic NMEA driver reports no accuracy, and the
schema defines 65535 as "at or above 65.535 m, unusable". Do not read that as
a large-but-real figure.

`yaw_accuracy_cdeg` is the exception: it is **left at 0**, not saturated, so it
must not be read as a perfect heading accuracy. `vdop_centi`,
`velocity_up_cm_s`, `yaw_cdeg` and `satellites_visible` are likewise
unpopulated by the NMEA path and stay 0. For all of these, trust the validity
flags and `fix_type`, never the value.

### AttitudeEstimate — id 11, 40 bytes

```
off  size  type    field
  0     8  u64     timestamp_us
  8    16  f32[4]  attitude                  quaternion w, x, y, z (in that order)
 24    12  f32[3]  angular_velocity_flu_rad_s  roll, pitch, yaw rate
 36     1  u8      flags                     AttitudeEstimateFlags
```

`AttitudeEstimateFlags`: `AttitudeValid=1`, `RatesValid=2`. Both are set
together while the IMU is healthy, and both clear otherwise.

**Attitude is a quaternion, not Euler angles** — the firmware converts from
its internal Euler state before publishing. Component order is `w, x, y, z`.
Angular velocity is body-frame FLU in rad/s and comes straight from the gyro.

### AttitudeCommand — id 19, 48 bytes

```
off  size  type    field
  0     8  u64     timestamp_us
  8    16  f32[4]  attitude                  desired quaternion w, x, y, z
 24    12  f32[3]  body_rate_flu_rad_s       desired roll, pitch, yaw rate
 36     4  f32     thrust                    (not populated)
 40     1  u8      type_mask                 (not populated)
```

Pair with `AttitudeEstimate` to plot desired against actual.

### PwmSignalOutputs — id 25, 48 bytes

```
off  size  type    field
  0     8  u64     timestamp_us
  8     4  u32     active_mask               0x0F: outputs 0..3 in use
 12     1  u8      port                      (not populated)
 13     1  --      padding
 14     2  u16     output0_us                1000..2000 us
 16     2  u16     output1_us
 18     2  u16     output2_us
 20     2  u16     output3_us
 22    24  u16[12] output4_us .. output15_us (not populated)
 46     2  --      padding
```

Note the pad byte at offset 13: `output0_us` starts at **14**, not 13.
Quad-X only, so outputs 4..15 are always zero. Values are 1000 µs when
disarmed.

### ControlLoopMetrics — id 26, 24 bytes

```
off  size  type    field
  0     8  u64     timestamp_us
  8     4  u32     period_us                 always 625 (1600 Hz)
 12     4  u32     latency_us                IMU interrupt -> DSHOT trigger
 16     4  u32     overrun_count             (not populated)
 20     2  u16     load_dpermille            (not populated)
```

`latency_us` is the measured hot-path latency and the most useful number on
the link for tuning. It is 0 under lockstep simulation.

## Fields that are never populated

Marked `(not populated)` above and always zero. A ground station must not
render them as real data:

- **All battery and power telemetry** — voltage, current, remaining percent.
  There is no battery monitor in the vehicle's published state.
- **CPU load**, in both `VehicleHealth` and `ControlLoopMetrics`.
- **Comm drop rate and error counters.**
- **`vehicle_type`, `system_state`** — use `flight_mode` and the `Armed` flag.
- **`thrust` and `type_mask`** in `AttitudeCommand`.
- **`overrun_count`.**

## Uplink: what the ground station may send

The vehicle accepts frames only for topics it declares as inbound; anything
else is counted and dropped. In the default build the inbound set is:

| Topic | id | Payload | Purpose |
|---|---|---|---|
| `ManualControlCommand` | 4 | 40 B | pilot input |
| `InertialSample` | 5 | 56 B | IMU injection, simulation only |

`GnssFix` (id 8) is inbound **only** in the mocap build variant, where the
vehicle accepts an externally supplied fix. In the default build `GnssFix` is
outbound and an injected fix is rejected.

Payload length must exactly match the catalog size for fixed-layout topics;
short frames are rejected rather than zero-extended.

## Build variants

The vehicle chooses its GNSS source at build time, and the direction of the
`gnss` topic follows:

| Vehicle build | `GnssFix` direction | Effect on the ground station |
|---|---|---|
| default (onboard receiver) | outbound | GNSS arrives as telemetry; injection rejected |
| `-S mocap-gnss` | inbound, **echoed back** | GNSS must be supplied; the accepted fix is telemetered |

`GnssFix` therefore arrives in both builds, and a ground station can treat it
as the vehicle's position without knowing which one it is talking to. What
differs is the meaning: outbound it is what the receiver measured, echoed it
is what the vehicle accepted from the ground station.

The echo exists because in a mocap build there is no onboard receiver and no
estimator downstream of the fix, so the injected position is the only one the
vehicle holds. It is rate-limited and change-detected like any other outbound
topic, so it costs one frame per TX interval rather than one per injection.

**Do not re-inject an echoed fix.** The transport cannot tell an echoed frame
from a fresh one, so feeding telemetry back into the uplink closes a loop it
has no way to break. A ground station that both injects and displays GNSS must
keep the two paths separate.

## Diagnosing from the vehicle side

The vehicle shell reports link counters with `csyn_serial status`:

```
port=uart@40190000 baud=57600 ready=yes init_rc=0
rx  frames=60 crc_err=0 bad_len=0 unknown=0 wrong_dir=0
rx  seq_gaps=0 ring_overrun=0
tx  frames=41 dropped=0 oversize=0
```

`unknown` counts frames for topics the vehicle does not declare;
`wrong_dir` counts frames for a topic declared in the other direction — that
is the counter that moves when a ground station injects GNSS into a vehicle
built with the onboard receiver.
