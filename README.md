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
and uses its generated FlatBuffer C headers and reflection schemas from the
build tree. Active schemas are staged under
`${CMAKE_BINARY_DIR}/generated/flatbuffers`; generated files are not kept in
the source tree.

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

## Nix / NixOS

This repo includes a flake for repeatable Zephyr host tooling on NixOS and
other Linux systems with Nix:

```sh
nix develop
rdd2-west-update
rdd2-build
```

Common commands are also exposed as flake apps:

```sh
nix run .#west-update
nix run .#build
nix run .#build-native-sim
nix run .#flash
```

The shell defaults to the `gnuarmemb` Zephyr toolchain for
`mr_vmu_tropic`. `rdd2-build-native-sim` overrides this to the host toolchain
for SITL builds and uses `native_sim/native/64` by default to avoid multilib
requirements on NixOS. Set `RDD2_NATIVE_SIM_BOARD=native_sim` if you need
Zephyr's 32-bit native simulator variant. The Nix shell includes x86 multilib
host support on `x86_64-linux`, so raw `west build -b native_sim` also works.
Use separate build directories when switching boards:

```sh
west build -b mr_vmu_tropic -d build
west build -b native_sim/native/64 -d build-native_sim
west build -b native_sim -d build-native_sim32
```

The Nix helpers already keep the common build outputs separate:
`rdd2-build` defaults to `build-mr_vmu_tropic`, while
`rdd2-build-native-sim` defaults to `build-native_sim`.

The app assumes the west workspace layout documented above:
`<workspace>/cerebri_rdd2`, `<workspace>/zephyr`, and `<workspace>/modules`.

If this checkout is nested inside a different west workspace, `rdd2-west-update`
creates a private module workspace under `${XDG_CACHE_HOME:-$HOME/.cache}/cerebri-rdd2`
and the build helpers use that workspace through `RDD2_WORKSPACE_ROOT`. Set
`RDD2_WEST_WORKSPACE=/path/to/workspace` to choose that location explicitly.

On NixOS, import the module and enable host tools plus debug-probe access:

```nix
{
  inputs.cerebri-rdd2.url = "path:/path/to/cerebri_rdd2";

  outputs = { self, nixpkgs, cerebri-rdd2, ... }: {
    nixosConfigurations.devbox = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        cerebri-rdd2.nixosModules.default
        {
          programs.cerebri-rdd2 = {
            enable = true;
            users = [ "alice" ];
          };
        }
      ];
    };
  };
}
```

Important assumptions:
- RC channel map is AETR on CRSF channels 1-4, arm is channel 5, and flight
  mode is channel 6.
- Mixer order is the local default in `src/main.c` and must be verified against the airframe wiring before flight.
- Imported generated estimator and PID source is transitional and should not be
  hand-edited; the intended long-term replacement is local Rumoca-generated
  code.
