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
- Rumoca eFMI control code generated into the build tree
- CRSF -> handwritten control -> quad-X mixer -> DSHOT
- `ACRO` and `AUTO_LEVEL` manual flight modes
- GNSS M10 path documented and devicetree-wired through Zephyr GNSS for later use

Build from this directory:

```sh
west build -b mr_vmu_tropic
```

RDD2 uses the same pinned CSyn module as CUBS2. CSyn owns the `synapse_fbs`
release, generated C headers, topic catalog, canonical Zenoh keys, payload
sizes, and transport bridge; RDD2 does not carry a second schema-fetch or
decoder path.

Deterministic lockstep never uses CSyn, ZROS bridging, Zenoh, or Ethernet for
pacing. `native_sim` and FastDyn select direct shared-memory backends in the
same `subsys/lockstep` module and exchange only generated `synapse_fbs`
payloads. FastDyn resolves the shared block from the ELF instead of relying on
a fixed firmware address. The normal Ethernet stack remains available, and a
lockstep communications build may enable CSyn/ZROS plus Zenoh as an
asynchronous side-channel without changing the direct lockstep coordinator.
Performance builds may omit the unused network stack; communications builds
retain ENET and enable CSyn/Zenoh independently of lockstep pacing.

CMake installs the pinned Rumoca release into the build tree, verifies the
installer and binary hashes, and generates eFMI Production Code from
`Vehicles.Rdd2.Controller` in the `modelica_models` West project under
`${CMAKE_BINARY_DIR}/generated/rumoca`. The reusable quadrotor plant, RDD2
parameters, controller, and model-level qualification mission all remain in
that common project. Generated C and `.efmu` containers are build outputs, not
committed source.

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
for lockstep builds and uses `native_sim/native/64` by default to avoid multilib
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

The Nix commands use an isolated RDD2 West workspace under
`.devenv/state/west/` by default. Set `RDD2_WEST_WORKSPACE=/path/to/workspace`
to choose its location explicitly; the selected workspace is governed only by
this repository's `west.yml`.

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
- Rumoca-generated control artifacts are build outputs and should not be
  committed or hand-edited; update the Modelica source and regenerate instead.
