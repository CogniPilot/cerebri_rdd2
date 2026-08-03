#!/usr/bin/env python3
"""
publish_gps.py
==============
ROS 2 node that subscribes to a nav_msgs/Odometry topic, converts the
local-frame position & velocity into WGS-84 GPS coordinates, and streams
MAVLink GPS_INPUT (#232) messages to ArduPilot over a serial or UDP link.

The node treats the odometry frame as a local ENU (East-North-Up) frame
anchored at a user-specified geodetic origin.

Usage
-----
  # Typical usage with a UDP connection:
  python3 publish_gps.py --ros-args \
      -p port:=udp:127.0.0.1:14551 \
      -p odom_topic:=/ardupilot/odom \
      -p origin_lat:=40.41535 \
      -p origin_lon:=-86.93291 \
      -p origin_alt:=0.0

  # Typical usage with a SiK radio:
  python3 publish_gps.py --ros-args \
      -p port:=/dev/ttyUSB0 \
      -p baud:=57600

  # Dry-run (prints to terminal, no serial port):
  python3 publish_gps.py --ros-args -p dry_run:=true

  # With FLU frame convention (ROS REP-105 body frame):
  python3 publish_gps.py --ros-args -p frame:=flu

  # Facility X-axis is 35 deg clockwise from true north:
  python3 publish_gps.py --ros-args -p yaw_offset:=35.0

Requirements
------------
  pip install pymavlink pyserial
  # Plus a sourced ROS 2 workspace (Humble / Iron / Jazzy / Rolling)
"""

import math
import sys
import time

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, HistoryPolicy
from nav_msgs.msg import Odometry

try:
    from pymavlink import mavutil
except ImportError:
    sys.exit("ERROR: pymavlink not found.\nRun: pip install pymavlink pyserial")


# ──────────────────────────────────────────────────────────────────────────────
# WGS-84 constants
# ──────────────────────────────────────────────────────────────────────────────
WGS84_A  = 6_378_137.0
WGS84_F  = 1.0 / 298.257_223_563
WGS84_B  = WGS84_A * (1 - WGS84_F)
WGS84_E2 = 1 - (WGS84_B / WGS84_A) ** 2


# ──────────────────────────────────────────────────────────────────────────────
# Coordinate helpers
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
    """Rotate a 2-D vector (x, y) counter-clockwise by angle_deg.

    Used to align a facility frame with true-north ENU before GPS
    conversion.  The angle is the clockwise bearing from the facility
    X-axis to true North, so we rotate by +angle to go from facility
    frame -> true-north ENU.
    """
    a = math.radians(angle_deg)
    cos_a = math.cos(a)
    sin_a = math.sin(a)
    return x * cos_a - y * sin_a, x * sin_a + y * cos_a


# ──────────────────────────────────────────────────────────────────────────────
# MAVLink GPS_INPUT sender
# ──────────────────────────────────────────────────────────────────────────────
def send_gps_input(mav, lat_deg7, lon_deg7, alt_m,
                   vn, ve, vd, yaw_deg,
                   hdop=0.3, vdop=0.4, satellites=14, fix_type=3):
    """Send a single MAVLink GPS_INPUT message."""
    speed = math.hypot(vn, ve)
    yaw_cdeg = int(yaw_deg * 100) if yaw_deg is not None else 36000
    time_usec = int(time.time() * 1e6)

    mav.mav.gps_input_send(
        time_usec,          # timestamp [us]
        0,                  # GPS ID
        0,                  # ignored_flags (0 = use all fields)
        0,                  # time_week_ms
        0,                  # time_week
        fix_type,           # 3 = 3-D fix
        lat_deg7,           # latitude  [degE7]
        lon_deg7,           # longitude [degE7]
        float(alt_m),       # altitude  [m MSL]
        float(hdop),
        float(vdop),
        float(vn),          # velocity north [m/s]
        float(ve),          # velocity east  [m/s]
        float(vd),          # velocity down  [m/s]
        float(max(speed * 0.05, 0.1)),  # speed accuracy [m/s]
        0.3,                # horizontal accuracy [m]
        0.4,                # vertical accuracy   [m]
        satellites,
        yaw_cdeg,           # yaw [cdeg]
    )


