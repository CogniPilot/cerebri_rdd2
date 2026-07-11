# SPEC_0007: FlatBuffer Topic Format

## Status
ACCEPTED

## Summary
Topic payloads are the generated fixed-layout `synapse_fbs` structs owned by
CSyn, while the control loop keeps private native working state.

## Specification

**REQUIRED:**
- CSyn owns the pinned `synapse_fbs` release, generated headers, topic catalog,
  canonical keys, and payload sizing.
- RDD2 and CUBS2 use the same CSyn and `synapse_fbs` revisions.
- Shared topic storage uses generated fixed-layout payload structs.
- When a topic schema defines a fixed struct, firmware code must use the generated flatcc struct type instead of a handwritten mirror.
- The control loop may keep private local state, but every published value must
  be mapped to a generated standard Synapse payload.
- FlatBuffer topic publication must remain heap-free and bounded.
- Topic fields must be read through generated structs/accessors or shared CSyn
  codecs, never handwritten offset logic.
- The published flight-state topics should carry the current flight mode, estimated attitude, desired attitude, desired rates, and commanded rates needed for bench and lockstep debugging.
- The published flight-state topic should also carry the measured hot-path latency from the IMU interrupt timestamp to the DSHOT trigger timestamp in microseconds.

**PROHIBITED:**
- Heap allocation or dynamic builders in the rate-loop hot path.
- Writing generated FlatBuffer code into the source tree.
- Hand-packed FlatBuffer table encoders or decoders for schema-defined topic payloads.
- Handwritten field offset maps for schema-defined FlatBuffer tables.
- Custom topic keys, payload-size tables, or schema mirrors in RDD2.
- Sharing hot-path native structs directly with diagnostics readers.

## Motivation

- The shared Synapse catalog gives all Cerebri firmware one stable contract.
- Fixed-size encodings keep the latency cost bounded and easy to reason about.
- Native local state in `ctx` keeps the control loop simple and fast.

## References

- `../west.yml`
- `SPEC_0002_LATENCY_DRIVEN_ARCHITECTURE.md`
- `SPEC_0006_CODE_SIZE_AND_DEBUG_SHELL.md`
