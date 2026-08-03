#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
fake_mocap_zenoh.py
===================
Publish a synthetic mocap trajectory as synapse ``ExternalOdometryData`` on
Zenoh, so ``publish_gps_zenoh.py`` can be brought up before the real bridge
exists.

This is the Zenoh-side counterpart of ``send_fake_gps.py``: it answers "is my
key expression right and is zenoh routing between these two hosts?" without
involving a mocap system, a radio or a vehicle.

    ./fake_mocap_zenoh.py                            # cub1/external_pose/0
    ./fake_mocap_zenoh.py --namespace field_lab/cub1 --instance 2
    ./fake_mocap_zenoh.py --lost-every 10            # exercise the Lost path

And in another terminal:

    ./publish_gps_zenoh.py --namespace cub1 --dry-run

Requirements
------------
    pip install eclipse-zenoh synapse-fbs
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Two stages on purpose: the names below the guard only exist when the schema
# bindings imported, so pulling them in unconditionally would replace the
# advice with an ImportError traceback.
from publish_gps_zenoh import HAVE_SYNAPSE_FBS  # noqa: E402

if not HAVE_SYNAPSE_FBS:
    sys.exit("ERROR: synapse-fbs is required: pip install synapse-fbs\n"
             "       (the schema bindings from github.com/CogniPilot/synapse_fbs)")

from publish_gps_zenoh import (COV_IDX_EAST, COV_IDX_NORTH, COV_IDX_UP,  # noqa: E402
                               ExternalOdometryFlags, ExternalOdometryStatus,
                               simulate_payload, topic_catalog)

# ExternalOdometryCovarianceData: timestamp, the 78-element upper triangle, the
# two ids and six bytes of tail padding. Only used to produce bytes; the bridge
# decodes them through the schema bindings, and its --selftest checks the
# element indices this writes into.
COVARIANCE = struct.Struct("<Q78f2B6x")


def covariance_payload(timestamp_us: int, h_sigma: float, v_sigma: float,
                       source_id: int, instance: int) -> bytes:
    values = [0.0] * 78
    # Split the horizontal variance evenly between east and north, which is
    # what the bridge recombines with sqrt(var_e + var_n).
    values[COV_IDX_EAST] = h_sigma * h_sigma / 2.0
    values[COV_IDX_NORTH] = h_sigma * h_sigma / 2.0
    values[COV_IDX_UP] = v_sigma * v_sigma
    return COVARIANCE.pack(timestamp_us, *values, source_id, instance)


def lost_payload(t: float, radius: float, period: float, instance: int) -> bytes:
    """A sample the producer has given up on.

    The position stays populated and PositionValid stays set -- Lost is the
    producer saying "this is the last thing I knew, do not navigate on it".
    That is the case worth rehearsing, because the bridge has to notice and
    downgrade the fix itself. A producer that instead stops publishing is
    covered by the bridge's --timeout, and needs nothing on this side.
    """
    return simulate_payload(t, radius, period, instance,
                            flags=ExternalOdometryFlags.PositionValid
                            | ExternalOdometryFlags.Lost,
                            status=ExternalOdometryStatus.Lost)


def key_for(namespace: str, catalog_key: str, instance: int) -> str:
    info = topic_catalog.topic_by_key(catalog_key)
    parts = ([namespace] if namespace else []) + [info.key]
    if info.multi_instance:
        parts.append(str(instance))
    return "/".join(parts)


def main() -> int:
    try:
        import zenoh
    except ImportError:
        print("ERROR: eclipse-zenoh is required: pip install eclipse-zenoh", file=sys.stderr)
        return 1

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[3])
    parser.add_argument("--namespace", default="cub1")
    parser.add_argument("--instance", type=int, default=0)
    parser.add_argument("--rate", type=float, default=50.0, help="Hz (default: %(default)s)")
    parser.add_argument("--radius", type=float, default=4.0, help="metres")
    parser.add_argument("--period", type=float, default=20.0, help="seconds per lap")
    parser.add_argument("--hsigma", type=float, default=0.02,
                        help="horizontal one-sigma published on external_pose_cov")
    parser.add_argument("--vsigma", type=float, default=0.03)
    parser.add_argument("--no-covariance", dest="covariance", action="store_false")
    parser.add_argument("--lost-every", type=float, default=0.0,
                        help="seconds between one-second Lost bursts; 0 disables")
    parser.add_argument("--connect", default=None, help="e.g. tcp/192.168.1.10:7447")
    parser.add_argument("--count", type=int, default=0, help="stop after N samples")
    args = parser.parse_args()

    config = zenoh.Config()
    if args.connect:
        config.insert_json5("connect/endpoints", f'["{args.connect}"]')
    session = zenoh.open(config)

    pose_key = key_for(args.namespace, "external_pose", args.instance)
    pub = session.declare_publisher(pose_key)
    cov_pub = None
    print(f"publishing {pose_key} at {args.rate} Hz")
    if args.covariance:
        cov_key = key_for(args.namespace, "external_pose_cov", args.instance)
        cov_pub = session.declare_publisher(cov_key)
        print(f"publishing {cov_key}")

    period = 1.0 / args.rate
    start = time.monotonic()
    sent = 0
    try:
        while True:
            t = time.monotonic() - start
            timestamp_us = int(t * 1e6)
            lost = args.lost_every > 0.0 and (t % args.lost_every) < 1.0
            pub.put(lost_payload(t, args.radius, args.period, args.instance) if lost else
                    simulate_payload(t, args.radius, args.period, args.instance))
            if cov_pub is not None and not lost:
                cov_pub.put(covariance_payload(timestamp_us, args.hsigma, args.vsigma,
                                               0, args.instance))
            sent += 1
            if args.count and sent >= args.count:
                break
            time.sleep(period)
    except KeyboardInterrupt:
        print()
    finally:
        session.close()

    print(f"sent {sent}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