# ──────────────────────────────────────────────────────────────────────────────
# ROS 2 Node
# ──────────────────────────────────────────────────────────────────────────────
class OdomToGpsMavlink(Node):
    """Subscribe to Odometry, convert to GPS, publish via MAVLink."""

    def __init__(self):
        super().__init__("odom_to_gps_mavlink")

        # ── Declare parameters ───────────────────────────────────────────────
        self.declare_parameter("port",       "udp:127.0.0.1:14551")
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

        # ── MAVLink connection ───────────────────────────────────────────────
        self.mav = None
        if self.dry_run:
            self.get_logger().info("=== DRY-RUN mode (no serial port) ===")
        else:
            self._connect_mavlink()

        # ── Latest odom cache ────────────────────────────────────────────────
        self._latest_odom = None
        self._prev_pos = None   # (east, north, up) after frame conversion
        self._prev_time = None  # time.monotonic()

        # ── Subscribe to odometry ────────────────────────────────────────────
        qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            history=HistoryPolicy.KEEP_LAST,
            depth=1,
        )
        self.create_subscription(Odometry, self.odom_topic, self._odom_cb, qos)
        self.get_logger().info(f"Subscribed to {self.odom_topic}")

        # ── Timer to send GPS_INPUT at fixed rate ────────────────────────────
        period = 1.0 / self.rate_hz
        self.create_timer(period, self._timer_cb)

        self._msg_count = 0
        self._last_log_time = time.monotonic()

        self.get_logger().info(
            f"Origin: {self.origin_lat:.6f} N  {self.origin_lon:.6f} E  "
            f"alt={self.origin_alt:.1f} m | Rate: {self.rate_hz} Hz | "
            f"Frame: {self.frame.upper()} | Yaw offset: {self.yaw_offset:.1f} deg"
        )

    # ── MAVLink setup ────────────────────────────────────────────────────────
    def _connect_mavlink(self):
        self.get_logger().info(f"Connecting to {self.port} @ {self.baud} baud")
        try:
            self.mav = mavutil.mavlink_connection(
                self.port,
                baud=self.baud,
                source_system=255,
                source_component=mavutil.mavlink.MAV_COMP_ID_GPS,
                dialect="ardupilotmega",
                mavlink_version=2,
            )
            self.mav.mav.srcSystem    = 255
            self.mav.mav.srcComponent = mavutil.mavlink.MAV_COMP_ID_GPS
            self.mav.mav.robust_parsing = True

            self.get_logger().info("Waiting for heartbeat from FC ...")
            hb = self.mav.wait_heartbeat(timeout=10)
            if hb is None:
                self.get_logger().error(
                    "No heartbeat received. Check wiring / MAVProxy."
                )
                self.mav = None
                return
            self.get_logger().info(
                f"Heartbeat OK - system {self.mav.target_system} "
                f"component {self.mav.target_component}"
            )
        except Exception as e:
            self.get_logger().error(f"Cannot open port: {e}")
            self.mav = None

    # ── Odometry callback ────────────────────────────────────────────────────
    def _odom_cb(self, msg: Odometry):
        self._latest_odom = msg

    # ── Timer callback ───────────────────────────────────────────────────────
    def _timer_cb(self):
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
        if self._prev_pos is not None and self._prev_time is not None:
            dt = now - self._prev_time
            if dt > 0.001:  # avoid division by zero
                ve = (east  - self._prev_pos[0]) / dt
                vn = (north - self._prev_pos[1]) / dt
                vd = -(up   - self._prev_pos[2]) / dt   # down = -up
            else:
                ve, vn, vd = 0.0, 0.0, 0.0
        else:
            ve, vn, vd = 0.0, 0.0, 0.0

        self._prev_pos = (east, north, up)
        self._prev_time = now

        # ── Yaw from quaternion (uses heading_offset, not yaw_offset) ────────
        q = odom.pose.pose.orientation
        yaw_deg = (quaternion_to_yaw(q.x, q.y, q.z, q.w) + self.heading_offset) % 360.0

        # ── ENU -> Geodetic ──────────────────────────────────────────────────
        lat, lon, alt = enu_to_geodetic(
            east, north, up,
            self.origin_lat, self.origin_lon, self.origin_alt,
        )

        lat_deg7 = int(lat * 1e7)
        lon_deg7 = int(lon * 1e7)

        # ── Send via MAVLink or dry-run log ──────────────────────────────────
        if self.mav is not None:
            try:
                send_gps_input(
                    self.mav, lat_deg7, lon_deg7, alt,
                    vn, ve, vd, None,
                )
            except Exception as e:
                self.get_logger().warn(f"MAVLink send failed: {e}")
        elif self.dry_run:
            pass  # just log below

        # ── Console logging (throttled to ~1 Hz) ────────────────────────────
        self._msg_count += 1
        now = time.monotonic()
        if now - self._last_log_time >= 1.0:
            self.get_logger().info(
                f"E={east:+7.3f} N={north:+7.3f} U={up:+6.3f} | "
                f"vn={vn:+6.3f} ve={ve:+6.3f} vd={vd:+6.3f} | "
                f"lat={lat:.7f} lon={lon:.7f} alt={alt:.2f} | "
                f"yaw={yaw_deg:.1f} deg | msgs={self._msg_count}"
            )
            self._last_log_time = now


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────
def main(args=None):
    rclpy.init(args=args)
    node = OdomToGpsMavlink()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        node.get_logger().info("Shutting down.")
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()