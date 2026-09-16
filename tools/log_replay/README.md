# Flight log replay

Host replay of the RDD2 navigation estimator eFMU on a recorded flight log.
The same generated production code that flies is stepped from the logged IMU,
GNSS and optical-flow streams through the same adapters the firmware uses, so
a log can be re-run against any estimator export without a vehicle.

## Steps

1. Decode the MCAP log into the interchange CSV set:

       ./mcap_to_interchange.py ~/flight0114.mcap replay_input [--fill-gaps]

   `--fill-gaps` bridges IMU dropouts by linear interpolation at the nominal
   800 Hz spacing. The logger drops several thousand frames at every session
   rotation, and the preintegrator discards any sample interval longer than
   20 ms, so the raw log loses rotation across those bursts.

2. Build the replay against the estimator export in modelica_models:

       MODELICA_MODELS=~/git/modelica_models ./build.sh

   `EFMU_PRODUCTION_CODE` overrides the ProductionCode directory. The estimator
   wiring (IMU preintegration, GNSS and optical-flow adapters) and the
   interface topic structs are compiled directly from the repository sources
   under `src/processes` and `src/interfaces`, so the replay runs the exact
   code the firmware carries; nothing is vendored.

   The adapters consume the fixed-layout synapse topic structs from the
   synapse_fbs C headers. `build.sh` takes the header directory from
   `RDD2_SYNAPSE_FBS_ROOT` (the variable the flake apps export, pointing at the
   `synapse_fbs-c` package that contains `include/synapse`) and falls back to
   the devenv-provisioned package at
   `.devenv/state/synapse_fbs-build/synapse_fbs-c`. If neither is present the
   build exits with an error; provision it with `scripts/synapse_fbs_package`
   or enter the flake dev shell first.

3. Run:

       ./replay --input replay_input --output estimate.csv [--no-flow] [--no-gps] \
           [--gps-deny T0:T1]... [--divisor N] [--accel-noise Q] [--gyro-noise Q] \
           [--gyro-bias-noise Q] [--accel-bias-noise Q] [--init-gyro-bias-var V] \
           [--init-accel-bias-var V] [--init-att-var V] [--gate G]

   The estimator is released every `--divisor` IMU samples (default 8, the
   100 Hz flight rate). The noise and variance options override the block's
   tunable parameters before `recalibrate`, everything else is the flight
   configuration. `--gps-deny` withholds fixes with T0 <= t < T1.

## Interchange format

All files are CSV with a header row; `t_s` is seconds on the flight
controller boot clock. Body frame is FLU, world frame is ENU about
`origin.json`.

- `imu.csv`: `t_s,gx_rad_s,gy_rad_s,gz_rad_s,ax_m_s2,ay_m_s2,az_m_s2`
- `gps.csv`: `t_s,lat_deg,lon_deg,alt_msl_m,alt_ell_m,vn_m_s,ve_m_s,vd_m_s,hacc_m,vacc_m,sacc_m_s,fix_type,sats_used,pos_valid,vel_valid,vd_derived,e_m,n_m,u_m`.
  The receiver reports course and ground speed only; `vd_m_s` is a smoothed
  finite difference of altitude and `vd_derived` marks that.
- `flow.csv`: `t_s,vx_flu_m_s,vy_flu_m_s,dist_m,quality,valid`
- `onboard.csv`: `t_s,qw,qx,qy,qz,wx,wy,wz`, the attitude the vehicle
  published, for reference.

Wire topics (GNSS, flow) come from another node: while it is gPTP synced their
timestamps are mapped onto the boot clock through the logged TimeReference
samples, before that the logger accept time less the measured transport
latency is used.

The output `estimate.csv` carries one row per estimator release with position,
velocity, quaternion (body FLU to world ENU), Euler angles, bias states, the
validity flags the firmware wrapper would publish, and the block's status
outputs (recovery stage, correction outcome and source, anchor, NIS, rejection
counters, position, velocity and attitude sigmas).

## Wire replay (feed the firmware over UDP/IPv6)

