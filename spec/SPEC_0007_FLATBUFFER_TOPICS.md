# SPEC_0007: Synapse Topic Format

## Status
ACCEPTED

## Summary
Topic payloads use generated fixed-layout `synapse_fbs` structs. ZROS owns
the local application bus. Network transports consume the same generated
catalog without owning or redefining schemas.

## Specification

**REQUIRED:**
- `synapse_fbs` owns generated headers, topic IDs, keys, payload sizes,
  validators, and bounded C codecs.
- RDD2 consumes an explicit generated `synapse_fbs` C package.
- Shared topic storage uses generated fixed-layout payload structs.
- ZROS is the application bus. A transport adapter publishes or subscribes
  through the existing application-owned ZROS topic.
- Strict direct-wire topics use the generated `synapse/wire.h` encoder and
  validator. Deployment code supplies addresses, ports, VLAN, source identity,
  timing limits, and enablement.
- A topic may additionally expose a generated ROS 2 CDRv1 projection. A CDR
  mirror is observational and does not replace the authoritative local topic.
- When a topic schema defines a fixed struct, firmware code must use the generated flatcc struct type instead of a handwritten mirror.
- The control loop may keep private local state, but every published value must
  be mapped to a generated standard Synapse payload.
- FlatBuffer topic publication must remain heap-free and bounded.
- Topic fields must be read through generated structs, accessors, or generated
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
- A transport package that owns or fetches a second schema copy.
- A second transport-owned bridge, registry, or topic store.

## Motivation

- The shared Synapse catalog gives all Cerebri firmware one stable contract.
- Fixed-size encodings keep the latency cost bounded and easy to reason about.
- Native local state in `ctx` keeps the control loop simple and fast.

## References

- `../west.yml`
- `SPEC_0002_LATENCY_DRIVEN_ARCHITECTURE.md`
- `SPEC_0006_CODE_SIZE_AND_DEBUG_SHELL.md`
