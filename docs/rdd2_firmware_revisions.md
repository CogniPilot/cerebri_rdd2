# RDD2 Firmware Revision Manifest

This document records the repository revisions used by the latest pushed RDD2
firmware branch as of 2026-08-14.

## Firmware repository

- Repository: <https://github.com/CogniPilot/cerebri_rdd2>
- Branch: `rumoca-efmu-update`
- Commit: `bdc71b44c128c8186292bd9afbb1a4ab65d0fd86`
- Commit subject: `Integrate GPS mission control and Tropic hardware packaging`

## West-managed repositories

These revisions are declared by `west.yml` at the firmware commit above.

| Repository | Revision |
|---|---|
| [CogniPilot/zephyr](https://github.com/CogniPilot/zephyr) | `5ddc8c25b49aa10b5daf0ac7555baaf0f12ecc83` |
| [zephyrproject-rtos/cmsis](https://github.com/zephyrproject-rtos/cmsis) | `512cc7e895e8491696b61f7ba8066b4a182569b8` |
| [zephyrproject-rtos/cmsis-dsp](https://github.com/zephyrproject-rtos/cmsis-dsp) | `97512610ec92058f0119450b9e743eeb7e95b5c8` |
| [zephyrproject-rtos/cmsis_6](https://github.com/zephyrproject-rtos/cmsis_6) | `b2dfbe1a20bbd49c2d2c605073799671074bbb30` |
| [zephyrproject-rtos/fatfs](https://github.com/zephyrproject-rtos/fatfs) | `f4ead3bf4a6dab3a07d7b5f5315795c073db568d` |
| [zephyrproject-rtos/hal_nxp](https://github.com/zephyrproject-rtos/hal_nxp) | `53946adb08b54449391c1c0c73cdd094d92a3b6e` |
| [zephyrproject-rtos/hal_tdk](https://github.com/zephyrproject-rtos/hal_tdk) | `0f209bd0a4b6511c8c6ffddda75da1fe6988df8f` |
| [zephyrproject-rtos/segger](https://github.com/zephyrproject-rtos/segger) | `50892fdbcf2f570e67baa72b8894a66b16946f72` |
| [CogniPilot/zros](https://github.com/CogniPilot/zros) | `20ab07983224ca078f17e2e851b4fe8cb92eb517` |
| [CogniPilot/csyn](https://github.com/CogniPilot/csyn) | `c34dd35d7b81f33b1480fda07558f26617e85a26` |
| [CogniPilot/cerebri_modules](https://github.com/CogniPilot/cerebri_modules) | `ef73a4f8adeb4385c34af4344c9af27e43e02033` |
| [CogniPilot/modelica_models](https://github.com/CogniPilot/modelica_models) | `a9e5037ab3e57b3fac6ca783c0bdbfdd2b6dd98e` |
| [jgoppert/FastDyn](https://github.com/jgoppert/FastDyn) | `94c85f3e47be410e8d760b7f8207eb1580d7cd4c` |
| [CogniPilot/zenoh-pico](https://github.com/CogniPilot/zenoh-pico) | `f16312bcb357481c94ae1a336ecdd08687d11c87` |
| [CogniPilot/zephyr_boards](https://github.com/CogniPilot/zephyr_boards) | `1d7c17e875938082f024e80d114ac82c25aa53a9` |
| [zephyrproject-rtos/mcuboot](https://github.com/zephyrproject-rtos/mcuboot) | `c38f7a10c14ae8b95c0296cd10320907c28e02c7` |
| [zephyrproject-rtos/zcbor](https://github.com/zephyrproject-rtos/zcbor) | `9164bd18dcd88ff9d9ef98279501fc1093571017` |

The checked-out West workspace used to prepare this manifest matched all of
these revisions and had no local modifications in the listed repositories.

## Nix-locked repositories

These revisions are recorded in `flake.lock`. The first two are direct inputs;
the remainder are transitive inputs used by the pinned Rumoca package.

| Repository | Role | Revision |
|---|---|---|
| [NixOS/nixpkgs](https://github.com/NixOS/nixpkgs) | Firmware build environment | `b5aa0fbd538984f6e3d201be0005b4463d8b09f8` |
| [CogniPilot/rumoca](https://github.com/CogniPilot/rumoca) | Modelica/eFMI compiler | `9860c30781242ff65dfcf47b136385ac5ecf4350` |
| [ipetkov/crane](https://github.com/ipetkov/crane) | Rumoca Nix build input | `80db5bdc391be8a1794f6d8a2d56e3a84ebcede2` |
| [nix-community/fenix](https://github.com/nix-community/fenix) | Rumoca Rust toolchain input | `16810aa8f4ad89ca480b1513774e8b6f485fe368` |
| [numtide/flake-utils](https://github.com/numtide/flake-utils) | Rumoca Nix utility input | `11707dc2f618dd54ca8739b309ec4fc024de578b` |
| [NixOS/nixpkgs](https://github.com/NixOS/nixpkgs) | Rumoca package set | `0ad6f47ea4fe188f4bc8f0380f93ae8523337c6c` |
| [NixOS/nixpkgs](https://github.com/NixOS/nixpkgs) | OpenModelica package set | `fd1462031fdee08f65fd0b4c6b64e22239a77870` |
| [jgoppert/OpenModelica](https://github.com/jgoppert/OpenModelica) | Modelica reference/runtime tooling | `a96aa1a682c463b0fd2d285b486c09a8b7fe496d` |
| [rust-lang/rust-analyzer](https://github.com/rust-lang/rust-analyzer) | Fenix development input | `0d381ca097a8e0375a19387874d952c0a230ac4f` |
| [nix-systems/default](https://github.com/nix-systems/default) | Flake systems input | `da67096a3b9bf56a91d16901293e51ba5b49a27e` |

## Reproducing the checkout

```bash
git clone https://github.com/CogniPilot/cerebri_rdd2.git
cd cerebri_rdd2
git checkout bdc71b44c128c8186292bd9afbb1a4ab65d0fd86
nix run .#west-update
```

Confirm the firmware checkout before building:

```bash
test "$(git rev-parse HEAD)" = \
  bdc71b44c128c8186292bd9afbb1a4ab65d0fd86
git status --short
```

Build the normal RDD2 target with:

```bash
nix run .#build
```

## Current qualification caveat

This manifest reproduces the latest **pushed** `rumoca-efmu-update` branch. It
does not yet include the newer checked FMI compiler changes or the hardened
RDD2 estimator source that are still being integrated and reviewed. In
particular, the branch currently pins Rumoca at `9860c307...` and
`modelica_models` at `a9e5037...`.

Treat firmware produced from this manifest as the current reproducible branch,
not as the final flight-qualified GPS mission image. Update this document in
the same commit whenever either pin changes.
