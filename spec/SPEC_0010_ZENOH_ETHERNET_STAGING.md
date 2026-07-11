# SPEC_0010: Zenoh Ethernet Staging

## Status
DRAFT

## Summary
Zenoh over Ethernet is provided by the same CSyn module and generated Synapse
topic catalog used by CUBS2.

## Specification

**REQUIRED:**
- Zenoh-over-Ethernet uses the standard Zephyr IPv4/UDP networking stack on `mr_vmu_tropic`.
- The Zenoh integration uses CSyn over `zenoh-pico`, not a custom wire parser.
- Session bring-up and reconnect run in a dedicated low-priority thread.
- Received data is retained in CSyn's bounded latest-sample topic store.
- Topic identity, key expressions, payload size, and generated decoding come
  from CSyn's pinned `synapse_fbs` catalog.
- Shell inspection uses the CSyn shell and stored state.

**CURRENT DIRECTION:**
- Default bench configuration uses static IPv4 settings to simplify bring-up.
- Offboard inputs must first be added to the standard Synapse catalog and CSyn;
  RDD2 must not add local raw-payload mirrors.
- The first target is reliable Ethernet and subscription bring-up without
  blocking the control loop.
- Board-level Zephyr bring-up shells such as `net ping`, `net stats`, and `mdio` are allowed for Ethernet diagnostics, but subsystem-specific transport shells should continue to read stored state rather than driving the live path directly.

**PROHIBITED:**
- Blocking the 1600 Hz control loop on Ethernet or Zenoh traffic.
- Handwritten message decoders or RDD2-local topic keys.
- Unbounded payload buffering or per-sample heap ownership in shell/debug code.

## Motivation

- Offboard inputs are useful, but bring-up should not destabilize manual flight.
- Zenoh transport debugging is easier when network and subscription state can be inspected independently from controller behavior.
- A bounded latest-sample store is enough for initial integration and shell diagnostics.

## References

- `../west.yml`
- `SPEC_0002_LATENCY_DRIVEN_ARCHITECTURE.md`
- `SPEC_0004_TROPIC_HARDWARE_SCOPE.md`
