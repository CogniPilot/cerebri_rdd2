#!/usr/bin/env python3
"""Least-privilege host bridge from Synapse wire UDP payloads to raw Ethernet
frames for the FastDyn ENET model.

The flight image receives GNSS and optical-flow inputs only as Synapse wire
frames over UDP/IPv6 on the VLAN 58 ENET interface (see subsys/synapse_wire and
boards/mr_vmu_tropic.conf). Creating a TAP needs CAP_NET_ADMIN, which this user
lacks, so instead the FastDyn ENET model is pointed at an AF_UNIX datagram
socket (FASTDYN_ENET_TAP=unix:<guest>:<host>), where each datagram is one
Ethernet frame. This bridge:

  1. binds the host peer socket (receives any guest TX; discarded), and two
     UDP/IPv6 sockets that the wire replayer sends to (GNSS and flow ports);
  2. wraps each received wire payload in Ethernet + 802.1Q VLAN 58 + IPv6 + UDP
     with exactly the addresses, ports and hop limit the receiver's
     binding_valid()/carrier_observe() checks require;
  3. delivers the frame as one datagram to the guest ENET socket.

No privilege is required. Frame addressing (defaults match the board config):
  dst MAC      guest ENET MAC (read from ENET PALR/PAUR, default 02:04:9f:00:00:00)
  src MAC      per node, EUI-64 of the source link-local address
  VLAN         id 58, priority 0
  IPv6 src     fe80::4:9fff:fe00:110 (GNSS) / ...:120 (flow), hop limit 1
  IPv6 dst     fe80::4:9fff:fe00:150 (receiver local address)
  UDP ports    src == dst == 46008 (GNSS) / 46010 (flow)
"""
import argparse
import ipaddress
import os
import selectors
import socket
import struct
import sys

ETH_P_8021Q = 0x8100
ETH_P_IPV6 = 0x86DD
IPPROTO_UDP = 17


def mac_bytes(text):
    return bytes(int(b, 16) for b in text.split(":"))


def ipv6_bytes(text):
    return ipaddress.IPv6Address(text).packed


def udp6_checksum(src, dst, udp_hdr_and_payload):
    length = len(udp_hdr_and_payload)
    pseudo = src + dst + struct.pack("!IHBB", length, 0, 0, IPPROTO_UDP)
    data = pseudo + udp_hdr_and_payload
    if len(data) % 2:
        data += b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    s = (~s) & 0xFFFF
    return s if s != 0 else 0xFFFF


