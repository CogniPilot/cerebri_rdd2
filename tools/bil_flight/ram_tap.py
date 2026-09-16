#!/usr/bin/env python3
"""Host-side reader of the flight image navigation estimate from the
file-backed OCRAM/DTCM banks of a FastDyn/QEMU same-binary BIL run.

This is a memory tap, not a transport: it reads the ZROS topic message
storage (g_msg_navigation_odometry, g_msg_attitude_estimate) straight out of
the shared memory-backed RAM images QEMU mmaps for the guest, resolving the
symbol addresses from the ELF and mapping them onto the bank files. Each new
publish (timestamp change) is emitted; the output matches the replay
interchange estimate.csv schema in the scratchpad replay_input/FORMAT.md.

Because the memory files are mapped MAP_SHARED, guest writes land in the page
cache and re-reading the file observes them live. Rows are keyed on the guest
odometry timestamp so the sample cadence follows guest time, not wall time.
"""
import argparse
import math
import mmap
import os
import struct
import subprocess
import sys
import time

# Memory banks of fastdyn/mr_vmu_tropic_flight.toml. Each entry:
#   (base_address, size, filename_suffix)
# The FastDyn memory dir holds files named machine0_<id>_<id>.bin.
BANKS = [
    (0x20200000, 512 * 1024, "machine0_main_ocram2.bin"),
    (0x20000000, 256 * 1024, "machine0_dtcm_dtcm.bin"),
    (0x00000000, 256 * 1024, "machine0_itcm_itcm.bin"),
    (0x70000000, 4 * 1024 * 1024, "machine0_xip_flash_xip_flash.bin"),
]


def resolve_symbols(nm_bin, elf, names):
    """Return {name: (addr, size)} using nm --print-size."""
    out = subprocess.check_output([nm_bin, "-S", "--defined-only", elf],
                                  text=True, errors="replace")
    want = set(names)
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr, size, _typ, name = parts
            if name in want:
                found[name] = (int(addr, 16), int(size, 16))
        elif len(parts) == 3:
            addr, _typ, name = parts
            if name in want:
                found[name] = (int(addr, 16), 0)
    missing = want - set(found)
    if missing:
        raise SystemExit("symbols not found in ELF: %s" % ", ".join(sorted(missing)))
    return found


class BankReader:
    def __init__(self, memory_dir):
        self.maps = []
        for base, size, suffix in BANKS:
            path = os.path.join(memory_dir, suffix)
            if not os.path.exists(path):
                continue
            fd = os.open(path, os.O_RDONLY)
            fsz = os.fstat(fd).st_size
            length = min(size, fsz) if fsz else size
            try:
                mm = mmap.mmap(fd, length, prot=mmap.PROT_READ)
            except ValueError:
                os.close(fd)
                continue
            self.maps.append((base, length, mm, fd))
        if not self.maps:
            raise SystemExit("no memory bank files found under %s" % memory_dir)

    def read(self, addr, nbytes):
        for base, length, mm, _fd in self.maps:
            if base <= addr < base + length:
                off = addr - base
                if off + nbytes > length:
                    raise ValueError("read crosses bank end")
                return mm[off:off + nbytes]
        raise ValueError("addr 0x%08x not in any bank" % addr)


def quat_to_euler_enu(w, x, y, z):
    """ZYX (yaw, pitch, roll) of the FLU->ENU rotation. yaw from east to north."""
    n = math.sqrt(w * w + x * x + y * y + z * z)
    if n == 0.0:
        return 0.0, 0.0, 0.0
    w, x, y, z = w / n, x / n, y / n, z / n
    sinr_cosp = 2.0 * (w * x + y * z)
    cosr_cosp = 1.0 - 2.0 * (x * x + y * y)
    roll = math.atan2(sinr_cosp, cosr_cosp)
    sinp = 2.0 * (w * y - z * x)
    sinp = max(-1.0, min(1.0, sinp))
    pitch = math.asin(sinp)
    siny_cosp = 2.0 * (w * z + x * y)
    cosy_cosp = 1.0 - 2.0 * (y * y + z * z)
    yaw = math.atan2(siny_cosp, cosy_cosp)
    return roll, pitch, yaw


# OdometryEstimateData layout (size 232), see synapse state_reader.h
ODO = dict(
    timestamp_ns=0,     # u64
    position_enu=8,     # 3f (x=e,y=n,z=u)
    attitude=20,        # 4f (w,x,y,z)
    velocity_enu=36,    # 3f
    angular_velocity=48,  # 3f roll,pitch,yaw
    reset_counter=228,  # u8
    estimator_type=229,  # u8
    quality_pct=230,    # i8
    time_status=231,    # u8
)
# AttitudeEstimateData layout (size 40)
ATT = dict(timestamp_ns=0, attitude=8, angular_velocity=24, flags=36, time_status=37)


