#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
zenoh_scan.py
=============
Listen on a zenoh session and report what is publishing, so "is anything on
this network, and is it what I think it is?" can be answered before pointing
``publish_gps_zenoh.py`` at it.

This is the zenoh-side counterpart of ``csyn_serial scan`` on the vehicle, and
it separates the same failure modes: whether the session reached the router at
all, and whether anything is publishing once it has.

    ./zenoh_scan.py --connect tcp/192.168.1.10:7447
    ./zenoh_scan.py --connect tcp/192.168.1.10:7447 --duration 15
    ./zenoh_scan.py                              # peer mode, local network
    ./zenoh_scan.py --key 'cub1/**'              # narrow the sweep

Each key is matched against the synapse catalog, so a payload whose length
disagrees with the schema is called out rather than counted as a healthy
topic. That is the ground-side twin of the vehicle's bad_len counter.

Requirements
------------
    pip install eclipse-zenoh synapse-fbs
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from publish_gps_zenoh import HAVE_SYNAPSE_FBS  # noqa: E402

if not HAVE_SYNAPSE_FBS:
    sys.exit("ERROR: synapse-fbs is required: pip install synapse-fbs\n"
             "       (the schema bindings from github.com/CogniPilot/synapse_fbs)")

from publish_gps_zenoh import (_payload_bytes, resolve_mode,  # noqa: E402
                               topic_catalog, zenoh_config)


class Tally:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = defaultdict(int)
        self.sizes = defaultdict(set)
        self.first = {}
        self.last = {}

    def add(self, sample):
        key = str(sample.key_expr)
        size = len(_payload_bytes(sample))
        now = time.monotonic()
        with self.lock:
            self.counts[key] += 1
            self.sizes[key].add(size)
            self.first.setdefault(key, now)
            self.last[key] = now


def describe(key: str, sizes: set) -> tuple:
    """Return (topic label, note) for one observed key."""
    parsed = topic_catalog.parse_key(key)
    if parsed is None:
        return "-", "not a synapse catalog key"

    info = parsed.topic
    label = info.key if parsed.instance is None else f"{info.key}/{parsed.instance}"

    if info.encoding == "table":
        return label, f"{info.root_table}, table-encoded"
    if len(sizes) > 1:
        return label, f"INCONSISTENT sizes {sorted(sizes)}, want {info.payload_size}"
    observed = next(iter(sizes))
    if observed != info.payload_size:
        return label, (f"SIZE MISMATCH: {observed} B on the wire, schema says "
                       f"{info.payload_size} -- schema skew")
    return label, info.payload_type


def report_connectivity(session, args) -> bool:
    info = session.info
    routers = list(info.routers_zid())
    peers = list(info.peers_zid())
    print(f"session {info.zid()}  mode={args.mode}")
    print(f"routers: {', '.join(str(r) for r in routers) if routers else 'none'}")
    print(f"peers  : {', '.join(str(p) for p in peers) if peers else 'none'}")

    if routers or peers:
        return True

    print()
    if args.connect:
        # Peer mode does not fail the open on an unreachable endpoint the way
        # client mode does; it just keeps scouting. So this is reachable.
        print(f"Session is up but nothing is linked, including {args.connect}.")
        print("  - is zenohd running there, and is the port open?")
    else:
        print("No router and no peer found by multicast scouting.")
        print("  - pass --connect tcp/<router-ip>:7447 if you know where it is")
        print("  - multicast often does not cross subnets or VPNs")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[3])
    parser.add_argument("--connect", default=None,
                        help="router endpoint, e.g. tcp/192.168.1.10:7447")
    parser.add_argument("--mode", default=None, choices=("peer", "client"),
                        help="default: client when --connect is given, else peer")
    parser.add_argument("--key", default="**", help="key expression to sweep (default: **)")
    parser.add_argument("--duration", type=float, default=10.0, help="seconds (default: %(default)s)")
    parser.add_argument("--liveliness", action="store_true",
                        help="also query liveliness tokens, which show declared but idle publishers")
    args = parser.parse_args()

    try:
        import zenoh
    except ImportError:
        print("ERROR: eclipse-zenoh is required: pip install eclipse-zenoh", file=sys.stderr)
        return 1

    args.mode = resolve_mode(args.mode, args.connect)
    try:
        session = zenoh.open(zenoh_config(zenoh, args.connect, args.mode))
    except Exception as exc:
        # In client mode an unreachable endpoint fails the open outright, which
        # is the clearest signal there is -- but the message underneath it is a
        # Rust source location, so say what it means.
        print(f"ERROR: cannot open a zenoh session in {args.mode} mode:\n  {exc}",
              file=sys.stderr)
        if args.connect:
            print(f"\nNothing answered at {args.connect}:", file=sys.stderr)
            print("  - is zenohd running on that host, and is the port open?", file=sys.stderr)
            print("  - an endpoint is <proto>/<host>:<port>, e.g. tcp/192.168.1.10:7447;"
                  " 7447 is the default", file=sys.stderr)
            print("  - if that address is another peer rather than a router,"
                  " try --mode peer", file=sys.stderr)
        return 1

    tally = Tally()
    try:
        # Give the transport a moment to settle before reporting on it, or a
        # healthy router reads as unreachable purely because of the race.
        time.sleep(1.0)
        connected = report_connectivity(session, args)
        print()

        sub = session.declare_subscriber(args.key, tally.add)
        print(f"listening on {args.key} for {args.duration:.0f}s...")
        try:
            time.sleep(args.duration)
        except KeyboardInterrupt:
            print()
        del sub

        if args.liveliness:
            print("\nliveliness tokens:")
            tokens = [str(r.ok.key_expr) for r in session.liveliness().get("**")
                      if r.ok is not None]
            for token in sorted(tokens) or ["  none"]:
                print(f"  {token}")
    finally:
        session.close()

    with tally.lock:
        keys = sorted(tally.counts, key=lambda k: -tally.counts[k])
        counts, sizes, first, last = tally.counts, tally.sizes, tally.first, tally.last

    print()
    if not keys:
        if connected:
            print("connected, but nothing published on that key expression.")
            print("  - is the mocap bridge running?")
            print("  - --key ** sweeps everything; a narrower --key may simply not match")
        return 0

    print(f"{'key':<40}{'topic':<20}{'n':>7}{'rate':>9}{'bytes':>7}  note")
    for key in keys:
        span = max(last[key] - first[key], 1e-6)
        rate = (counts[key] - 1) / span if counts[key] > 1 else 0.0
        label, note = describe(key, sizes[key])
        observed = "/".join(str(s) for s in sorted(sizes[key]))
        print(f"{key:<40}{label:<20}{counts[key]:>7}{rate:>8.1f}/s{observed:>7}  {note}")

    print()
    usable = [k for k in keys if topic_catalog.parse_key(k) is not None]
    if usable:
        print("point the bridge at one of these, for example:")
        example = topic_catalog.parse_key(usable[0])
        namespace = example.namespace or "**"
        print(f"  ./publish_gps_zenoh.py --namespace {namespace} "
              f"--topic {example.topic.key}"
              + (f" --instance {example.instance}" if example.instance is not None else "")
              + (f" --connect {args.connect}" if args.connect else "")
              + " --dry-run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
