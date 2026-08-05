#!/usr/bin/env python3
"""
publish_gps_synapse.py
======================
ROS 2 node that subscribes to a nav_msgs/Odometry topic, converts the
local-frame position & velocity into WGS-84 GPS coordinates, and streams
synapse ``GnssFix`` payloads to the vehicle over the zros serial transport.

This is the synapse counterpart of ``publish_gps.py``: identical odometry
handling, frame conventions and geodetic math, but instead of MAVLink
GPS_INPUT (#232) it emits the bare 64-byte ``GnssFixData`` struct inside the
compact synapse serial framing that ``subsys/zros_serial/`` speaks. The
vehicle publishes it on the ``gnss`` topic (catalog id 8).

The node treats the odometry frame as a local ENU (East-North-Up) frame
anchored at a user-specified geodetic origin.

Usage
-----
  # Typical usage with a SiK radio:
  python3 publish_gps_synapse.py --ros-args \
      -p port:=/dev/ttyUSB0 \
      -p baud:=57600 \
      -p odom_topic:=/drone/odom \
      -p origin_lat:=40.41535 \
      -p origin_lon:=-86.93291 \
      -p origin_alt:=0.0

  # Dry-run (prints decoded fixes, no serial port):
  python3 publish_gps_synapse.py --ros-args -p dry_run:=true

  # With FLU frame convention (ROS REP-105 body frame):
  python3 publish_gps_synapse.py --ros-args -p frame:=flu

  # Facility X-axis is 35 deg clockwise from true north:
  python3 publish_gps_synapse.py --ros-args -p yaw_offset:=35.0

Check it landed, on the vehicle shell:

  csyn topic echo gnss
  zros_serial status

Requirements
------------
  pip install pyserial
  # Plus a sourced ROS 2 workspace (Humble / Iron / Jazzy / Rolling)
"""

import math
import sys
import time
from pathlib import Path

# The wire format lives with the ground tool; never duplicate it here, or the
# two ends can silently drift apart.
_TOOLS = Path(__file__).resolve().parent.parent / "tools" / "synapse_serial"
sys.path.insert(0, str(_TOOLS))
try:
    from synapse_serial import (FLAG_COURSE_VALID, FLAG_TIME_VALID,
                                FLAG_VELOCITY_UP_VALID, FLAG_YAW_VALID,
                                FrameDecoder, GnssFix, TOPIC_GNSS_FIX,
                                TOPIC_NAMES, encode_frame, saturate_u16)
except ImportError as exc:
    sys.exit(f"ERROR: cannot import the synapse wire format from {_TOOLS}: {exc}")

# Importable without ROS so the conversion below can be exercised on its own.
try:
    import rclpy
    from rclpy.executors import ExternalShutdownException
    from rclpy.node import Node
    from rclpy.qos import QoSProfile, ReliabilityPolicy, HistoryPolicy
    from nav_msgs.msg import Odometry
    HAVE_ROS = True
except ImportError:
    HAVE_ROS = False
    Node = object
    ExternalShutdownException = KeyboardInterrupt


# ──────────────────────────────────────────────────────────────────────────────
# WGS-84 constants
# ──────────────────────────────────────────────────────────────────────────────
WGS84_A  = 6_378_137.0
WGS84_F  = 1.0 / 298.257_223_563
WGS84_B  = WGS84_A * (1 - WGS84_F)
WGS84_E2 = 1 - (WGS84_B / WGS84_A) ** 2

# Below this ground speed the course-over-ground derived from differencing is
# noise, so the fix is published without CourseValid rather than with a
# meaningless heading.
COURSE_VALID_MIN_SPEED_MS = 0.15


# ──────────────────────────────────────────────────────────────────────────────
# Coordinate helpers  (identical to publish_gps.py, so calibration carries over)
# ──────────────────────────────────────────────────────────────────────────────
def enu_to_geodetic(east, north, up, lat0_deg, lon0_deg, alt0_m):
    """Flat-Earth ENU -> WGS-84 lat/lon/alt (accurate to <1 cm indoors)."""
    lat0 = math.radians(lat0_deg)
    N = WGS84_A / math.sqrt(1 - WGS84_E2 * math.sin(lat0) ** 2)
    d_lat = math.degrees(north / (N + alt0_m))
    d_lon = math.degrees(east / ((N + alt0_m) * math.cos(lat0)))
    return lat0_deg + d_lat, lon0_deg + d_lon, alt0_m + up


