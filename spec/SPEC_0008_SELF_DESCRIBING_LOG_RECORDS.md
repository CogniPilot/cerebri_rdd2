# SPEC_0008: Self-Describing Log Records

## Status
ACCEPTED

## Summary
SD-card logging uses a dedicated FlatBuffer log-envelope schema plus generated binary schemas so recorded data can be decoded later without private source-tree knowledge.

## Specification

**REQUIRED:**
- Log records use `fbs/synapse_log.fbs` from the pinned `synapse_fbs` C release asset, not ad-hoc raw byte dumps.
- Logged topic payloads are wrapped in a typed FlatBuffer union record.
- The log format supports a `SchemaRecord` for carrying schema bytes when that path is enabled.
- Build outputs stage active `.fbs` files under `${CMAKE_BINARY_DIR}/generated/flatbuffers`.
- The logger remains outside the flight hot path.

**PROHIBITED:**
- Writing anonymous topic bytes to storage with no typed envelope.
- Doing reflection parsing or schema serialization in the rate-loop hot path.

## Motivation

- A log file should still be decodable after the source tree changes.
- A typed log envelope avoids guessing payload type from filename or offset.
- Schema records let offline tools recover structure directly from the log stream.

## References

- `synapse_fbs-c.tar.gz:fbs/synapse_log.fbs`
- `synapse_fbs-c.tar.gz:fbs/synapse_topics.fbs`
- `SPEC_0007_FLATBUFFER_TOPICS.md`
