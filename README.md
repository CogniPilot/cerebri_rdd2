# RDD2

`rdd2` is the active multirotor platform folder in this repository.

Start with [spec/README.md](spec/README.md)
for platform rules.

V1 goals:
- `mr_vmu_tropic` only
- CRSF input only
- DSHOT output only
- ICM45686 IMU only
- one application hot-path thread
- no dependency on the legacy `cerebri` module
- no `double` in the control path

Current implementation scope:
- CEP-0002 platform layout under `rdd2/`
- local FlexIO DSHOT driver vendored into this repo
- generated estimator and controller source isolated under `src/generated/`
- CRSF -> generated control -> quad-X mixer -> DSHOT
- `ACRO` and `AUTO_LEVEL` manual flight modes
- GNSS M10 path documented and devicetree-wired through Zephyr GNSS for later use

Build from this directory:

```sh
west build -b mr_vmu_tropic
```

CMake downloads the `synapse_fbs-c.tar.gz` release asset, verifies its SHA256,
and uses its generated FlatBuffer C headers from the build tree. Active schemas
are staged under `${CMAKE_BINARY_DIR}/generated/flatbuffers`; generated files
are not kept in the source tree.

To bootstrap a fresh minimal workspace from this repo's manifest, check out
this repo at `<workspace>/cerebri_rdd2` and initialize west from the workspace
root:

```sh
mkdir -p /tmp/cerebri-ws
git clone <repo-url> /tmp/cerebri-ws/cerebri_rdd2
cd /tmp/cerebri-ws
west init -l cerebri_rdd2
west update
west build -b mr_vmu_tropic cerebri_rdd2
```

Important assumptions:
- RC channel map is AETR on CRSF channels 1-4, arm is channel 5, and flight
  mode is channel 6.
- Mixer order is the local default in `src/main.c` and must be verified against the airframe wiring before flight.
- Imported generated estimator and PID source is transitional and should not be
  hand-edited; the intended long-term replacement is local Rumoca-generated
  code.