def parse_odo(buf):
    ts = struct.unpack_from("<Q", buf, ODO["timestamp_ns"])[0]
    e, n, u = struct.unpack_from("<3f", buf, ODO["position_enu"])
    qw, qx, qy, qz = struct.unpack_from("<4f", buf, ODO["attitude"])
    ve, vn, vu = struct.unpack_from("<3f", buf, ODO["velocity_enu"])
    reset_counter = buf[ODO["reset_counter"]]
    quality = struct.unpack_from("<b", buf, ODO["quality_pct"])[0]
    time_status = buf[ODO["time_status"]]
    return dict(ts=ts, e=e, n=n, u=u, qw=qw, qx=qx, qy=qy, qz=qz,
                ve=ve, vn=vn, vu=vu, reset_counter=reset_counter,
                quality=quality, time_status=time_status)


HEADER = ("t_s,e_m,n_m,u_m,ve_m_s,vn_m_s,vu_m_s,qw,qx,qy,qz,"
          "roll_rad,pitch_rad,yaw_rad,bgx_rad_s,bgy_rad_s,bgz_rad_s,"
          "bax_m_s2,bay_m_s2,baz_m_s2,pos_valid,att_valid")

# NavigationEstimatorState (efmu) field byte offsets within g_process (efmu is
# at offset 0 of struct navigation_estimator_process). Resolved from the ELF
# DWARF; stable for a given estimator export.
EFMU = dict(
    imu_timestamp_s=4,           # float
    gps_timestamp_s=404,         # float
    status_initialized=1928,     # bool
    status_gps_pos_accepted=1931,  # bool
    status_gps_vel_accepted=1932,  # bool
    status_flow_accepted=1936,   # bool
    gps_consec_rejections=1952,  # int32
    status_correction_outcome=1960,  # int32
    status_nis=1972,             # float
    status_recovery_stage=1976,  # int32
    status_reseeded=1988,        # bool
)
STATUS_HEADER = ("imu_ts_s,gps_ts_s,gps_minus_imu_ms,gps_pos_acc,gps_vel_acc,"
                 "flow_acc,gps_consec_rej,corr_outcome,nis,recovery_stage,"
                 "reseeded,status_init")


def _largest_symbol(nm_bin, elf, name):
    """Return (addr,size) of the largest-sized definition of name (there are
    several static g_process symbols; the navigation estimator's is by far the
    biggest, ~222 kB)."""
    out = subprocess.check_output([nm_bin, "-S", "--defined-only", elf],
                                  text=True, errors="replace")
    best = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] == name:
            addr, size = int(parts[0], 16), int(parts[1], 16)
            if best is None or size > best[1]:
                best = (addr, size)
    if best is None:
        raise SystemExit("symbol %s not found" % name)
    return best