def quaternion_to_yaw(x, y, z, w):
    """Extract yaw (heading) in degrees from a quaternion."""
    siny_cosp = 2.0 * (w * z + x * y)
    cosy_cosp = 1.0 - 2.0 * (y * y + z * z)
    yaw_rad = math.atan2(siny_cosp, cosy_cosp)
    return math.degrees(yaw_rad) % 360.0


def rotate_2d(x, y, angle_deg):
    """Rotate a 2-D vector (x, y) counter-clockwise by angle_deg."""
    a = math.radians(angle_deg)
    cos_a = math.cos(a)
    sin_a = math.sin(a)
    return x * cos_a - y * sin_a, x * sin_a + y * cos_a


def clamp_i16(value):
    return max(-32768, min(32767, int(round(value))))


# ──────────────────────────────────────────────────────────────────────────────
# ENU state -> synapse GnssFixData
# ──────────────────────────────────────────────────────────────────────────────
def build_gnss_fix(east, north, up, vn, ve, vd,
                   origin_lat, origin_lon, origin_alt,
                   *, boot_us, yaw_deg=None, hdop=0.3, vdop=0.4,
                   satellites=14, fix_type=3, h_acc_m=0.3, v_acc_m=0.4,
                   instance=0, have_velocity=True):
    """Pure conversion, kept free of ROS so it can be tested on its own.

    GnssFixData has no NED velocity vector: it carries ground speed, course
    over ground and vertical velocity instead, so the horizontal components
    are folded into speed/course here. Course is receiver-native (zero at
    true north, positive clockwise), which is atan2(east, north).
    """
    lat, lon, alt = enu_to_geodetic(east, north, up,
                                    origin_lat, origin_lon, origin_alt)

    speed = math.hypot(vn, ve)
    course_deg = math.degrees(math.atan2(ve, vn)) % 360.0

    flags = FLAG_TIME_VALID
    if have_velocity:
        flags |= FLAG_VELOCITY_UP_VALID
        if speed >= COURSE_VALID_MIN_SPEED_MS:
            flags |= FLAG_COURSE_VALID
    if yaw_deg is not None:
        flags |= FLAG_YAW_VALID

    return GnssFix(
        timestamp_us=int(boot_us),
        time_unix_us=int(time.time() * 1e6),
        latitude_deg_e7=int(round(lat * 1e7)),
        longitude_deg_e7=int(round(lon * 1e7)),
        altitude_msl_mm=int(round(alt * 1000.0)),
        altitude_ellipsoid_mm=int(round(alt * 1000.0)),
        horizontal_accuracy_mm=saturate_u16(h_acc_m * 1000.0),
        vertical_accuracy_mm=saturate_u16(v_acc_m * 1000.0),
        velocity_accuracy_mm_s=saturate_u16(max(speed * 0.05, 0.1) * 1000.0),
        yaw_accuracy_cdeg=saturate_u16(500.0) if yaw_deg is not None else 0,
        hdop_centi=saturate_u16(hdop * 100.0),
        vdop_centi=saturate_u16(vdop * 100.0),
        ground_speed_cm_s=saturate_u16(speed * 100.0),
        # % 36000 so a course of 359.999 deg reports 35999 rather than 36000,
        # which is outside the range the vehicle's own producer can emit.
        course_over_ground_cdeg=int(course_deg * 100.0) % 36000,
        yaw_cdeg=saturate_u16((yaw_deg % 360.0) * 100.0) if yaw_deg is not None else 0,
        velocity_up_cm_s=clamp_i16(-vd * 100.0),
        flags=flags,
        fix_type=fix_type,
        satellites_used=min(int(satellites), 255),
        satellites_visible=min(int(satellites), 255),
        id=instance,
    )


