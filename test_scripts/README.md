# test_scripts

Bring-up scripts for talking to the vehicle from a laptop. Not part of any
build; the wire format is imported from `tools/synapse_serial/` so these never
carry a second copy of it.

**These need the vehicle built with `-S mocap-gnss`.** The default image takes
the `gnss_fix` topic from the onboard receiver, which is then its only
publisher and the radio carries it outbound, so an injected fix is rejected
into the `wrong_dir` counter rather than accepted. The snippet leaves the
onboard driver out and makes the serial transport the topic's publisher, which
is what lets a fix arrive over the radio.

## send_fake_gps.py

Streams a fake GNSS fix over the SiK telemetry radio, to answer "is the drone
receiving anything at all?" without a real GPS.

```sh
./send_fake_gps.py                     # /dev/ttyUSB0 at 57600
./send_fake_gps.py /dev/ttyACM0
./send_fake_gps.py /dev/ttyUSB0 --baud 115200 --rate 10
```

The position walks slowly north, so a frozen latitude on the drone means
stale data rather than a live link. `--still` disables that.

On the drone shell:

```
zros_serial status         # rx frames= climbing
zros topic echo gnss_fix   # latitude ticking upward
```

A healthy run looks like this, with every counter but `frames` at zero:

```
port=uart_1 baud=57600 ready=yes init_rc=0
rx  frames=60 crc_err=0 bad_len=0 unknown=0 wrong_dir=0
rx  seq_gaps=0 ring_overrun=0
```

The script also decodes anything the drone sends back and reports it on exit,
which confirms the radio pair works in both directions.

## publish_gps_synapse.py