def read_status(banks, base):
    def f(off):
        return struct.unpack_from("<f", banks.read(base + off, 4), 0)[0]

    def i(off):
        return struct.unpack_from("<i", banks.read(base + off, 4), 0)[0]

    def b(off):
        return banks.read(base + off, 1)[0]

    imu_ts = f(EFMU["imu_timestamp_s"])
    gps_ts = f(EFMU["gps_timestamp_s"])
    return dict(
        imu_ts=imu_ts, gps_ts=gps_ts,
        gps_minus_imu_ms=(gps_ts - imu_ts) * 1e3,
        gps_pos_acc=b(EFMU["status_gps_pos_accepted"]),
        gps_vel_acc=b(EFMU["status_gps_vel_accepted"]),
        flow_acc=b(EFMU["status_flow_accepted"]),
        gps_consec_rej=i(EFMU["gps_consec_rejections"]),
        corr_outcome=i(EFMU["status_correction_outcome"]),
        nis=f(EFMU["status_nis"]),
        recovery_stage=i(EFMU["status_recovery_stage"]),
        reseeded=b(EFMU["status_reseeded"]),
        status_init=b(EFMU["status_initialized"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--memory-dir", required=True,
                    help="FASTDYN_QEMU_MEMORY_DIR of the run")
    ap.add_argument("--out", required=True, help="estimate.csv output")
    ap.add_argument("--nm", default=os.environ.get("BIL_NM", "arm-zephyr-eabi-nm"))
    ap.add_argument("--duration", type=float, default=0.0,
                    help="wall seconds to sample (0 = until --stop-file gone/Ctrl-C)")
    ap.add_argument("--poll-hz", type=float, default=2000.0,
                    help="wall polling rate; rows are deduped on guest timestamp")
    ap.add_argument("--stop-file", default=None,
                    help="stop when this file disappears")
    ap.add_argument("--counters", action="store_true",
                    help="also print imu sample-count / last-sample-ns diagnostics")
    ap.add_argument("--status", action="store_true",
                    help="also log the estimator eFMU status fields (imu/gps "
                         "clocks, gps acceptance, rejections, NIS, recovery, "
                         "reseed) alongside each odometry row")
    args = ap.parse_args()

    syms = ["g_msg_navigation_odometry", "g_msg_attitude_estimate"]
    if args.counters:
        syms += ["g_imu_sample_count", "g_last_sample_ns"]
    addrs = resolve_symbols(args.nm, args.elf, syms)
    banks = BankReader(args.memory_dir)

    odo_addr = addrs["g_msg_navigation_odometry"][0]
    att_addr = addrs["g_msg_attitude_estimate"][0]
    proc_addr = None
    if args.status:
        proc_addr = _largest_symbol(args.nm, args.elf, "g_process")[0]

    out = open(args.out, "w")
    out.write(HEADER + ("," + STATUS_HEADER if args.status else "") + "\n")

    period = 1.0 / args.poll_hz
    t_end = time.monotonic() + args.duration if args.duration > 0 else None
    last_ts = None
    rows = 0
    nan = float("nan")
    diag_last = 0.0
    try:
        while True:
            now = time.monotonic()
            if t_end is not None and now >= t_end:
                break
            if args.stop_file is not None and not os.path.exists(args.stop_file):
                break
            odo = parse_odo(banks.read(odo_addr, 232))
            if odo["ts"] != 0 and odo["ts"] != last_ts:
                last_ts = odo["ts"]
                roll, pitch, yaw = quat_to_euler_enu(odo["qw"], odo["qx"],
                                                     odo["qy"], odo["qz"])
                att_ts = struct.unpack_from("<Q", banks.read(att_addr, 8), 0)[0]
                pos_valid = 1 if (odo["quality"] > 0 or odo["e"] != 0.0 or
                                  odo["n"] != 0.0) else 0
                att_valid = 1 if att_ts != 0 else 0
                line = (
                    "%.9f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.7f,%.7f,%.7f,%.7f,"
                    "%.7f,%.7f,%.7f,%g,%g,%g,%g,%g,%g,%d,%d" % (
                        odo["ts"] * 1e-9, odo["e"], odo["n"], odo["u"],
                        odo["ve"], odo["vn"], odo["vu"],
                        odo["qw"], odo["qx"], odo["qy"], odo["qz"],
                        roll, pitch, yaw,
                        nan, nan, nan, nan, nan, nan,
                        pos_valid, att_valid))
                if args.status:
                    st = read_status(banks, proc_addr)
                    line += (",%.6f,%.6f,%.2f,%d,%d,%d,%d,%d,%.4f,%d,%d,%d" % (
                        st["imu_ts"], st["gps_ts"], st["gps_minus_imu_ms"],
                        st["gps_pos_acc"], st["gps_vel_acc"], st["flow_acc"],
                        st["gps_consec_rej"], st["corr_outcome"], st["nis"],
                        st["recovery_stage"], st["reseeded"], st["status_init"]))
                out.write(line + "\n")
                rows += 1
                if rows % 500 == 0:
                    out.flush()
            if args.counters and now - diag_last > 1.0:
                diag_last = now
                sc = struct.unpack_from("<i", banks.read(
                    addrs["g_imu_sample_count"][0], 4), 0)[0]
                ns = struct.unpack_from("<Q", banks.read(
                    addrs["g_last_sample_ns"][0], 8), 0)[0]
                sys.stderr.write(
                    "[ram_tap] imu_sample_count=%d last_sample_ns=%d (%.3fs guest)"
                    " odo_ts=%.3fs rows=%d\n" % (
                        sc, ns, ns * 1e-9, last_ts * 1e-9 if last_ts else 0.0, rows))
                sys.stderr.flush()
            time.sleep(period)
    except KeyboardInterrupt:
        pass
    finally:
        out.flush()
        out.close()
    sys.stderr.write("[ram_tap] wrote %d rows to %s\n" % (rows, args.out))


if __name__ == "__main__":
    main()
