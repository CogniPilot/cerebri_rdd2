#!/usr/bin/env python3
"""Read the flight image guest clock from the file-backed QEMU memory banks.

Prints the newest inertial sample time (g_last_sample_ns) in seconds. Sampled
twice a few wall-seconds apart, the delta gives the guest virtual-time rate
(guest-seconds per wall-second) under the current icount setting. Pace the wire
replayer at that rate (wire_replay.py --rate) so GNSS and optical-flow frames
arrive at their true cadence relative to the estimator's IMU clock: fed faster,
fixes land ahead of the IMU clock and are dropped as future; fed slower, they
age out of the aiding window. This is a memory tap, not a transport.
"""
import argparse
import mmap
import os
import struct
import subprocess

# OCRAM2/DTCM/ITCM/XIP banks of fastdyn/mr_vmu_tropic_flight.toml.
BANKS = [
    (0x20200000, 512 * 1024, "machine0_main_ocram2.bin"),
    (0x20000000, 256 * 1024, "machine0_dtcm_dtcm.bin"),
    (0x00000000, 256 * 1024, "machine0_itcm_itcm.bin"),
    (0x70000000, 4 * 1024 * 1024, "machine0_xip_flash_xip_flash.bin"),
]


def resolve(nm_bin, elf, name):
    out = subprocess.check_output([nm_bin, "-S", "--defined-only", elf],
                                  text=True, errors="replace")
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[-1] == name:
            return int(p[0], 16)
    raise SystemExit("symbol %s not found" % name)


def read(memory_dir, addr, nbytes):
    for base, size, suffix in BANKS:
        if base <= addr < base + size:
            path = os.path.join(memory_dir, suffix)
            fd = os.open(path, os.O_RDONLY)
            try:
                length = min(size, os.fstat(fd).st_size)
                mm = mmap.mmap(fd, length, prot=mmap.PROT_READ)
                off = addr - base
                return mm[off:off + nbytes]
            finally:
                os.close(fd)
    raise SystemExit("addr 0x%08x not in any bank" % addr)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--memory-dir", required=True)
    ap.add_argument("--nm", default=os.environ.get("BIL_NM", "arm-zephyr-eabi-nm"))
    ap.add_argument("--symbol", default="g_last_sample_ns")
    args = ap.parse_args()
    addr = resolve(args.nm, args.elf, args.symbol)
    ns = struct.unpack("<Q", read(args.memory_dir, addr, 8))[0]
    print("%.9f" % (ns * 1e-9))


if __name__ == "__main__":
    main()