The synapse counterpart of `publish_gps.py`. Same ROS 2 node shape, same
odometry handling, frame conventions and geodetic math — so an origin and
`yaw_offset` calibrated against the MAVLink version carry over unchanged. The
only difference is what goes on the wire: a bare 64-byte `GnssFixData` in the
compact synapse framing instead of MAVLink `GPS_INPUT` (#232).

```sh
python3 publish_gps_synapse.py --ros-args \
    -p port:=/dev/ttyUSB0 -p baud:=57600 \
    -p odom_topic:=/drone/odom \
    -p origin_lat:=40.41535 -p origin_lon:=-86.93291

python3 publish_gps_synapse.py --ros-args -p dry_run:=true   # no serial port
```

Parameters shared with `publish_gps.py`: `port`, `baud`, `odom_topic`,
`origin_lat/lon/alt`, `rate_hz`, `dry_run`, `frame` (`enu`/`flu`),
`yaw_offset`, `heading_offset`. Added: `publish_yaw` (default false, matching
the MAVLink script which never sent yaw), `hdop`, `vdop`, `satellites`, and
`gnss_instance`.

Two things differ from the MAVLink path, both forced by the schema:

- **`GnssFixData` has no NED velocity vector.** It carries ground speed,
  course over ground and vertical velocity, so `vn`/`ve` are folded into
  speed plus a course of `atan2(east, north)` — receiver-native, zero at
  true north, positive clockwise. `vd` becomes `velocity_up_cm_s`, negated.
- **There is no heartbeat to wait for.** Instead the node decodes whatever
  the vehicle streams back and reports it as `from vehicle: pwm=…, health=…`
  in the throttled log line, which is the equivalent link-health signal.

Course is only marked valid above 0.15 m/s, since a heading differenced from
a stationary position is noise.

## publish_gps_zenoh.py

Same job, but the input is the synapse wire format instead of ROS 2: the
fixed-layout payload structs [synapse_fbs](https://github.com/CogniPilot/synapse_fbs)
defines, as a mocap bridge publishes them on Zenoh under
`[<namespace>/]<key>[/<instance>]`. What goes *out* is byte-for-byte what
`publish_gps_synapse.py` sends, so the vehicle side and the calibration carry
over unchanged.

```sh
./.venv/bin/python publish_gps_zenoh.py --port /dev/ttyUSB0
./.venv/bin/python publish_gps_zenoh.py --port /dev/ttyUSB0 --namespace cub1 --instance 0
./.venv/bin/python publish_gps_zenoh.py --topic mocap --instance 7 --dry-run
./.venv/bin/python publish_gps_zenoh.py --simulate --dry-run   # no mocap and no radio
./.venv/bin/python publish_gps_zenoh.py --selftest             # no zenoh needed
```

### Dependencies

Neither `eclipse-zenoh` nor `synapse-fbs` is in nixpkgs, so these two scripts
are the one thing here that does not come from the flake. On Debian and Ubuntu
a plain `pip install` into the system Python fails with
`error: externally-managed-environment` (PEP 668), so use a virtualenv:

```sh
python3 -m venv .venv
./.venv/bin/pip install eclipse-zenoh synapse-fbs pyserial
```

`.venv/` is gitignored. Nothing here needs ROS, so the venv does not need
`--system-site-packages`: `publish_gps_synapse.py` is imported only for its
geodetic math, and it guards its own rclpy import.

`--list-topics` prints what it can subscribe to:

| `--topic` | payload | notes |
|---|---|---|
| `external_pose` | `ExternalOdometryData` | the estimator-input contract a bridge writes into the vehicle namespace. Carries velocity and tracking status. Default. |
| `odom` | `OdometryData` | a filtered estimate; same content |
| `pose` | `PoseData` | filtered pose, no velocity |
| `pose_raw` | `RawPoseData` | unfiltered source pose, no velocity |
| `mocap` | `MocapPoseFrame` | raw mocap frame; `--instance` picks a rigid body |

`mocap.fbs` calls `MocapPoseFrame` a debugging and bridge-level type and points
estimators at `ExternalOdometry`, so prefer `external_pose` when a bridge is
running. The `mocap` path is for when one is not.

Three things it does that the ROS version cannot, all because the schema
carries information `nav_msgs/Odometry` does not:

- **Velocity is read, not differenced.** `ExternalOdometryData` carries
  `linear_velocity_enu_m_s` with a validity bit. Differencing is still there as
  the fallback for `pose`, `pose_raw` and `mocap`.
- **Tracking status reaches the vehicle.** `Lost` — or no sample for
  `--timeout` seconds — is sent as `fix_type=NoFix` with accuracy saturated to
  65535 mm, which the schema documents as "unusable". `Extrapolated`,
  `OutlierRejected` and `Degraded` multiply the accuracy by
  `--degraded-scale`. A frozen position sent at full confidence would be worse
  than no fix, because the estimator has no way to tell it is stale.
- **Accuracy can be real.** With `--topic external_pose` it also subscribes to
  `external_pose_cov` and takes the horizontal and vertical accuracy from the
  position block of the covariance. It falls back to `--hacc`/`--vacc` when
  nothing arrives there; `--no-covariance` turns it off.

There is no `frame` parameter, unlike the ROS script. The synapse types name
their frame in the field (`position_enu_m`, `angular_velocity_flu_rad_s`) and
the schema requires bridges to transform before publishing, so the input is
ENU by contract and cannot be FLU.

`--namespace` defaults to `**`, which matches any namespace, and the status
line reports the key each sample actually arrived on — so start broad, then
pin it down once you can see what is publishing.

### Version skew

The firmware pins synapse_fbs 0.7.0 and the Python package is versioned
separately, so the script compares the *schema set hash* rather than the
release number and refuses to start on a mismatch. Both are
`2fd857effb6c7558d6869f4307a5354c` today. Left unchecked this is the fault
that shows up on the vehicle as a climbing `bad_len` counter.

`--selftest` needs neither zenoh nor a radio and checks that hash, every
payload size against the catalog, and the decoder against a reference buffer
generated by `flatc`.

## fake_mocap_zenoh.py

The zenoh-side counterpart of `send_fake_gps.py`. It publishes a synthetic
circular trajectory as `ExternalOdometryData`, with a matching
`external_pose_cov`, so the bridge can be brought up before the real mocap
bridge exists — and so "is my key expression right, and is zenoh routing
between these two hosts?" can be answered on its own.

```sh
./.venv/bin/python fake_mocap_zenoh.py                  # cub1/external_pose/0 at 50 Hz
./.venv/bin/python fake_mocap_zenoh.py --namespace field_lab/cub1 --instance 2
./.venv/bin/python fake_mocap_zenoh.py --lost-every 10  # one-second Lost bursts
```

and in another terminal:

```sh
./.venv/bin/python publish_gps_zenoh.py --namespace cub1 --dry-run
```

`--lost-every` is the one worth running before a flight: it is how you see what
the vehicle does when tracking drops, without having to walk a rigid body out
of the capture volume.

Use `publish_gps_zenoh.py --simulate` instead when the question is only about
the conversion — it generates the same trajectory in-process and needs no
zenoh session at all.

## zenoh_scan.py

The zenoh-side counterpart of `zros_serial scan`: it reports what is
publishing, so you can find the right `--namespace` and `--topic` instead of
guessing at them one bridge restart at a time.

```sh
./.venv/bin/python zenoh_scan.py --connect tcp/192.168.1.10:7447
./.venv/bin/python zenoh_scan.py                       # peer mode, local network
./.venv/bin/python zenoh_scan.py --key 'cub1/**' --duration 15
```

```
session fb32eb827dc083a9  mode=client
routers: none
peers  : 8470f0f220a7f559

listening on ** for 4s...

key                                topic                  n     rate  bytes  note
cub1/external_pose/0               external_pose/0      197   49.2/s     64  ExternalOdometryData
cub1/external_pose_cov/0           external_pose_cov/0  197   49.2/s    328  ExternalOdometryCovarianceData

point the bridge at one of these, for example:
  ./publish_gps_zenoh.py --namespace cub1 --topic external_pose --instance 0 --dry-run
```

It separates the same two failure modes the radio scan does — whether the
session reached anything at all, and whether anything is publishing once it
has — because the fixes are completely different:

| Symptom | Meaning |
|---|---|
| `zenoh.open` fails outright | In client mode an unreachable endpoint fails immediately. Wrong host, wrong port, firewall, or `zenohd` is not running. |
| `routers: none  peers: none` | The session came up but nothing is linked. In peer mode with no `--connect`, multicast scouting found nothing — it rarely crosses subnets or VPNs. |
| Linked, but no keys listed | The network is fine and nothing is publishing. Check the mocap bridge itself. |
| `SIZE MISMATCH` in the note | The key is a known synapse topic but the payload is the wrong length — a schema skew between publisher and this catalog, the ground-side twin of the vehicle's `bad_len`. |

`--mode` defaults to `client` when `--connect` is given and `peer` otherwise,
which is also how `publish_gps_zenoh.py` behaves. Both take the same
`--connect`, so once the scan shows a topic the printed command works as-is.

## Finding which port the radio is on

If you do not know which TELEM header the SiK is plugged into, do not rebuild
once per guess. `zros_serial scan` listens on every free UART at the same time
and reports where the bytes land.

On the drone shell:

```
zros_serial scan 10 57600      # seconds, baud (both optional)
```

While it counts down, start the sender on the laptop. A hit looks like:

```
alias    node                    bytes   sync  verdict
zros-scan0 uart@4018c000             0      0  silent
zros-scan1 uart@40190000          2220     30  SYNAPSE FRAMES -- point zros-serial here
zros-scan2 uart@40194000             0      0  silent
```

`bytes` with no `sync` means the link is alive but the framing is not being
recognised — nearly always a baud mismatch, so rerun the scan at another rate.

The candidates on `mr_vmu_tropic`:

| alias | UART | node | notes |
|---|---|---|---|
| `zros-scan0` | `lpuart3` | `uart@4018c000` | |
| `zros-scan1` | `lpuart4` | `uart@40190000` | current `zros-serial` port |
| `zros-scan2` | `lpuart5` | `uart@40194000` | |
| `zros-scan3` | `lpuart2` | `uart@40188000` | GPS connector — **`-S mocap-gnss` builds only** |

Only UARTs that no driver owns may be scanned, because the scan reconfigures
the baud and polls for bytes. `lpuart6` (console) and `lpuart8` (CRSF) are
therefore always excluded, and `lpuart2` appears only in `mocap-gnss` builds,
where the GNSS receiver is not compiled in and the port is free.

Once you know the port, set `zros-serial` to it in
`boards/mr_vmu_tropic.overlay` and rebuild. Scanning the port the transport
already owns is safe: the scan borrows RX interrupts for its duration and
hands them back, so the link keeps working afterwards.

## When nothing arrives

Read `zros_serial status` first — the counters separate the failure modes:

| Symptom | Meaning |
|---|---|
| `ready=no`, non-zero `init_rc` | Transport never started. `-19` is `ENODEV`: the UART behind the `zros-serial` alias is not present or not ready. Nothing else matters until this is fixed. |
| Everything stays `0` | No bytes reaching the MCU at all. Wrong UART, wrong wiring, or the radios are not paired. |
| `crc_err` climbing, `frames` at 0 | Bytes are arriving but garbled — almost always a baud mismatch between the drone's `current-speed` and the radio. |
| `ring_overrun` climbing | Bytes arriving faster than the transport drains them. Lower `--rate`, or raise `CONFIG_RDD2_ZROS_SERIAL_RX_RING_SIZE`. |
| `unknown` climbing | Frames are intact, but this link does not carry that topic. The set it carries is the table at the top of `subsys/zros_serial/zros_serial.c`. |
| `wrong_dir` climbing | The link carries that topic, but outbound only. Injecting `gnss` needs `-S mocap-gnss`, which is what makes the transport its publisher. |
| `bad_len` climbing | Length field disagrees with the catalog payload size — usually a schema version mismatch between the two ends. |

Three things to check on the hardware itself:

1. **Which UART.** `boards/mr_vmu_tropic.overlay` aliases `zros-serial` to
   `lpuart4`. If the radio is on a different TELEM header, repoint it at
   `lpuart3` or `lpuart5` and rebuild.
2. **Baud agreement.** Three places must match: both SiK radios, and
   `current-speed` on the aliased UART. SiK ships at 57600, and all three
   TELEM candidates are set to it so moving the alias cannot silently leave
   the transport on a different rate.
3. **SiK pairing.** Both radios need the same NETID and air rate, and the
   green LED should be solid once they are linked. A blinking LED means no
   peer, and nothing will cross the link.

## Testing without a radio

`tools/synapse_serial/README.md` describes a `native_sim` build that exposes
the transport on a pseudoterminal, so `send_fake_gps.py` can be pointed at
`/dev/pts/N` and exercise the real firmware parser with no hardware at all.