`wire_replay.py` feeds the RDD2 firmware its GNSS and optical-flow inputs the
way the vehicle receives them in flight: as Synapse wire frames over UDP/IPv6,
into the direct receiver in `subsys/synapse_wire/synapse_wire.c`. It reads the
`gnss_fix` and `optical_flow_vel` channels from a log, re-wraps each recorded
payload struct in a fresh wire header, re-stamps the timestamps, and sends the
frames paced by the log accept times.

    ./wire_replay.py LOG.mcap --dest ADDR [--gnss-port N] [--flow-port N] \
        [--rate 1.0] [--start T0] [--end T1] [--time-base freerun|synced] \
        [--no-gnss] [--no-flow] [--dry-run] [--imu-out FILE]

- `--dest` is the receiver's IPv6 address. For a real target on a link-local
  address add the interface scope, e.g. `fe80::4:9fff:fe00:150%eth0`.
- `--gnss-port` / `--flow-port` default to 46008 and 46010, the
  `subsys/synapse_wire/Kconfig` and `boards/mr_vmu_tropic.conf` values. The
  source node ids (11 GNSS, 12 flow), topic ids (8 GNSS, 10 flow) and schema
  set id are fixed to the same contract.
- `--rate` scales the send pace (`--rate 240` replays 240x real time).
- `--start` / `--end` keep only frames in `[T0, T1]` seconds measured from the
  first replayed frame's log accept time.
- `--dry-run` writes the datagrams, length-prefixed (`u16` LE length + bytes),
  to `<log>.wireframes` and prints a per-stream summary instead of sending.

### Frame layout

Each datagram is the 44-byte big-endian Synapse wire v1 header
(`synapse/wire.h`) followed by the bare fixed-layout payload struct. Byte
offsets in the header:

    off  size  field
      0     4  magic 0x53594E57 ("SYNW"), big-endian
      4     2  wire_protocol_version = 1
      6     2  topic_id (8 gnss_fix, 10 optical_flow_vel)
      8     4  source_node_id (11 gnss, 12 flow)
     12     4  sequence (per stream, from 1, contiguous)
     16     8  capture_timestamp_ns
     24     8  schema_set_id = 0x232721F0EE5B6C32
     32     2  payload_length (64 gnss, 32 flow)
     34     2  flags
     36     8  source_session_id (one fresh random id per run)
     44   ...  payload struct (GnssFixData 64 B / OpticalFlowVelocityData 32 B)

The payload is copied verbatim from the log except two fields: `timestamp_ns`
(offset 0) and the `time_status` byte (offset 56 in GnssFixData, 30 in
OpticalFlowVelocityData) are rewritten by the stamping rule below.

### Stamping rule

The recorded payloads carry another node's clock: in this log the GNSS/flow
producer runs about 0.35 s ahead of the flight-controller boot clock and never
gPTP-syncs, so the log timestamps cannot be replayed verbatim.

`--time-base freerun` (default) stamps the receiver's freerun path:
`flags = 0`, `capture_timestamp_ns = 0`, payload `time_status = LocalFreerun`,
and payload `timestamp_ns` = the frame's log accept time rebased so the first
frame is near zero (a boot-relative monotonic clock, clamped non-zero). The
wire receiver accepts this because, with the gPTP flag clear, `header_flags_valid`
requires exactly `capture_timestamp_ns == 0`; `header_payload_time_valid`
requires the header and payload to agree that they are unsynced (they do); and
`evaluate_freshness` skips the capture-time age check entirely when the header
is not gPTP-synced (or the receiver is not), so the sample is always fresh
against `receive_monotonic_ns`. The GPS adapter (`navigation_gps.c`) then
compares `fix->timestamp_ns` against the IMU boot clock: a boot-relative stamp
started at replay start matches a native_sim/QEMU target booted at replay
start, landing inside the 500 ms staleness window (`GPS_MAX_AGE_NS`).

