# SPEC_0001: Repository Architecture

## Status
ACCEPTED

## Summary
`rdd2` follows a CEP-0002 platform layout, and the active `rdd2` Zephyr app
lives under `rdd2/` with a platform-local manifest, specs, and build wiring.

## Specification

**REQUIRED:**
- The `rdd2` Zephyr application lives under `rdd2/`, not at the repository
  root.
- The `rdd2` platform manifest lives in `rdd2/west.yml`.
- The repository root does not carry a compatibility `west.yml`.
- Platform-local generated output ignore rules live in `rdd2/.gitignore`, while
  repo-root tooling and workspace ignore rules stay in the root `.gitignore`.
- `src/main.c` is only the composition root. Every generated eFMU owns one
  adapter under `src/processes/`; the rate adapter owns the IMU-paced calling
  thread and every other adapter owns one Zephyr worker thread.
- Local app build wiring lives in `src/CMakeLists.txt`, not in one growing root source list.
- Modelica sources for generated control artifacts live in the shared
  `modelica_models` West project. Generated Rumoca/eFMI outputs live in the
  build tree. Thin process adapters translate generated eFMU state to `synapse_fbs`
  messages and devices without reimplementing control laws.
- Reproducible host-side development and CI commands live in the root Cargo
  workspace under `xtask/`. The FastDyn FMI/lockstep mission host is the
  `fastdyn-mission` xtask command, orchestrated by `fastdyn-ci`.
- Local subsystem build/config wiring lives under `subsys/` with local `CMakeLists.txt` and `Kconfig` files.
- Local driver build/config wiring lives under `drivers/` with family-local `CMakeLists.txt` and `Kconfig` files where needed.
- Debug and shell helpers live outside the hot-path module when they grow beyond trivial size.
- The Tropic FlexIO DSHOT driver family remains vendored locally under `drivers/nxp_flexio_dshot/`.
- `spec/` is the source of truth for project rules.
- The repo targets one board in v1: `mr_vmu_tropic`.

**PROHIBITED:**
- Dependency on the legacy `cerebri` module.
- Committing or editing generated control source directly when a handwritten
  wrapper or regenerated build artifact should be used instead.
- ZROS hops between functions that execute in the same RTOS thread.
- Ad hoc private C payloads on established cross-thread or external
  interfaces. A model connector absent from the pinned Synapse catalog may
  have one fixed-capacity ingress type, isolated in its owning process until a
  catalogued transport adapter exists.
- Multi-board abstraction layers for v1.
- A second firmware-local implementation of a control law owned by
  `modelica_models`.
- Unreferenced top-level `tests/`, `test_scripts/`, `scripts/`, or `tools/`
  collections. A build configuration belongs to its Zephyr/FastDyn directory;
  an executable host workflow belongs in `xtask/`.

## Motivation

- The codebase should be easy to audit.
- AI agents need obvious ownership boundaries.
- Tropic bring-up should not be blocked by architecture speculation.
- Platform-local manifests and source trees avoid root-level ambiguity as more
  platforms are added.

## References

- `../west.yml`
- `../src/CMakeLists.txt`
- `../src/processes/README.md`
- `../xtask/Cargo.toml`
- `../subsys/CMakeLists.txt`
- `../subsys/Kconfig`
- `../drivers/CMakeLists.txt`
- `../drivers/Kconfig`
- `SPEC_0002_LATENCY_DRIVEN_ARCHITECTURE.md`
- `SPEC_0004_TROPIC_HARDWARE_SCOPE.md`
