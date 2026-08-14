# RDD2 Firmware Revision Manifest

This document records the repository revisions used by the validated RDD2 GPS
firmware branch as of 2026-08-14.

## Firmware repository

- Repository: <https://github.com/CogniPilot/cerebri_rdd2>
- Branch: `rumoca-efmu-update`
- Commit: `fb3cfecfa37e22e1ca143c16c2dc41ac9ca83c94`
- Commit subject: `Pin hardened eFMI stack for RDD2 GPS flight`

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
| [CogniPilot/modelica_models](https://github.com/CogniPilot/modelica_models) | `a41f7c0c00b55c1bf54f03c9b66b901ba8e43c6f` |
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
| [CogniPilot/rumoca](https://github.com/CogniPilot/rumoca) | Modelica/eFMI compiler | `4d0e521d9a0bd2527808dbce2c5689834d1a0349` |
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
git checkout fb3cfecfa37e22e1ca143c16c2dc41ac9ca83c94
nix run .#west-update
```

Confirm the firmware checkout before building:

```bash
test "$(git rev-parse HEAD)" = \
  fb3cfecfa37e22e1ca143c16c2dc41ac9ca83c94
git status --short
```

Build the normal RDD2 target with:

```bash
RDD2_BUILD_DIR="$PWD/build-mr_vmu_tropic" nix run .#build
```

The merged MCUboot/application image is written to
`build-mr_vmu_tropic/merged_mr_vmu_tropic_mimxrt1064.hex`.

## Validation receipts

The exact revisions above passed these local gates:

- `nix run .#test-gps-lockstep` exited successfully, including the mandatory
  native lifecycle and all focused GPS, mission, generated-controller, wrapper,
  scheduler, and lockstep firmware suites.
- The normal `mr_vmu_tropic/mimxrt1064` sysbuild linked successfully with CRSF,
  ICM45686 streaming, onboard GNSS, MCUboot, and the generated eFMI controllers.
- `nix run .#fastdyn-ci` passed the 44-second GPS mission using the Rumoca FMI
  3.0.2 Co-Simulation plant and the MR-VMU firmware under FastDyn/QEMU.

The FastDyn acceptance report recorded `passed=true`, all five planner and
vehicle square corners, continuous POSITION mission operation, final disarm,
1,600/1,000/200/50 Hz rate/navigation/guidance/planning releases, maximum
horizontal navigation error `0.01799 m`, and maximum altitude `1.54 m`.

## Current qualification caveat

This is the current reproducible GPS flight-test candidate, but simulation and
link receipts do not replace the props-off hardware checklist. Before a
propeller-on flight, verify MCUboot handoff, ICM45686 streaming, CRSF input and
kill/failsafe behavior, M10 configuration/ACK and stable fixes, motor order and
direction, calibration, time synchronization, and the operator-visible mission
lifecycle. The build currently uses MCUboot's development signing key and is
not a production-secure release image.