`--time-base synced` stamps the gPTP path: `flags` sets
`CAPTURE_TIME_GPTP_SYNCED`, `capture_timestamp_ns == payload timestamp_ns`, and
`time_status = GptpSynced`, with the timestamp taken from the sender's Unix
clock. Use this only against a target disciplined to the same Unix epoch; a
freerun native_sim will wire-accept it but the GPS adapter will read it as far
in the future and drop it.

### IMU sensor-frame export

`--imu-out FILE` writes the 800 Hz `control_imu` samples with the FLU body
mapping from `src/interfaces/imu.c` undone, so the values are what the
ICM45686 produces in its own sensor axes (still SI units, only the axis
permutation is inverted): `sensor.x = -body.y`, `sensor.y = body.x`,
`sensor.z = body.z` for both accelerometer and gyroscope. This feeds the
peripheral model that drives the SPI IMU.

If `FILE` ends in `.csv` a header row plus
`t_ns,ax_m_s2,ay_m_s2,az_m_s2,gx_rad_s,gy_rad_s,gz_rad_s,temp_c` are written.
Otherwise the output is a packed little-endian binary stream of 36-byte
records, one per sample, no header:

    off  size  type     field
      0     8  u64      t_ns (ns since the first IMU sample)
      8     4  f32      ax_m_s2   (sensor frame)
     12     4  f32      ay_m_s2
     16     4  f32      az_m_s2
     20     4  f32      gx_rad_s  (sensor frame)
     24     4  f32      gy_rad_s
     28     4  f32      gz_rad_s
     32     4  f32      temp_c

### Self-test over loopback

`wire_capture.py` binds the same two ports and prints decoded frames; use it to
self-test the pair without a target:

    ./wire_capture.py --bind ::1 --quiet --timeout 4 &
    ./wire_replay.py LOG.mcap --dest ::1 --rate 240

The capturer reports per-stream received/accepted counts, sequence gaps and the
session id, which must match the sender. It decodes and counts only; it does not
run the firmware's carrier, binding, freshness or payload validation.

### Against native_sim or QEMU

The firmware receiver binds a link-local IPv6 address on VLAN 58 and rejects
any datagram whose IPv6 hop limit is not 1, whose source address/port is not
the configured node's, or whose destination is not the receiver's address
(`carrier_observe` / `binding_valid`). `wire_replay.py` pins the hop limit to 1
and, for a non-loopback destination, binds its source port to the stream port;
the remaining carrier and binding checks require the emulator's networking
(a `zeth`/TAP interface carrying VLAN 58 with the source and destination
link-local addresses) to be provisioned so the source address and interface
match. Start the target first, then start the replay promptly so the
boot-relative GNSS stamps stay inside the GPS adapter's staleness window.

## Raw optical-flow bench (indoor, no GPS)

`flow_bench.py` checks the flow node's raw product (the `optical_flow` channel,
synapse `OpticalFlowData`, decoded to `flow_raw.csv`) against the flight
controller IMU, so the axis convention, sign, gyro compensation and
sensitivity of the raw channel can be verified indoors with no position
reference. It is meant for a hand-carry test over a textured floor at roughly
1 m range.

It reads `flow_raw.csv` and `imu.csv` from an interchange directory (or decodes
an MCAP first). Both streams are on the flight-controller control-IMU boot
clock: `mcap_to_interchange.py` re-stamps the wire flow payloads onto that clock
(`wire_to_boot`) and rebases IMU and flow to the first IMU sample, so the flow
window times and the IMU times share one domain and are differenced directly.

The check follows the convention in `src/processes/navigation_optical_flow_raw.c`:
`flow_rad` is the integrated angular image flow in body FLU in the same
rotational sense as the body rotation, `delta_angle_flu` is the genuine
`+integral` of the body rate, the translation flow is `tau = flow_rad -
delta_angle` (x, y), and body velocity is `v_forward = range*tau_y/dt`,
`v_left = -range*tau_x/dt`. So a pure rotation gives `flow_rad = delta_angle`
(tau near zero), a forward `+x` translation gives `flow_rad.y > 0`, and a left
`+y` translation gives `flow_rad.x < 0`. The node forms `flow_rad` from PAA3905
counts through `VOF_SENS` (480.24 counts/rad); a sensitivity error there shows
as a distance-scale error on the walk.

