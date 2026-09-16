# Same-binary BIL harness (flight image under FastDyn/QEMU)

Tools for running the unmodified MR-VMU-Tropic flight image
(`build-mr_vmu_tropic-flight-bil`) under FastDyn/QEMU and driving it with the
recorded flight0114 sensors.

- `run_flight_image.sh` boots the flight image with the ICM45686 inertial path.
- `ram_tap.py` reads the navigation estimate from the file-backed OCRAM/DTCM
  memory banks by resolving `g_msg_navigation_odometry` /
  `g_msg_attitude_estimate` from the ELF, and writes the interchange
  `estimate.csv` schema. It is a memory tap, not a transport.
- `wire_bridge.py` wraps the wire replayer's UDP payloads into raw Ethernet +
  802.1Q VLAN 58 + IPv6 + UDP frames and hands them to the ENET model over a
  privilege-free AF_UNIX datagram socket.

## IMU stream

Set `BIL_ICM_SAMPLE_FILE` to a 36-byte-record raw sensor stream produced by
`tools/log_replay/wire_replay.py --imu-out`. The ICM45686 model replays it at
the 800 Hz ODR paced in guest time.

## GNSS and optical-flow over the wire

The flight image receives GNSS and flow only as Synapse wire frames over
UDP/IPv6 on the VLAN 58 ENET interface (`subsys/synapse_wire`,
`boards/mr_vmu_tropic.conf`). Two host paths deliver them.

### Least-privilege socket path (no root)

Point the ENET model at an AF_UNIX datagram backend and run the bridge:

    export FASTDYN_ENET_TAP="unix:/tmp/rdd2_enet_g.sock:/tmp/rdd2_enet_h.sock"
    # (launch the flight image so the model binds the guest socket)
    python3 tools/bil_flight/wire_bridge.py &
    python3 tools/log_replay/wire_replay.py LOG.mcap --dest ::1 --rate R

The bridge builds each frame with the addresses the receiver enforces:
dst MAC = guest ENET MAC (ENET PALR/PAUR, default 02:04:9f:00:00:00), source
MACs/IPv6 = the GNSS/flow node link-local addresses, VLAN 58, hop limit 1, and
UDP source port equal to the destination port. Pace the replayer at the guest
time rate (`--rate` = guest-seconds per wall-second) so the fixes stay inside
the GPS adapter's age window.

### Pre-provisioned TAP path (one-time root)

If a kernel TAP is preferred, create it once, then leave `FASTDYN_ENET_TAP`
unset (the model defaults to the `enet` TAP):

    sudo ip tuntap add dev enet mode tap user "$USER"
    sudo ip link set enet up
    sudo ip link add link enet name enet.58 type vlan id 58
    sudo ip link set enet.58 up
    sudo ip -6 addr add fe80::4:9fff:fe00:110/64 dev enet.58 scope link
    sudo ip -6 addr add fe80::4:9fff:fe00:120/64 dev enet.58 scope link

Then send with `wire_replay.py --dest fe80::4:9fff:fe00:150%enet.58`; the host
kernel builds the Ethernet frames and resolves the guest MAC by neighbour
discovery. This path exercises the identical guest-side RX code as the socket
path.

## Estimator timing fidelity and GNSS fusion

The estimator fuses a wire GNSS fix only while the fix timestamp sits inside its
aiding window relative to the IMU clock (about +0.1 s ahead to -0.5 s behind).
Two BIL settings keep the same-image run inside that window.

- `fastdyn/mr_vmu_tropic_flight.toml` sets `icount` `shift = 4`. At `shift = 6`
  the modelled CPU runs ~10x slower than the 600 MHz Cortex-M7, so inertial
  samples queue ~0.27 s between the driver and the estimator (measured) and
  every wire fix, stamped at guest receive time, lands past the +0.1 s freshness
  gate. `shift = 4` brings that latency to ~0.035 s (hardware is ~0.010-0.015 s).
  Read it live with `guest_clock.py` minus the estimator odometry timestamp.
- Pace the wire replayer at the guest virtual-time rate. Sample `guest_clock.py`
  twice a few seconds apart, take the delta as guest-seconds per wall-second, and
  pass it as `wire_replay.py --rate`. Fed faster, fixes arrive ahead of the IMU
  clock (future-dropped); fed slower, they age out (stale-dropped).

`ram_tap.py --status` additionally logs the eFMU aiding state (imu/gps clocks,
gps acceptance, consecutive rejections, NIS, recovery stage, reseed) so a
divergence can be attributed to future/stale drops, innovation rejections, or an
uncaptured origin.

## RC link and the pre-flight origin

The estimator seeds its GNSS origin only when vehicle health is fresh and not in
failsafe (`navigation_gps.c` `disarmed_health_eligible`). The flight image
declares failsafe whenever RC is stale, and RC arrives as CRSF frames on LPUART8
(async, over eDMA0 channel 16). A boot-only BIL with no RC therefore stays in
failsafe and never captures the origin, so GNSS is never fused and the estimate
dead-reckons. To reproduce the vehicle's pre-flight bench condition, model the
CRSF receiver so a valid disarmed frame (sticks centred, throttle idle, arm
switch off) arrives faster than the 100 ms RC-stale timeout; failsafe then
clears, `health.flags` reads 0, and the origin is captured.

### CRSF injector

`crsf_inject.py` provides that RC link. It emits CRSF RC_CHANNELS_PACKED frames
(sync 0xC8, len 0x18, type 0x16, 22-byte 16x11-bit payload, CRC8 poly 0xD5) into
the FIFO the LPUART8 model reads (`/tmp/lpuart8_pty` by default). The eDMA0
channel-16 model delivers the bytes into the flight image's async RX buffer, so
`input_crsf` parses them exactly as on the vehicle. Start it before the flight
image so the FIFO exists at model init:

    python3 tools/bil_flight/crsf_inject.py --path /tmp/lpuart8_pty --rate 250 &
    # (then launch the flight image, then the wire GNSS replay)

Defaults are the disarmed bench frame: sticks centred, throttle idle, arm switch
off (CRSF channel 5 below centre). The channels carry a small per-frame jitter
(`--dither`, default 3 ticks) because `input_crsf` only forwards a channel event
when its value changes; a perfectly static stream forwards nothing after the
first frame and the RC topic never validates, exactly as a real link's jitter
keeps events flowing. Options flip the arm switch (`--arm`, `--arm-after`) and
move the sticks (`--move-after` with `--move-roll/pitch/yaw/throttle`) later in a
run. With the injector running the manual-control topic reports Valid and
Active with the arm switch off, the control failsafe latch clears
(`health.flags` reads 0), and the estimator seeds its GNSS origin on the walk
replay so the wire GNSS is fused.
