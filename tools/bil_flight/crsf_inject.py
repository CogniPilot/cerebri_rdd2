#!/usr/bin/env python3
"""Feed CRSF RC_CHANNELS_PACKED frames to the LPUART8 model of the same-binary BIL.

The eDMA0 channel-16 model in the FastDyn scroll reads bytes from a FIFO and
delivers them into the flight image's async RX buffer exactly as the DMA engine
would. This injector emits the RC frames the real TBS receiver would send.

By default it streams a disarmed bench frame: sticks centred, throttle idle,
arm switch off. That makes rc.c report valid RC, clears the control failsafe
latch (``rdd2_control_fault_latch`` clears when the arm switch is valid and
off), and lets the estimator seed its GNSS origin. Options flip the arm switch
and move the sticks after a delay so an armed / moving-stick condition can be
exercised later in a run.

CRSF RC_CHANNELS_PACKED frame (26 bytes):
  0xC8 (sync) | 0x18 (len=24) | 0x16 (type) | 22 bytes (16 x 11-bit, LSB first) |
  CRC8 (poly 0xD5, over the type + 22 payload bytes).

Channel ticks: 172 = min, 992 = centre (1500 us), 1811 = max. rc.c reads the arm
switch on CRSF channel 5 (index 4): >= 1500 us (tick >= 992) is armed.
"""

import argparse
import errno
import os
import sys
import time

SYNC = 0xC8
TYPE_RC_CHANNELS = 0x16
FRAME_LEN = 24  # type + 22 payload + crc

TICK_MIN = 172
TICK_CENTER = 992
TICK_MAX = 1811


def crc8_dvbs2(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if (crc & 0x80) else ((crc << 1) & 0xFF)
    return crc


def pack_channels(channels):
    """Pack 16 x 11-bit channel values LSB-first into 22 bytes."""
    out = bytearray()
    bitbuf = 0
    bits = 0
    for value in channels[:16]:
        bitbuf |= (int(value) & 0x7FF) << bits
        bits += 11
        while bits >= 8:
            out.append(bitbuf & 0xFF)
            bitbuf >>= 8
            bits -= 8
    if bits:
        out.append(bitbuf & 0xFF)
    return bytes(out[:22].ljust(22, b"\x00"))


def build_frame(channels):
    payload = pack_channels(channels)
    body = bytes([TYPE_RC_CHANNELS]) + payload
    crc = crc8_dvbs2(body)
    return bytes([SYNC, FRAME_LEN]) + body + bytes([crc])


def open_fifo(path):
    if not os.path.exists(path):
        try:
            os.mkfifo(path, 0o666)
        except FileExistsError:
            pass
    # Read/write, non-blocking: keeps the FIFO open regardless of the model's
    # open order and never blocks when the reader is slow (excess frames drop).
    return os.open(path, os.O_RDWR | os.O_NONBLOCK)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--path", default="/tmp/lpuart8_pty",
                    help="FIFO the LPUART8 model reads (default /tmp/lpuart8_pty)")
    ap.add_argument("--rate", type=float, default=250.0,
                    help="frame rate in Hz (default 250)")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="seconds to stream, 0 = until killed (default 0)")
    ap.add_argument("--throttle", type=int, default=TICK_MIN,
                    help="throttle tick (CRSF ch3, default %d)" % TICK_MIN)
    ap.add_argument("--roll", type=int, default=TICK_CENTER)
    ap.add_argument("--pitch", type=int, default=TICK_CENTER)
    ap.add_argument("--yaw", type=int, default=TICK_CENTER)
    ap.add_argument("--flight-mode", type=int, default=TICK_CENTER,
                    help="flight-mode tick (CRSF ch6, default centre)")
    ap.add_argument("--arm", action="store_true",
                    help="hold the arm switch on for the whole run")
    ap.add_argument("--arm-after", type=float, default=None,
                    help="flip the arm switch on this many seconds in")
    ap.add_argument("--move-after", type=float, default=None,
                    help="apply --move-roll/pitch/yaw/throttle after N seconds")
    ap.add_argument("--move-roll", type=int, default=None)
    ap.add_argument("--move-pitch", type=int, default=None)
    ap.add_argument("--move-yaw", type=int, default=None)
    ap.add_argument("--move-throttle", type=int, default=None)
    ap.add_argument("--dither", type=int, default=3,
                    help="peak per-frame tick jitter added to the sticks so the "
                         "receiver keeps emitting channel events (a real link "
                         "always jitters; 0 disables). Default 3.")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    fd = open_fifo(args.path)
    period = 1.0 / args.rate if args.rate > 0 else 0.004

    def log(msg):
        if not args.quiet:
            sys.stderr.write(msg + "\n")
            sys.stderr.flush()

    log("crsf_inject: %s @ %.0f Hz, disarmed=%s"
        % (args.path, args.rate, not args.arm))

    start = time.monotonic()
    sent = 0
    dropped = 0
    next_t = start
    try:
        while True:
            now = time.monotonic()
            elapsed = now - start
            if args.duration > 0 and elapsed >= args.duration:
                break

            armed = args.arm
            if args.arm_after is not None and elapsed >= args.arm_after:
                armed = True

            channels = [TICK_CENTER] * 16
            channels[0] = args.roll
            channels[1] = args.pitch
            channels[2] = args.throttle
            channels[3] = args.yaw
            channels[4] = TICK_MAX if armed else TICK_MIN
            channels[5] = args.flight_mode

            if args.move_after is not None and elapsed >= args.move_after:
                if args.move_roll is not None:
                    channels[0] = args.move_roll
                if args.move_pitch is not None:
                    channels[1] = args.move_pitch
                if args.move_throttle is not None:
                    channels[2] = args.move_throttle
                if args.move_yaw is not None:
                    channels[3] = args.move_yaw

            if args.dither > 0:
                # A real receiver's channels jitter frame to frame. The CRSF
                # input driver only forwards a channel event when its value
                # changes (report filter), so a perfectly static stream would
                # forward nothing after the first frame and the RC topic would
                # never validate. Add a small triangle jitter to keep events
                # flowing while staying inside the disarmed band.
                tri = sent % (2 * args.dither)
                d = tri - args.dither
                for idx in (0, 1, 3):
                    channels[idx] = max(TICK_MIN, min(TICK_MAX, channels[idx] + d))
                # Throttle and arm jitter by 1 tick (kept well below centre so
                # the arm switch and throttle stay off/idle) so their values
                # also propagate to the manual-control topic.
                channels[2] = max(TICK_MIN, channels[2] + (sent & 1))
                channels[4] = max(TICK_MIN, channels[4] + (sent & 1))

            frame = build_frame(channels)
            try:
                os.write(fd, frame)
                sent += 1
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    dropped += 1
                else:
                    raise

            if not args.quiet and sent % (int(args.rate) * 5 or 1) == 0:
                log("crsf_inject: sent=%d dropped=%d t=%.1fs armed=%s"
                    % (sent, dropped, elapsed, armed))

            next_t += period
            sleep = next_t - time.monotonic()
            if sleep > 0:
                time.sleep(sleep)
            else:
                next_t = time.monotonic()
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)
        log("crsf_inject: done sent=%d dropped=%d" % (sent, dropped))


if __name__ == "__main__":
    main()
