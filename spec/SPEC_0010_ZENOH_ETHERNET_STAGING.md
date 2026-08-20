# SPEC_0010: Ethernet Direct Wire and CDR Mirrors

## Status
DRAFT

## Summary
Selected bounded sensor and control topics use direct Synapse UDP/IPv6
datagrams. Any topic with a generated bounded ROS projection may also be
published as a ROS 2 CDRv1 mirror through `rmw_zenoh_cpp`.

## Specification

**REQUIRED:**
- Embedded direct wire uses the standard Zephyr UDP/IPv6 stack and VLAN
  support on `mr_vmu_tropic`.
- Header encoding, decoding, and validation use the generated
  `synapse_fbs` C library.
- Socket receive and reconnect run in a dedicated preemptible thread outside
  the 800 Hz controller.
- The receiver validates source and destination addresses, ports, interface,
  hop limit, schema, topic, source identity, session, sequence, timestamp, and
  payload before publishing onto ZROS.
- ROS 2 CDR mirrors use generated IDL, exact ROS and DDS names, RIHS01 identity,
  fixed serialized size, and allocation-free generated codecs where available.
- `rmw_zenoh_cpp` owns its native 33-byte publication attachment. Application
  code does not synthesize a competing attachment format.
- Mirror selection is deployment policy. Adding a CDR mirror does not require
  adding a topic to the strict direct-wire set.
- Shell inspection reports stored counters and state without driving the live
  receive path.

**CURRENT DIRECTION:**
- The bench uses static IPv6 link-local addresses on VLAN 58.
- Optical flow and GNSS are the first direct-wire receive topics.
- Optical flow, GNSS, and bootstrap health are the first ROS 2 CDR mirrors.
- Additional bounded topics can gain CDR projections without changing the
  direct-wire carrier.
- Board-level Zephyr network shells are allowed for diagnostics.

**PROHIBITED:**
- Blocking the 800 Hz control loop on Ethernet or Zenoh traffic.
- Handwritten schema decoders or RDD2-local topic keys.
- Unbounded payload buffering or per-sample heap ownership in shell/debug code.
- Treating a CDR mirror as authoritative control, estimation, readiness, or
  arming input.
- A second embedded transport bridge, registry, or latest-sample store.

## Motivation

- Direct wire gives selected embedded paths a small bounded carrier.
- CDR mirrors provide normal ROS 2 interoperability without forcing every
  topic into the strict embedded carrier.
- Keeping transport policy outside the schema package lets one generated
  payload support serial, direct wire, logging, and ROS 2 mirrors.

## References

- `../west.yml`
- `SPEC_0002_LATENCY_DRIVEN_ARCHITECTURE.md`
- `SPEC_0004_TROPIC_HARDWARE_SCOPE.md`