### Test procedure

Record one log with the vehicle level, about 1 m over a textured floor. Hold a
few seconds of rest between segments; each moving segment 10 to 20 s.

- A. static rest.
- B. pure translation forward along body `+x` and back, walking pace, no
  rotation.
- C. pure translation left along body `+y` and back.
- D. rotation in place about body `z` (yaw), both directions.
- E. rotation in place about body `x` (roll) and about `y` (pitch), small
  amplitude, both directions, with a short rest between roll and pitch.
- F. a hallway walk of a known paced length, out and back; note the length in
  metres.
- G. optional: the same walk at a different height.

### Run

    nix shell --impure --expr 'with import <nixpkgs> {}; \
        python3.withPackages (ps: [ps.numpy ps.matplotlib ps.scipy ps.pandas])' \
        -c python3 ./flow_bench.py INPUT [--walk-length METRES] [options]

`INPUT` is an interchange directory or an MCAP log (decoded first with
`--fill-gaps`). Options:

- `--walk-length METRES` the paced length of segment F, for the scale check.
- `--walk-segment LABEL` which segment is the hallway walk (default: the
  longest translation segment).
- `--segment LABEL:T0:T1` an explicit segment override (repeatable); when any
  is given it replaces the automatic segmentation. A label containing `roll`,
  `pitch`, `yaw`, `rot` or `spin` is treated as rotation, one containing `rest`,
  `static` or `still` as rest, otherwise as translation. A clean
  single-direction translation window gives the crispest axis map.
- segmentation tuning: `--rest-gyro`, `--rest-speed`, `--rot-gyro`,
  `--trans-speed` (thresholds), `--min-seg` (minimum duration), `--smooth`
  (feature smoothing) and `--bridge` (gap-close radius that keeps an
  oscillating rotation one segment).
- lag sweep: `--lag-min-ms`, `--lag-max-ms`, `--lag-step-ms` (default -200 to
  +200 ms in 5 ms steps).

### Output

A timeline of the detected segments, then per analysis:

- rotation: for each rotation segment the best flow-to-IMU lag, the node's own
  gyro gain on the rotated axis (node `delta_angle` against the FC gyro
  integrated over each flow window), and for an in-plane roll or pitch the
  flow-vs-gyro gain and cross-axis term. The compensation verdict flags
  consistent, doubled, missing, sign-flipped or cross-leaked, and the residual
  is reported before and after the adapter compensation (`tau = flow_rad -
  delta_angle`), which should approach zero for a consistent in-plane rotation.
  For a yaw the in-plane flow should stay near zero.
- translation: the best 2x2 signed-permutation map from body velocity (from the
  high-pass accelerometer-integrated velocity) to the flow axes, with the fitted
  scale and residual for all eight candidates, whether it matches the adapter
  convention and whether a reflection (mirrored axis) is present.
- hallway walk: the compensated flow times range integrated into a body-frame
  distance, compared with `--walk-length` to give the sensitivity correction
  factor and the implied `VOF_SENS`, plus a dead-reckoned 2D track (yaw from the
  integrated gyro) written to `flow_bench_track.csv` and `flow_bench_track.png`.
- static: flow noise, the rate of false motion, and quality and range
  statistics over the rest segments.

A summary is written to `flow_bench_summary.json` and a text report to
`flow_bench_report.txt`. The tool exits nonzero when `flow_raw.csv` is absent,
which means the node did not send the raw `OpticalFlowData` topic.

The translation axis map relies on the accelerometer-integrated velocity as its
direction reference, which is valid only while the vehicle stays roughly level
(so gravity stays on the z axis). It is intended for the level hand-carry above;
on data with sustained tilts the body-velocity reference degrades and the map
candidates tie, at which point the segment overrides and the rotation and walk
checks remain the reliable diagnostics.