# ──────────────────────────────────────────────────────────────────────────────
# ROS 2 Node
# ──────────────────────────────────────────────────────────────────────────────
class OdomToGnssSynapse(Node):
    """Subscribe to Odometry, convert to GPS, publish as a synapse GnssFix."""

    def __init__(self):
        super().__init__("odom_to_gnss_synapse")

        # ── Declare parameters ───────────────────────────────────────────────
        self.declare_parameter("port",       "/dev/ttyUSB0")
        self.declare_parameter("baud",       57600)
        self.declare_parameter("odom_topic", "/drone/odom")
        self.declare_parameter("origin_lat", 40.41545396897393)
        self.declare_parameter("origin_lon", -86.93275866259437)
        self.declare_parameter("origin_alt", 0.0)
        self.declare_parameter("rate_hz",    10.0)
        self.declare_parameter("dry_run",    False)
        self.declare_parameter("frame",      "enu")
        self.declare_parameter("yaw_offset", 230.0)
        self.declare_parameter("heading_offset", 138.0)
        self.declare_parameter("publish_yaw", False)
        self.declare_parameter("hdop",       0.3)
        self.declare_parameter("vdop",       0.4)
        self.declare_parameter("satellites", 14)
        self.declare_parameter("gnss_instance", 0)

        # ── Read parameters ──────────────────────────────────────────────────
        self.port       = self.get_parameter("port").value
        self.baud       = self.get_parameter("baud").value
        self.odom_topic = self.get_parameter("odom_topic").value
        self.origin_lat = self.get_parameter("origin_lat").value
        self.origin_lon = self.get_parameter("origin_lon").value
        self.origin_alt = self.get_parameter("origin_alt").value
        self.rate_hz    = self.get_parameter("rate_hz").value
        self.dry_run    = self.get_parameter("dry_run").value
        self.frame      = self.get_parameter("frame").value.lower()
        self.yaw_offset = self.get_parameter("yaw_offset").value
        self.heading_offset = self.get_parameter("heading_offset").value
        self.publish_yaw = self.get_parameter("publish_yaw").value
        self.hdop       = self.get_parameter("hdop").value
        self.vdop       = self.get_parameter("vdop").value
        self.satellites = self.get_parameter("satellites").value
        self.instance   = self.get_parameter("gnss_instance").value

        # ── Serial link ──────────────────────────────────────────────────────
        self.link = None
        if self.dry_run:
            self.get_logger().info("=== DRY-RUN mode (no serial port) ===")
        else:
            self._connect_serial()

        # There is no handshake in this protocol, so instead of waiting for a
        # heartbeat the node reports whatever the vehicle streams back: any
        # decoded frame proves the radio link works in both directions.
        self._decoder = FrameDecoder()
        self._from_vehicle = {}
        self._read_error_logged = False

        # ── Latest odom cache ────────────────────────────────────────────────
        self._latest_odom = None
        self._prev_pos = None   # (east, north, up) after frame conversion
        self._prev_time = None  # time.monotonic()
        self._seq = 0
        self._start = time.monotonic()

        # ── Subscribe to odometry ────────────────────────────────────────────
        qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            history=HistoryPolicy.KEEP_LAST,
            depth=1,
        )
        self.create_subscription(Odometry, self.odom_topic, self._odom_cb, qos)
        self.get_logger().info(f"Subscribed to {self.odom_topic}")

        # ── Timer to send GnssFix at fixed rate ──────────────────────────────
        self.create_timer(1.0 / self.rate_hz, self._timer_cb)

        self._msg_count = 0
        self._last_log_time = time.monotonic()

        self.get_logger().info(
            f"Origin: {self.origin_lat:.6f} N  {self.origin_lon:.6f} E  "
            f"alt={self.origin_alt:.1f} m | Rate: {self.rate_hz} Hz | "
            f"Frame: {self.frame.upper()} | Yaw offset: {self.yaw_offset:.1f} deg"
        )

    # ── Serial setup ─────────────────────────────────────────────────────────
    def _connect_serial(self):
        try:
            import serial
        except ImportError:
            self.get_logger().error("pyserial not found. Run: pip install pyserial")
            return

        self.get_logger().info(f"Opening {self.port} @ {self.baud} baud")
        try:
            self.link = serial.Serial(self.port, self.baud, timeout=0)
        except serial.SerialException as exc:
            self.get_logger().error(f"Cannot open port: {exc}")
            self.link = None

    # ── Odometry callback ────────────────────────────────────────────────────
    def _odom_cb(self, msg):
        self._latest_odom = msg

    # ── Timer callback ───────────────────────────────────────────────────────
    def _timer_cb(self):
        self._drain_vehicle()

        odom = self._latest_odom
        if odom is None:
            return

        # ── Extract position from odometry ───────────────────────────────────
        px = odom.pose.pose.position.x
        py = odom.pose.pose.position.y
        pz = odom.pose.pose.position.z

        # ── Convert to ENU depending on frame convention ─────────────────────
        if self.frame == "flu":
            east, north, up = -py, px, pz
        else:
            east, north, up = px, py, pz

        # ── Rotate by yaw_offset to align facility frame with true north ─────
        if self.yaw_offset != 0.0:
            east, north = rotate_2d(east, north, self.yaw_offset)

        # ── Compute velocity from position differences ───────────────────────
        now = time.monotonic()
        have_velocity = False
        if self._prev_pos is not None and self._prev_time is not None:
            dt = now - self._prev_time
            if dt > 0.001:  # avoid division by zero
                ve = (east  - self._prev_pos[0]) / dt
                vn = (north - self._prev_pos[1]) / dt
                vd = -(up   - self._prev_pos[2]) / dt   # down = -up
                have_velocity = True
            else:
                ve, vn, vd = 0.0, 0.0, 0.0
        else:
            ve, vn, vd = 0.0, 0.0, 0.0

        self._prev_pos = (east, north, up)
        self._prev_time = now

        # ── Yaw from quaternion (uses heading_offset, not yaw_offset) ────────
        q = odom.pose.pose.orientation
        yaw_deg = (quaternion_to_yaw(q.x, q.y, q.z, q.w) + self.heading_offset) % 360.0

        fix = build_gnss_fix(
            east, north, up, vn, ve, vd,
            self.origin_lat, self.origin_lon, self.origin_alt,
            boot_us=(now - self._start) * 1e6,
            yaw_deg=yaw_deg if self.publish_yaw else None,
            hdop=self.hdop, vdop=self.vdop, satellites=self.satellites,
            instance=self.instance, have_velocity=have_velocity,
        )

        # ── Send over the radio, or log in dry-run ───────────────────────────
        if self.link is not None:
            try:
                self.link.write(encode_frame(TOPIC_GNSS_FIX, fix.pack(), self._seq))
            except Exception as exc:
                self.get_logger().warn(f"serial write failed: {exc}")
        self._seq = (self._seq + 1) & 0xFF

        # ── Console logging (throttled to ~1 Hz) ────────────────────────────
        self._msg_count += 1
        if now - self._last_log_time >= 1.0:
            back = ", ".join(f"{k}={v}" for k, v in sorted(self._from_vehicle.items()))
            self.get_logger().info(
                f"E={east:+7.3f} N={north:+7.3f} U={up:+6.3f} | "
                f"vn={vn:+6.3f} ve={ve:+6.3f} vd={vd:+6.3f} | "
                f"lat={fix.latitude_deg_e7 / 1e7:.7f} "
                f"lon={fix.longitude_deg_e7 / 1e7:.7f} "
                f"alt={fix.altitude_msl_mm / 1000.0:.2f} | "
                f"yaw={yaw_deg:.1f} deg | msgs={self._msg_count} | "
                f"from vehicle: {back or 'nothing yet'}"
            )
            self._last_log_time = now

    # ── Inbound telemetry, purely as a link-health signal ────────────────────
    def _drain_vehicle(self):
        if self.link is None:
            return
        try:
            waiting = self.link.in_waiting
            if not waiting:
                return
            for topic_id, _seq, _payload in self._decoder.feed(self.link.read(waiting)):
                name = TOPIC_NAMES.get(topic_id, f"id{topic_id}")
                self._from_vehicle[name] = self._from_vehicle.get(name, 0) + 1
        except Exception as exc:
            # An unplugged radio raises on every tick; log once so it does not
            # masquerade as a vehicle that simply has nothing to say, but do
            # not spam the console at the timer rate.
            if not self._read_error_logged:
                self.get_logger().warn(f"serial read failed: {exc}")
                self._read_error_logged = True


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────
def main(args=None):
    if not HAVE_ROS:
        sys.exit("ERROR: rclpy / nav_msgs not found. Source your ROS 2 workspace.")

    rclpy.init(args=args)
    try:
        node = OdomToGnssSynapse()
    except Exception:
        rclpy.shutdown()
        raise

    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        node.get_logger().info("Shutting down.")
    except ExternalShutdownException:
        # SIGTERM, which is how ros2 launch and systemd stop a node. rclpy
        # raises this out of spin() and has already torn the context down;
        # letting it propagate would make every normal teardown look like a
        # crash.
        pass
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == "__main__":
    main()