def build_frame(dst_mac, src_mac, vlan_id, src6, dst6, sport, dport, payload,
                hop_limit=1, priority=0):
    udp_len = 8 + len(payload)
    udp_hdr = struct.pack("!HHHH", sport, dport, udp_len, 0)
    csum = udp6_checksum(src6, dst6, udp_hdr + payload)
    udp = struct.pack("!HHHH", sport, dport, udp_len, csum) + payload
    # IPv6 header: ver/tc/flow, payload len, next header, hop limit, src, dst
    ipv6 = struct.pack("!IHBB", 0x60000000, len(udp), IPPROTO_UDP, hop_limit)
    ipv6 += src6 + dst6
    inner = struct.pack("!H", ETH_P_IPV6) + ipv6 + udp
    tci = ((priority & 0x7) << 13) | (vlan_id & 0x0FFF)
    vlan = struct.pack("!HH", ETH_P_8021Q, tci)
    return dst_mac + src_mac + vlan + inner


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--guest-sock", default="/tmp/rdd2_enet_g.sock",
                    help="AF_UNIX dgram the ENET model binds (frames sent here)")
    ap.add_argument("--host-sock", default="/tmp/rdd2_enet_h.sock",
                    help="AF_UNIX dgram this bridge binds (guest TX arrives here)")
    ap.add_argument("--udp-bind", default="::1",
                    help="host address the wire replayer sends its UDP to")
    ap.add_argument("--gnss-port", type=int, default=46008)
    ap.add_argument("--flow-port", type=int, default=46010)
    ap.add_argument("--dst-mac", default="02:04:9f:00:00:00",
                    help="guest ENET MAC (ENET PALR/PAUR)")
    ap.add_argument("--vlan", type=int, default=58)
    ap.add_argument("--recv-local", default="fe80::4:9fff:fe00:150")
    ap.add_argument("--gnss-src", default="fe80::4:9fff:fe00:110")
    ap.add_argument("--flow-src", default="fe80::4:9fff:fe00:120")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    dst_mac = mac_bytes(args.dst_mac)
    recv6 = ipv6_bytes(args.recv_local)

    def src_mac_from_ll(addr):
        # EUI-64 inverse: drop the ff:fe inserted in the middle of the interface
        # id to recover the locally-administered MAC the board assigned.
        iid = ipv6_bytes(addr)[8:]
        return bytes([iid[0], iid[1], iid[2], iid[5], iid[6], iid[7]])

    streams = {
        args.gnss_port: dict(name="gnss", src6=ipv6_bytes(args.gnss_src),
                             src_mac=src_mac_from_ll(args.gnss_src),
                             port=args.gnss_port),
        args.flow_port: dict(name="flow", src6=ipv6_bytes(args.flow_src),
                             src_mac=src_mac_from_ll(args.flow_src),
                             port=args.flow_port),
    }

    # guest ENET socket (send target)
    guest = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)

    # host peer socket (guest TX sink)
    try:
        os.unlink(args.host_sock)
    except OSError:
        pass
    host = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    host.bind(args.host_sock)
    host.setblocking(False)

    sel = selectors.DefaultSelector()
    sel.register(host, selectors.EVENT_READ, ("tx", None))

    for port, meta in streams.items():
        us = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        us.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        us.bind((args.udp_bind, port))
        us.setblocking(False)
        sel.register(us, selectors.EVENT_READ, ("udp", meta))
        sys.stderr.write("[wire_bridge] listening UDP [%s]:%d -> %s\n" %
                         (args.udp_bind, port, meta["name"]))

    counts = {"gnss": 0, "flow": 0, "tx": 0, "senterr": 0}
    sys.stderr.write("[wire_bridge] guest=%s host=%s dst_mac=%s vlan=%d\n" %
                     (args.guest_sock, args.host_sock, args.dst_mac, args.vlan))
    sys.stderr.flush()

    try:
        while True:
            for key, _mask in sel.select(timeout=1.0):
                kind, meta = key.data
                if kind == "tx":
                    try:
                        key.fileobj.recv(4096)
                        counts["tx"] += 1
                    except BlockingIOError:
                        pass
                    continue
                try:
                    payload, _addr = key.fileobj.recvfrom(4096)
                except BlockingIOError:
                    continue
                frame = build_frame(dst_mac, meta["src_mac"], args.vlan,
                                    meta["src6"], recv6, meta["port"],
                                    meta["port"], payload)
                try:
                    guest.sendto(frame, args.guest_sock)
                    counts[meta["name"]] += 1
                    if args.verbose and counts[meta["name"]] <= 5:
                        sys.stderr.write("[wire_bridge] %s frame %d bytes -> guest\n"
                                         % (meta["name"], len(frame)))
                        sys.stderr.flush()
                except OSError as exc:
                    counts["senterr"] += 1
                    if counts["senterr"] <= 3:
                        sys.stderr.write("[wire_bridge] send to guest failed: %s\n"
                                         % exc)
                        sys.stderr.flush()
            if sum(counts.values()) and (counts["gnss"] + counts["flow"]) % 200 == 0:
                pass
    except KeyboardInterrupt:
        pass
    finally:
        sys.stderr.write("[wire_bridge] gnss=%d flow=%d guest_tx=%d senderr=%d\n" %
                         (counts["gnss"], counts["flow"], counts["tx"], counts["senterr"]))


if __name__ == "__main__":
    main()
