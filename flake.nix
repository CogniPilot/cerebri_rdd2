{
  description = "Nix development and host support for the cerebri_rdd2 Zephyr app";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    rumoca.url = "github:CogniPilot/rumoca/4d0e521d9a0bd2527808dbce2c5689834d1a0349";
  };

  outputs =
    {
      self,
      nixpkgs,
      rumoca,
    }:
    let
      lib = nixpkgs.lib;
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = lib.genAttrs supportedSystems;
      pkgsFor =
        system:
        import nixpkgs {
          inherit system;
          config = {
            allowUnfreePredicate =
              pkg:
              builtins.elem (lib.getName pkg) [
                "segger-jlink"
                "segger-systemview"
              ];
            segger-jlink.acceptLicense = true;
          };
        };
      mkPythonEnv =
        pkgs:
        pkgs.python3.withPackages (
          ps: with ps; [
            anytree
            cbor2
            # Zephyr Twister imports these build/run-test modules eagerly.
            click
            colorama
            cryptography
            intelhex
            jinja2
            jsonschema
            junitparser
            matplotlib
            natsort
            packaging
            ply
            psutil
            pyelftools
            pykwalify
            pytest
            python-dotenv
            # Required by Zephyr's jlink runner: runners/jlink.py imports
            # pylink and refuses to run without it.
            pylink-square
            pyserial
            pyyaml
            requests
            semver
            setuptools
            tabulate
            tqdm
            west
          ]
        );
      mkFastDynCiTools =
        pkgs: with pkgs; [
          cargo
          rustc
          rustfmt
          universal-ctags
          meson
          jq
          pkg-config
          protobuf
        ];
      mkFastDynCiLibraries =
        pkgs: with pkgs; [
          cjson
          dtc
          expat
          glib
          openssl
          pixman
          systemd
          zlib
        ];
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          pythonEnv = mkPythonEnv pkgs;
          flightRumoca = rumoca.packages.${system}.default;
          hostCc = if system == "x86_64-linux" then pkgs.gcc_multi else pkgs.stdenv.cc;
          sdkPlatform = if system == "x86_64-linux" then "x86_64" else "aarch64";
          sdkMinimalHash =
            if system == "x86_64-linux" then
              "sha256-ypvA/2b6/KHaydWSo22VPPFtCWqdCbHANX8CHPn2p+s="
            else
              "sha256-15xb/GjmeUiGWb6iiaQCblKmTwMziHXIychQ//E87jA=";
          sdkArmHash =
            if system == "x86_64-linux" then
              "sha256-IbhZgctaGBjZvFPYKvgPIIlG7AOLmC/xkHKHVy7TpjQ="
            else
              "sha256-uYBbaR8vCokmySaUyuN40Fuge3arynReIW/MUnU8xNY=";
          zephyrSdk = pkgs.stdenvNoCC.mkDerivation {
            pname = "zephyr-sdk-arm";
            version = "1.0.1";
            sdkMinimal = pkgs.fetchurl {
              url = "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-${sdkPlatform}_minimal.tar.xz";
              hash = sdkMinimalHash;
            };
            sdkArm = pkgs.fetchurl {
              url = "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/toolchain_gnu_linux-${sdkPlatform}_arm-zephyr-eabi.tar.xz";
              hash = sdkArmHash;
            };
            nativeBuildInputs = [ pkgs.autoPatchelfHook ];
            buildInputs = with pkgs; [
              ncurses
              python312
              stdenv.cc.cc.lib
              zlib
            ];
            dontConfigure = true;
            dontBuild = true;
            unpackPhase = ''
              runHook preUnpack
              mkdir source
              tar -xJf "$sdkMinimal" --strip-components=1 -C source
              mkdir source/gnu
              tar -xJf "$sdkArm" -C source/gnu
              runHook postUnpack
            '';
            installPhase = ''
              runHook preInstall
              cp -R source "$out"
              runHook postInstall
            '';
            meta = {
              description = "Zephyr SDK with the GNU Arm toolchain";
              homepage = "https://github.com/zephyrproject-rtos/sdk-ng";
              license = lib.licenses.asl20;
              platforms = supportedSystems;
            };
          };
          jlinkCli = pkgs.segger-jlink-headless.overrideAttrs {
            postInstall = ''
              install -Dm755 JLinkExe $out/opt/SEGGER/JLink/JLinkExe
              # Zephyr's jlink runner spawns a GDB server named JLinkGDBServer;
              # upstream ships that as a symlink to the CL binary, which is the
              # headless one. RTOSPlugin_Zephyr.so is what makes gdb see threads
              # instead of one opaque context, so it belongs next to the server.
              install -Dm755 JLinkGDBServerCLExe $out/opt/SEGGER/JLink/JLinkGDBServerCLExe
              install -Dm644 GDBServer/RTOSPlugin_Zephyr.so \
                $out/opt/SEGGER/JLink/GDBServer/RTOSPlugin_Zephyr.so
              mkdir -p $out/bin
              ln -s $out/opt/SEGGER/JLink/JLinkExe $out/bin/JLinkExe
              ln -s $out/opt/SEGGER/JLink/JLinkExe $out/bin/JLink
              ln -s $out/opt/SEGGER/JLink/JLinkGDBServerCLExe $out/bin/JLinkGDBServerCLExe
              ln -s $out/opt/SEGGER/JLink/JLinkGDBServerCLExe $out/bin/JLinkGDBServer
            '';
          };
          systemView = pkgs.stdenv.mkDerivation {
            pname = "segger-systemview";
            version = "4.10b";
            src = pkgs.fetchurl {
              name = "SystemView_Linux_V410b_x86_64.tgz";
              url = "https://www.segger.com/downloads/systemview/systemview_linux_tgz64";
              hash = "sha256-RFH+ZfMp+VO8q8W91Q0GzAbPEJgUWfIKWYu+dGdagRc=";
              curlOpts = "--data accept_license_agreement=accepted";
            };
            nativeBuildInputs = [ pkgs.autoPatchelfHook ];
            buildInputs = with pkgs; [
              fontconfig
              freetype
              libGL
              libice
              libsm
              libx11
              libxcursor
              libxext
              libxfixes
              libxrandr
              libxrender
              stdenv.cc.cc.lib
            ];
            dontBuild = true;
            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin $out/opt/SEGGER/SystemView
              cp -R . $out/opt/SEGGER/SystemView
              ln -s ${jlinkCli}/opt/SEGGER/JLink/libjlinkarm.so \
                $out/opt/SEGGER/SystemView/libjlinkarm.so
              ln -s $out/opt/SEGGER/SystemView/SystemView $out/bin/SystemView
              runHook postInstall
            '';
            meta = {
              description = "SEGGER SystemView real-time software analysis tool";
              homepage = "https://www.segger.com/products/development-tools/systemview/";
              license = lib.licenses.unfree;
              platforms = [ "x86_64-linux" ];
            };
          };
          hostMultilibTools = lib.optionals (system == "x86_64-linux") [
            pkgs.glibc_multi.dev
            systemView
          ];

          baseTools = [
            pkgs.ccache
            pkgs.cmake
            pkgs.coreutils
            pkgs.curl
            pkgs.dtc
            pkgs.file
            pkgs.findutils
            pkgs.git
            pkgs.gitRepo
            pkgs.gnumake
            pkgs.gnugrep
            pkgs.gnused
            pkgs.gperf
            pkgs.ncurses
            pkgs.ninja
            pkgs.openocd
            pkgs.openssh
            pkgs.picocom
            pkgs.pkg-config
            pkgs.python3Packages.pyocd
            pkgs.screen
            jlinkCli
            hostCc
            pkgs.unzip
            pkgs.util-linux
            pkgs.which
            pkgs.xz
            pkgs.zip
            pythonEnv
            zephyrSdk
          ]
          ++ hostMultilibTools;

          fmiStandard = pkgs.fetchzip {
            url = "https://github.com/modelica/fmi-standard/releases/download/v3.0.2/FMI-Standard-3.0.2.zip";
            hash = "sha256-AaFIArg4re9a5mPrUUfJPhI1e+RxHhL/DG+LWmqqdSM=";
            stripRoot = false;
          };

          workspaceScript = ''
            rdd2_find_app() {
              local dir
              dir="$(pwd -P)"

              while [ "$dir" != "/" ]; do
                if [ -f "$dir/west.yml" ] && [ -f "$dir/prj.conf" ] && [ -f "$dir/CMakeLists.txt" ]; then
                  printf '%s\n' "$dir"
                  return 0
                fi

                if [ -f "$dir/cerebri_rdd2/west.yml" ] && [ -f "$dir/cerebri_rdd2/prj.conf" ]; then
                  printf '%s\n' "$dir/cerebri_rdd2"
                  return 0
                fi

                dir="$(dirname "$dir")"
              done

              printf 'error: could not find the cerebri_rdd2 app from %s\n' "$(pwd -P)" >&2
              printf 'run this command from the app directory or its west workspace root\n' >&2
              return 1
            }

            rdd2_source_workspace() {
              dirname "$1"
            }

            rdd2_active_manifest() {
              local dir="$1"
              (cd "$dir" && west manifest --path 2>/dev/null || true)
            }

            rdd2_managed_workspace() {
              local app="$1"

              if [ -n "''${RDD2_WEST_WORKSPACE:-}" ]; then
                realpath -m "$RDD2_WEST_WORKSPACE"
                return 0
              fi

              printf '%s\n' "$app/.devenv/state/west"
            }

            rdd2_workspace() {
              local app="$1"
              local source_workspace
              local expected_manifest
              local actual_manifest

              if [ -n "''${RDD2_WEST_WORKSPACE:-}" ]; then
                rdd2_managed_workspace "$app"
                return 0
              fi

              source_workspace="$(rdd2_source_workspace "$app")"
              expected_manifest="$(realpath "$app/west.yml")"
              actual_manifest="$(rdd2_active_manifest "$app")"

              if [ -n "$actual_manifest" ]; then
                actual_manifest="$(realpath "$actual_manifest")"
              fi

              if [ "$actual_manifest" = "$expected_manifest" ]; then
                printf '%s\n' "$source_workspace"
              else
                rdd2_managed_workspace "$app"
              fi
            }

            rdd2_prepare_managed_workspace() {
              local app="$1"
              local workspace="$2"
              local manifest_dir="$workspace/manifest"
              local manifest_path="$manifest_dir/west.yml"
              local actual_manifest

              mkdir -p "$manifest_dir"
              if [ ! -d "$manifest_dir/.git" ]; then
                git init -q "$manifest_dir"
              fi

              cp "$app/west.yml" "$manifest_path"
              (
                cd "$manifest_dir"
                git add west.yml
                if ! git rev-parse --verify HEAD >/dev/null 2>&1 ||
                   ! git diff --cached --quiet; then
                  git -c user.name='cerebri-rdd2 nix' \
                      -c user.email='cerebri-rdd2-nix@example.invalid' \
                      commit -q -s -m 'Update cerebri_rdd2 manifest'
                fi
              )

              if [ ! -d "$workspace/.west" ]; then
                mkdir -p "$workspace/.west"
                printf '[manifest]\npath = manifest\nfile = west.yml\n' >"$workspace/.west/config"
              fi

              actual_manifest="$(rdd2_active_manifest "$workspace")"
              if [ -z "$actual_manifest" ]; then
                printf 'error: could not read the managed west manifest in %s\n' "$workspace" >&2
                return 1
              fi
              actual_manifest="$(realpath "$actual_manifest")"
              if [ "$actual_manifest" != "$(realpath "$manifest_path")" ]; then
                printf 'error: managed workspace %s uses unexpected manifest %s\n' "$workspace" "$actual_manifest" >&2
                printf '       expected %s\n' "$manifest_path" >&2
                return 1
              fi
            }
          '';

          # Shared by every command that drives the probe: flashing and
          # attaching a debugger fail the same way when the udev rule is
          # missing, and the message is the useful part.
          jlinkAccessScript = ''
            rdd2_require_jlink_access() {
              local sysdev busnum devnum usbdev

              for sysdev in /sys/bus/usb/devices/*; do
                [ -r "$sysdev/idVendor" ] || continue
                [ "$(<"$sysdev/idVendor")" = "1366" ] || continue
                [ -r "$sysdev/busnum" ] || continue
                [ -r "$sysdev/devnum" ] || continue

                busnum="$(<"$sysdev/busnum")"
                devnum="$(<"$sysdev/devnum")"
                usbdev="$(printf '/dev/bus/usb/%03d/%03d' "$((10#$busnum))" "$((10#$devnum))")"

                if [ ! -r "$usbdev" ] || [ ! -w "$usbdev" ]; then
                  printf 'error: J-Link USB device %s is not accessible to user %s\n' "$usbdev" "$(id -un)" >&2
                  printf '       on NixOS, add the SEGGER udev rule documented in README.md and rebuild the system\n' >&2
                  printf '       then reconnect the probe and try again\n' >&2
                  return 1
                fi
              done
            }
          '';

          commonScript = ''
                        ${workspaceScript}

                        export LD_LIBRARY_PATH="${
                          lib.makeLibraryPath [
                            pkgs.stdenv.cc.cc.lib
                            pkgs.systemd
                          ]
                        }:${jlinkCli}/opt/SEGGER/JLink''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

                        rdd2_require_module_paths() {
                          local workspace="$1"
                          local missing=0
                          local path

                          for path in \
                            zephyr \
                            modules/hal/cmsis \
                            modules/hal/cmsis_6 \
                            modules/hal/nxp \
                            modules/debug/segger \
                            modules/fs/fatfs \
                            modules/lib/cmsis-dsp \
                            modules/lib/zcbor \
                            modules/lib/zenoh-pico \
                            modules/lib/zephyr_boards \
                            bootloader/mcuboot
                          do
                            if [ ! -d "$workspace/$path" ]; then
                              printf 'error: missing required west checkout: %s/%s\n' "$workspace" "$path" >&2
                              missing=1
                            fi
                          done

                          for path in \
                            "$RDD2_CEREBRI_MODULES_ROOT" \
                            "$RDD2_ZROS_ROOT" \
                            "$RDD2_CSYN_ROOT" \
                            "$RDD2_MODELICA_MODELS_ROOT"
                          do
                            if [ ! -d "$path" ]; then
                              printf 'error: missing required editable module: %s\n' "$path" >&2
                              missing=1
                            fi
                          done

                          if [ "$missing" -ne 0 ]; then
                            printf 'run: nix run .#west-update\n' >&2
                            return 1
                          fi
                        }

                        rdd2_export_common() {
                          local app="$1"
                          local workspace
                          workspace="$(rdd2_workspace "$app")"

                          export WEST_PYTHON="''${WEST_PYTHON:-${pythonEnv}/bin/python}"
                          export RDD2_CEREBRI_MODULES_ROOT="''${RDD2_CEREBRI_MODULES_ROOT:-$workspace/modules/lib/cerebri_lockstep}"
                          export RDD2_ZROS_ROOT="''${RDD2_ZROS_ROOT:-$workspace/modules/lib/zros}"
                          export RDD2_CSYN_ROOT="''${RDD2_CSYN_ROOT:-$workspace/modules/lib/csyn}"
                          export RDD2_MODELICA_MODELS_ROOT="''${RDD2_MODELICA_MODELS_ROOT:-$workspace/models/modelica_models}"
                          export ZEPHYR_SDK_INSTALL_DIR="''${ZEPHYR_SDK_INSTALL_DIR:-${zephyrSdk}}"
                          export RDD2_WORKSPACE_ROOT="$workspace"
                          export WEST_TOPDIR="$workspace"
                          export FASTDYN_ROOT="''${FASTDYN_ROOT:-$workspace/modules/sim/fastdyn}"
                          export FASTDYN_STATE="''${FASTDYN_STATE:-$app/.devenv/state/fastdyn}"
                          export FASTDYN_EXECUTABLE="''${FASTDYN_EXECUTABLE:-$FASTDYN_STATE/venv/bin/fastdyn}"
                          export FASTDYN_QEMU_PATH="''${FASTDYN_QEMU_PATH:-$FASTDYN_STATE/qemu/build/qemu-system-arm}"
                          export FASTDYN_MONITOR_ELF="''${FASTDYN_MONITOR_ELF:-$FASTDYN_STATE/qemu/ws/monitor.elf}"
                          if [ -d "$workspace/zephyr" ]; then
                            export ZEPHYR_BASE="$workspace/zephyr"
                          fi
                        }

                        rdd2_require_workspace() {
                          local app="$1"
                          local workspace
                          local actual_manifest
                          workspace="$(rdd2_workspace "$app")"

                          if [ ! -d "$workspace/.west" ]; then
                            printf 'error: missing west workspace metadata at %s/.west\n' "$workspace" >&2
                            printf 'initialize/update it with: nix run .#west-update\n' >&2
                            return 1
                          fi

                          actual_manifest="$(rdd2_active_manifest "$workspace")"
                          if [ -z "$actual_manifest" ]; then
                            printf 'error: could not read the active west manifest in %s\n' "$workspace" >&2
                            return 1
                          fi
                          actual_manifest="$(realpath "$actual_manifest")"

                          if [ -z "''${ZEPHYR_BASE:-}" ] || [ ! -d "$ZEPHYR_BASE" ]; then
                            printf 'error: missing Zephyr checkout; expected %s/zephyr or ZEPHYR_BASE\n' "$workspace" >&2
                            printf 'run: nix run .#west-update\n' >&2
                            return 1
                          fi

                          rdd2_require_module_paths "$workspace"
                        }

                        rdd2_require_mcuboot_sysbuild() {
                          local build_dir="$1"
                          local domains="$build_dir/domains.yaml"
                          local main_image
                          local main_config
                          local boot_config="$build_dir/mcuboot/zephyr/.config"
                          local merged_hex
                          local required
                          local -a merged_hexes

                          if [ ! -f "$domains" ]; then
                            printf 'error: %s is not a sysbuild output; refusing an app-only MCUboot image\n' \
                              "$build_dir" >&2
                            return 1
                          fi

                          main_image="$(sed -n 's/^default: //p' "$domains")"
                          if [ -z "$main_image" ] || [ "$main_image" = "mcuboot" ]; then
                            printf 'error: invalid default sysbuild image in %s\n' "$domains" >&2
                            return 1
                          fi
                          main_config="$build_dir/$main_image/zephyr/.config"

                          mapfile -t merged_hexes < <(
                            find "$build_dir" -maxdepth 1 -type f -name 'merged_*.hex' -print
                          )
                          if [ "''${#merged_hexes[@]}" -ne 1 ]; then
                            printf 'error: expected exactly one merged sysbuild HEX in %s, found %d\n' \
                              "$build_dir" "''${#merged_hexes[@]}" >&2
                            return 1
                          fi
                          merged_hex="''${merged_hexes[0]}"

                          for required in \
                            "$build_dir/mcuboot/zephyr/zephyr.hex" \
                            "$build_dir/$main_image/zephyr/zephyr.signed.bin" \
                            "$build_dir/$main_image/zephyr/zephyr.signed.hex" \
                            "$merged_hex" \
                            "$main_config" \
                            "$boot_config"
                          do
                            if [ ! -s "$required" ]; then
                              printf 'error: incomplete MCUboot sysbuild output: %s\n' "$required" >&2
                              return 1
                            fi
                          done

                          if ! grep -qx 'CONFIG_BOOTLOADER_MCUBOOT=y' "$main_config" ||
                             ! grep -q '^CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="..*"$' "$main_config" ||
                             ! grep -qx 'CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y' "$boot_config" ||
                             ! grep -qx 'CONFIG_BOOT_ECDSA_TINYCRYPT=y' "$boot_config"; then
                            printf 'error: MCUboot image/signature configuration is incomplete\n' >&2
                            return 1
                          fi

                          if ! grep -qx '  - mcuboot' "$domains" ||
                             ! grep -qx "  - $main_image" "$domains"; then
                            printf 'error: sysbuild flash order does not contain MCUboot and %s\n' \
                              "$main_image" >&2
                            return 1
                          fi

                          printf '[firmware] verified MCUboot sysbuild artifacts\n'
                          sha256sum \
                            "$build_dir/mcuboot/zephyr/zephyr.hex" \
                            "$build_dir/$main_image/zephyr/zephyr.signed.bin" \
                            "$merged_hex"
                        }

                        rdd2_require_flight_configuration() {
                          local build_dir="$1"
                          local domains="$build_dir/domains.yaml"
                          local main_image
                          local main_config
                          local setting

                          main_image="$(sed -n 's/^default: //p' "$domains")"
                          main_config="$build_dir/$main_image/zephyr/.config"

                          for setting in \
                            CONFIG_INPUT_CRSF=y \
                            CONFIG_ICM45686_STREAM=y \
                            CONFIG_RDD2_GNSS_SOURCE_ONBOARD=y \
                            CONFIG_NET_GPTP=y \
                            CONFIG_NET_GPTP_STATIC_TIME_RECEIVER=y \
                            CONFIG_CSYN_ZENOH=y
                          do
                            if ! grep -qx "$setting" "$main_config"; then
                              printf 'error: flight image is missing required setting %s\n' \
                                "$setting" >&2
                              return 1
                            fi
                          done

                          if grep -qx 'CONFIG_RDD2_COMMS_STUB=y' "$main_config"; then
                            printf 'error: refusing the non-flyable communications stub as flight firmware\n' >&2
                            return 1
                          fi
                        }

                        rdd2_require_optical_flow_configuration() {
                          local build_dir="$1"
                          local domains="$build_dir/domains.yaml"
                          local main_image
                          local main_config

                          rdd2_require_flight_configuration "$build_dir"
                          main_image="$(sed -n 's/^default: //p' "$domains")"
                          main_config="$build_dir/$main_image/zephyr/.config"
                          if ! grep -qx 'CONFIG_RDD2_OPTICAL_FLOW_SOURCE_CSYN=y' \
                              "$main_config"; then
                            printf 'error: optical-flow firmware is missing its CSyn source\n' >&2
                            return 1
                          fi
                        }

                        rdd2_require_comms_stub_configuration() {
                          local build_dir="$1"
                          local domains="$build_dir/domains.yaml"
                          local main_image
                          local main_config
                          local setting

                          main_image="$(sed -n 's/^default: //p' "$domains")"
                          main_config="$build_dir/$main_image/zephyr/.config"

                          for setting in \
                            CONFIG_RDD2_COMMS_STUB=y \
                            CONFIG_INPUT_CRSF=y \
                            CONFIG_ICM45686_STREAM=y \
                            CONFIG_RDD2_GNSS_SOURCE_ONBOARD=y \
                            CONFIG_NET_GPTP=y \
                            CONFIG_NET_GPTP_STATIC_TIME_RECEIVER=y \
                            CONFIG_CSYN_ZENOH=y
                          do
                            if ! grep -qx "$setting" "$main_config"; then
                              printf 'error: communications bench image is missing required setting %s\n' \
                                "$setting" >&2
                              return 1
                            fi
                          done

                          printf '[firmware] verified communications bench hardware configuration\n'
                        }

                        rdd2_require_fastdyn_runtime() {
                          local missing=0

                          if [ ! -f "$FASTDYN_ROOT/setup.sh" ]; then
                            printf 'error: missing pinned FastDyn checkout at %s\n' "$FASTDYN_ROOT" >&2
                            printf '       run: rdd2-west-update\n' >&2
                            return 1
                          fi

                          for path in \
                            "$FASTDYN_EXECUTABLE" \
                            "$FASTDYN_QEMU_PATH" \
                            "$FASTDYN_MONITOR_ELF" \
                            "$FASTDYN_ROOT/build/libfastdyn.so"
                          do
                            if [ ! -e "$path" ]; then
                              printf 'error: missing FastDyn runtime artifact: %s\n' "$path" >&2
                              missing=1
                            fi
                          done

                          if [ -x "$FASTDYN_EXECUTABLE" ] &&
                             ! "$FASTDYN_EXECUTABLE" --help >/dev/null 2>&1; then
                            printf 'error: FastDyn launcher is not runnable: %s\n' \
                              "$FASTDYN_EXECUTABLE" >&2
                            missing=1
                          fi

                          if [ "$missing" -ne 0 ]; then
                            printf '       prepare the pinned runtime with: rdd2-fastdyn-setup\n' >&2
                            return 1
                          fi
                        }

                        rdd2_fastdyn_runtime_ready() {
                          local revision
                          local stamp="$FASTDYN_STATE/runtime.revision"

                          revision="$(git -C "$FASTDYN_ROOT" rev-parse HEAD 2>/dev/null || true)"
                          [ -x "$FASTDYN_EXECUTABLE" ] &&
                            "$FASTDYN_EXECUTABLE" --help >/dev/null 2>&1 &&
                            [ -x "$FASTDYN_QEMU_PATH" ] &&
                            [ -f "$FASTDYN_MONITOR_ELF" ] &&
                            [ -f "$FASTDYN_ROOT/build/libfastdyn.so" ] &&
                            [ -n "$revision" ] &&
                            [ -f "$stamp" ] &&
                            [ "$(<"$stamp")" = "1:$revision" ]
                        }

                        rdd2_ensure_workspace() {
                          local app="$1"
                          local update_command="$2"
                          local workspace
                          local active_manifest
                          local needs_update=0

                          workspace="$(rdd2_workspace "$app")"
                          if [ ! -d "$workspace/.west" ]; then
                            needs_update=1
                          else
                            active_manifest="$(rdd2_active_manifest "$workspace")"
                            if [ -z "$active_manifest" ]; then
                              needs_update=1
                            elif [ "$(realpath "$active_manifest")" != "$(realpath "$app/west.yml")" ] &&
                                 ! cmp -s "$app/west.yml" "$active_manifest"; then
                              needs_update=1
                            fi
                          fi

                          for path in \
                            zephyr \
                            modules/sim/fastdyn \
                            models/modelica_models
                          do
                            if [ ! -d "$workspace/$path" ]; then
                              needs_update=1
                            fi
                          done

                          if [ "$needs_update" -ne 0 ]; then
                            printf '[deps] updating the pinned West workspace\n'
                            "$update_command"
                          fi
                        }

                        rdd2_require_fastdyn_model_sources() {
                          local missing=0
                          local relative
                          local revision

                          revision="$(git -C "$RDD2_MODELICA_MODELS_ROOT" rev-parse HEAD 2>/dev/null || true)"

                          for relative in \
                            Planning/Bezier/WaypointTrajectoryPlanner.mo \
                            Vehicles/Rdd2/GuidanceController.mo \
                            Vehicles/Rdd2/NavigationEstimator.mo \
                            Vehicles/Rdd2/RateControlAllocator.mo \
                            Vehicles/Rdd2/Plant.mo
                          do
                            if [ ! -f "$RDD2_MODELICA_MODELS_ROOT/$relative" ]; then
                              printf 'error: pinned Modelica dependency is missing %s\n' "$relative" >&2
                              missing=1
                            fi
                          done

                          if [ "$missing" -ne 0 ]; then
                            printf '       modelica_models revision: %s\n' "''${revision:-unknown}" >&2
                            printf '       west.yml must pin a published commit containing these interfaces\n' >&2
                            return 1
                          fi
                        }

                        rdd2_require_pinned_modelica_checkout() {
                          local project="$RDD2_WORKSPACE_ROOT/models/modelica_models"
                          local manifest_revision
                          local expected_revision
                          local actual_revision

                          if [ "$(realpath "$RDD2_MODELICA_MODELS_ROOT")" != "$(realpath "$project")" ]; then
                            printf '[deps] using editable Modelica sources: %s\n' \
                              "$RDD2_MODELICA_MODELS_ROOT"
                            return 0
                          fi

                          manifest_revision="$(
                            cd "$RDD2_WORKSPACE_ROOT"
                            west list modelica_models -f '{revision}'
                          )"
                          expected_revision="$(git -C "$project" rev-parse "$manifest_revision^{commit}")"
                          actual_revision="$(git -C "$project" rev-parse HEAD)"

                          if [ "$actual_revision" != "$expected_revision" ]; then
                            printf 'error: modelica_models checkout is not at the west.yml revision\n' >&2
                            printf '       expected: %s\n' "$expected_revision" >&2
                            printf '       actual:   %s\n' "$actual_revision" >&2
                            return 1
                          fi
                          if [ -n "$(git -C "$project" status --porcelain)" ]; then
                            printf 'error: modelica_models checkout has local modifications: %s\n' "$project" >&2
                            printf '       FastDyn CI requires the exact clean revision pinned by west.yml\n' >&2
                            return 1
                          fi

                          export RDD2_MODELICA_MODELS_ROOT="$project"
                        }

                        rdd2_build_fastdyn_firmware() {
                          local app="$1"
                          local board="''${RDD2_BOARD:-mr_vmu_tropic}"
                          local conf_file="$app/fastdyn/prj.conf"
                          local overlay="$app/fastdyn/mr_vmu_tropic.overlay"

                          export RDD2_FASTDYN_BUILD_DIR="''${RDD2_FASTDYN_BUILD_DIR:-$app/build-mr_vmu_tropic-fastdyn}"
                          export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"
                          rdd2_require_fastdyn_model_sources

                          printf '[deps] building the FastDyn firmware image incrementally\n'
                          (
                            cd "$RDD2_WORKSPACE_ROOT"
                            west build -p always -b "$board" -d "$RDD2_FASTDYN_BUILD_DIR" "$app" -- \
                              -DCONF_FILE="$conf_file" \
                              -DDTC_OVERLAY_FILE="$overlay" \
                              -DRDD2_MODELICA_MODELS_ROOT="$RDD2_MODELICA_MODELS_ROOT" \
                              -DRDD2_RUMOCA_EXECUTABLE="$RDD2_RUMOCA_EXECUTABLE" \
                              -DRDD2_RUMOCA_EXECUTABLE_SHA256="$RDD2_RUMOCA_EXECUTABLE_SHA256" \
                          )

                          if [ ! -f "$RDD2_FASTDYN_BUILD_DIR/zephyr/zephyr.elf" ]; then
                            printf 'error: firmware build did not produce %s/zephyr/zephyr.elf\n' \
                              "$RDD2_FASTDYN_BUILD_DIR" >&2
                            return 1
                          fi
                        }

                        rdd2_prepare_fmi_plant() {
                          local description="''${RDD2_RUMOCA_PLANT_DESCRIPTION:-}"
                          local library="''${RDD2_RUMOCA_PLANT_LIBRARY:-}"
                          local plant_root
                          local model_identifier
                          local binary_dir
                          local source

                          if [ -n "$description" ] && [ -n "$library" ] &&
                             [ -f "$description" ] && [ -f "$library" ]; then
                            export RDD2_RUMOCA_PLANT_DESCRIPTION="$description"
                            export RDD2_RUMOCA_PLANT_LIBRARY="$library"
                            return 0
                          fi

                          printf '[deps] exporting the RDD2 FMI 3 plant\n'
                          if [ -n "$RDD2_RUMOCA_EXECUTABLE" ]; then
                            "$RDD2_RUMOCA_EXECUTABLE" \
                              --cache-dir "$RDD2_FASTDYN_BUILD_DIR/.rumoca-cache" \
                              compile "$RDD2_MODELICA_MODELS_ROOT/Vehicles/Rdd2/Plant.mo" \
                              --model Vehicles.Rdd2.Plant \
                              --source-root "$RDD2_MODELICA_MODELS_ROOT" \
                              --target fmi3 \
                              --output "$RDD2_MODELICA_MODELS_ROOT/artifacts/vehicles/rdd2/plant"
                          else
                            MODELICA_MODELS_ROOT="$RDD2_MODELICA_MODELS_ROOT" \
                              ${pkgs.nix}/bin/nix run "$RDD2_MODELICA_MODELS_ROOT#rdd2-export-plant"
                          fi

                          plant_root="$RDD2_MODELICA_MODELS_ROOT/artifacts/vehicles/rdd2/plant/Vehicles_Rdd2_Plant"
                          description="''${RDD2_RUMOCA_PLANT_DESCRIPTION:-$plant_root/modelDescription.xml}"
                          if [ ! -f "$description" ]; then
                            printf 'error: plant export did not produce %s\n' "$description" >&2
                            return 1
                          fi

                          model_identifier="$(${pythonEnv}/bin/python - "$description" <<'PY'
            import sys
            import xml.etree.ElementTree as ET

            root = ET.parse(sys.argv[1]).getroot()
            interface = root.find("CoSimulation")
            if interface is None or not interface.get("modelIdentifier"):
                raise SystemExit("FMI model description has no CoSimulation modelIdentifier")
            print(interface.get("modelIdentifier"))
            PY
                          )"
                          source="$plant_root/sources/model.c"
                          if [ ! -f "$source" ]; then
                            printf 'error: plant export did not produce %s\n' "$source" >&2
                            return 1
                          fi
                          binary_dir="$plant_root/binaries/${system}"
                          library="''${RDD2_RUMOCA_PLANT_LIBRARY:-$binary_dir/$model_identifier.so}"
                          mkdir -p "$binary_dir"

                          if [ ! -f "$library" ] ||
                             [ "$source" -nt "$library" ] ||
                             [ "$description" -nt "$library" ]; then
                            printf '[deps] compiling the RDD2 FMI 3 plant shared library\n'
                            cc -std=c99 -Wall -Wextra -Wpedantic -Werror -fPIC -shared \
                              -DFMI3_OVERRIDE_FUNCTION_PREFIX=1 \
                              -I"$plant_root/sources" \
                              -I${fmiStandard}/headers \
                              "$source" -lm -o "$library"
                          fi

                          export RDD2_RUMOCA_PLANT_DESCRIPTION="$description"
                          export RDD2_RUMOCA_PLANT_LIBRARY="$library"
                        }
          '';

          # Supply the pinned compiler as a default, never as an override. A
          # caller that selected a provider keeps it; unsetting the variable
          # asks for the pinned one. An override must carry its own digest so
          # the eFMI kernel check verifies the compiler that actually ran, and
          # the digest is checked against the file rather than trusted.
          flightCompilerScript = ''
            if [ -n "''${RDD2_RUMOCA_EXECUTABLE:-}" ]; then
              if [ -z "''${RDD2_RUMOCA_EXECUTABLE_SHA256:-}" ]; then
                printf 'error: RDD2_RUMOCA_EXECUTABLE was supplied without RDD2_RUMOCA_EXECUTABLE_SHA256\n' >&2
                printf '       supply the digest of the selected compiler, or unset both to use the pinned one\n' >&2
                exit 1
              fi
              selected_rumoca_sha256="$(${pkgs.coreutils}/bin/sha256sum \
                "$RDD2_RUMOCA_EXECUTABLE")"
              selected_rumoca_sha256="''${selected_rumoca_sha256%% *}"
              if [ "$selected_rumoca_sha256" != "$RDD2_RUMOCA_EXECUTABLE_SHA256" ]; then
                printf 'error: RDD2_RUMOCA_EXECUTABLE_SHA256 does not match %s\n' \
                  "$RDD2_RUMOCA_EXECUTABLE" >&2
                printf '       declared %s\n       actual   %s\n' \
                  "$RDD2_RUMOCA_EXECUTABLE_SHA256" "$selected_rumoca_sha256" >&2
                exit 1
              fi
            else
              export RDD2_RUMOCA_EXECUTABLE="${flightRumoca}/bin/rumoca"
              pinned_rumoca_sha256="$(${pkgs.coreutils}/bin/sha256sum \
                ${flightRumoca}/bin/rumoca)"
              pinned_rumoca_sha256="''${pinned_rumoca_sha256%% *}"
              export RDD2_RUMOCA_EXECUTABLE_SHA256="$pinned_rumoca_sha256"
            fi
          '';

          mkWestApp =
            name: text:
            pkgs.writeShellApplication {
              inherit name;
              runtimeInputs = baseTools;
              inherit text;
            };

          mkWestUpdateApp =
            name: text:
            pkgs.writeShellApplication {
              inherit name text;
              runtimeInputs = [
                pkgs.coreutils
                pkgs.git
                pkgs.python3Packages.west
              ];
            };

          mkCargoApp =
            name: text:
            pkgs.writeShellApplication {
              inherit name text;
              runtimeInputs = baseTools ++ mkFastDynCiTools pkgs;
            };

          rdd2-build = mkWestApp "rdd2-build" ''
            ${commonScript}
            ${flightCompilerScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_BUILD_DIR:-$app/build-$board_slug}"

            cd "$workspace"
            west build --sysbuild -p always -b "$board" -d "$build_dir" "$app" "$@"
            rdd2_require_mcuboot_sysbuild "$build_dir" "$board"
            rdd2_require_flight_configuration "$build_dir"
          '';

          rdd2-build-comms-stub = mkWestApp "rdd2-build-comms-stub" ''
            ${commonScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_COMMS_STUB_BUILD_DIR:-$app/build-$board_slug-comms-stub}"

            cd "$workspace"
            west build --sysbuild -p always -b "$board" -d "$build_dir" "$app" "$@" -- \
              -DEXTRA_CONF_FILE="$app/comms_stub.conf"
            rdd2_require_mcuboot_sysbuild "$build_dir" "$board"
            rdd2_require_comms_stub_configuration "$build_dir"
          '';

          rdd2-build-optical-flow = mkWestApp "rdd2-build-optical-flow" ''
            ${commonScript}
            ${flightCompilerScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_OPTICAL_FLOW_BUILD_DIR:-$app/build-$board_slug-optical-flow}"

            cd "$workspace"
            west build --sysbuild -p always -b "$board" -d "$build_dir" "$app" "$@" -- \
              -DEXTRA_CONF_FILE="$app/optical_flow.conf"
            rdd2_require_mcuboot_sysbuild "$build_dir" "$board"
            rdd2_require_optical_flow_configuration "$build_dir"
          '';

          rdd2-build-native-sim = mkWestApp "rdd2-build-native-sim" ''
            ${commonScript}
            ${flightCompilerScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-host}"

            board="''${RDD2_NATIVE_SIM_BOARD:-native_sim/native/64}"
            build_dir="''${RDD2_NATIVE_SIM_BUILD_DIR:-$app/build-native_sim}"

            cd "$workspace"
            exec west build -p always -b "$board" -d "$build_dir" "$app" "$@"
          '';

          rdd2-test-gps-lockstep = mkCargoApp "rdd2-test-gps-lockstep" ''
            ${commonScript}
            ${flightCompilerScript}

            app="$(rdd2_find_app)"
            rdd2_ensure_workspace "$app" "${rdd2-west-update}/bin/rdd2-west-update"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-host}"
            export NIX_HARDENING_ENABLE=""

            board="''${RDD2_NATIVE_SIM_BOARD:-native_sim/native/64}"
            build_dir="''${RDD2_GPS_LOCKSTEP_TEST_BUILD_DIR:-$app/build-native_sim-gps-lockstep}"

            cd "$RDD2_WORKSPACE_ROOT"
            west build -p always -b "$board" -d "$build_dir" "$app"

            executable="$build_dir/zephyr/zephyr.exe"
            if [ ! -x "$executable" ]; then
              printf 'error: native GPS lockstep build did not produce %s\n' "$executable" >&2
              exit 1
            fi

            cd "$app"
            RDD2_NATIVE_SIM_EXECUTABLE="$executable" \
              cargo test --workspace --locked \
                native_firmware_exposes_odometry_before_the_one_shot_plan -- \
                --ignored --nocapture

            focused_root="$(mktemp -d -t rdd2-gps-focused.XXXXXXXX)"
            trap 'chmod -R u+w "$focused_root" 2>/dev/null || true; rm -rf -- "$focused_root"' EXIT
            efmi_root="$build_dir/generated/rumoca"
            for suite in \
              mission_shell \
              waypoint_mission_ingress \
              process_wrapper_fault_injection \
              process_control_safety \
              generated_guidance_validity \
              generated_navigation_fault_injection \
              navigation_optical_flow \
              gnss_m10_protocol \
              lockstep_transport \
              gnss_lockstep_source
            do
              suite_build="$focused_root/$suite"
              cd "$RDD2_WORKSPACE_ROOT"
              west build -p always -b "$board" -d "$suite_build" \
                "$app/tests/$suite" -- \
                -DRDD2_TEST_EFMI_ROOT="$efmi_root"
              suite_executable="$suite_build/zephyr/zephyr.exe"
              if [ ! -x "$suite_executable" ]; then
                printf 'error: %s did not produce %s\n' \
                  "$suite" "$suite_executable" >&2
                exit 1
              fi
              "$suite_executable"
            done
          '';

          rdd2-flash = mkWestApp "rdd2-flash" ''
            ${commonScript}
            ${jlinkAccessScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_BUILD_DIR:-$app/build-$board_slug}"
            runner="''${RDD2_FLASH_RUNNER:-jlink}"
            runner_args=()
            flash_domains=()

            if [ -n "$runner" ]; then
              runner_args=(--runner "$runner")
            fi

            if [ "$runner" = "jlink" ]; then
              rdd2_require_jlink_access
            fi

            rdd2_require_mcuboot_sysbuild "$build_dir" "$board"
            rdd2_require_flight_configuration "$build_dir"

            mapfile -t flash_domains < <(
              sed -n '/^flash_order:/,$s/^  - //p' "$build_dir/domains.yaml"
            )
            if [ "''${#flash_domains[@]}" -ne 2 ] ||
              [ "''${flash_domains[0]}" != "mcuboot" ]; then
              printf 'error: refusing unexpected sysbuild flash order\n' >&2
              exit 1
            fi

            cd "$workspace"
            for domain in "''${flash_domains[@]}"; do
              west flash --no-rebuild -d "$build_dir" --domain "$domain" \
                "''${runner_args[@]}" "$@"
            done
          '';

          rdd2-flash-comms-stub = mkWestApp "rdd2-flash-comms-stub" ''
            ${commonScript}
            ${jlinkAccessScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_COMMS_STUB_BUILD_DIR:-$app/build-$board_slug-comms-stub}"
            runner="''${RDD2_FLASH_RUNNER:-jlink}"
            runner_args=()
            flash_domains=()

            if [ -n "$runner" ]; then
              runner_args=(--runner "$runner")
            fi

            if [ "$runner" = "jlink" ]; then
              rdd2_require_jlink_access
            fi

            rdd2_require_mcuboot_sysbuild "$build_dir"
            rdd2_require_comms_stub_configuration "$build_dir"

            mapfile -t flash_domains < <(
              sed -n '/^flash_order:/,$s/^  - //p' "$build_dir/domains.yaml"
            )
            if [ "''${#flash_domains[@]}" -ne 2 ] ||
              [ "''${flash_domains[0]}" != "mcuboot" ]; then
              printf 'error: refusing unexpected sysbuild flash order\n' >&2
              exit 1
            fi

            cd "$workspace"
            for domain in "''${flash_domains[@]}"; do
              west flash --no-rebuild -d "$build_dir" --domain "$domain" \
                "''${runner_args[@]}" "$@"
            done
          '';

          rdd2-flash-optical-flow = mkWestApp "rdd2-flash-optical-flow" ''
            ${commonScript}
            ${jlinkAccessScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_OPTICAL_FLOW_BUILD_DIR:-$app/build-$board_slug-optical-flow}"
            runner="''${RDD2_FLASH_RUNNER:-jlink}"
            runner_args=()
            flash_domains=()

            if [ -n "$runner" ]; then
              runner_args=(--runner "$runner")
            fi
            if [ "$runner" = "jlink" ]; then
              rdd2_require_jlink_access
            fi

            rdd2_require_mcuboot_sysbuild "$build_dir" "$board"
            rdd2_require_optical_flow_configuration "$build_dir"
            mapfile -t flash_domains < <(
              sed -n '/^flash_order:/,$s/^  - //p' "$build_dir/domains.yaml"
            )
            if [ "''${#flash_domains[@]}" -ne 2 ] ||
              [ "''${flash_domains[0]}" != "mcuboot" ]; then
              printf 'error: refusing unexpected sysbuild flash order\n' >&2
              exit 1
            fi

            cd "$workspace"
            for domain in "''${flash_domains[@]}"; do
              west flash --no-rebuild -d "$build_dir" --domain "$domain" \
                "''${runner_args[@]}" "$@"
            done
          '';

          # `west debug` attaches gdb to the running target through the J-Link
          # GDB server. `rdd2-debug attach` maps to `west attach`, which leaves
          # the firmware running instead of resetting and halting it -- the
          # difference matters when you are chasing something that only happens
          # after the vehicle has been up for a while.
          rdd2-debug = mkWestApp "rdd2-debug" ''
            ${commonScript}
            ${jlinkAccessScript}

            mode="debug"
            case "''${1:-}" in
            attach)
              mode="attach"
              shift
              ;;
            server)
              mode="debugserver"
              shift
              ;;
            debug)
              shift
              ;;
            esac

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_BUILD_DIR:-$app/build-$board_slug}"
            runner="''${RDD2_DEBUG_RUNNER:-jlink}"
            runner_args=()

            if [ ! -d "$build_dir" ]; then
              printf 'error: no build at %s\n' "$build_dir" >&2
              printf '       build the firmware with rdd2-build first\n' >&2
              exit 1
            fi

            if [ -n "$runner" ]; then
              runner_args=(--runner "$runner")
            fi

            if [ "$runner" = "jlink" ]; then
              rdd2_require_jlink_access
            fi

            cd "$workspace"
            exec west "$mode" -d "$build_dir" "''${runner_args[@]}" "$@"
          '';

          rdd2-menuconfig = mkWestApp "rdd2-menuconfig" ''
            ${commonScript}
            ${flightCompilerScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_BUILD_DIR:-$app/build-$board_slug}"

            cd "$workspace"
            exec west build -b "$board" -d "$build_dir" -t menuconfig "$app" "$@"
          '';

          rdd2-west-update = mkWestUpdateApp "rdd2-west-update" ''
            ${workspaceScript}

            app="$(rdd2_find_app)"
            workspace="$(rdd2_workspace "$app")"
            source_workspace="$(rdd2_source_workspace "$app")"
            expected_manifest="$(realpath "$app/west.yml")"

            # The group filter is a property of the invocation, never of the
            # workspace. Write it every time, and delete it when none is
            # selected, so a workspace previously updated with a filter cannot
            # keep omitting projects that the current invocation needs.
            rdd2_apply_group_filter() {
              if [ -n "''${RDD2_WEST_GROUP_FILTER:-}" ]; then
                # `--` is required: a filter that disables a group starts with
                # `-`, which west's argument parser would otherwise read as an
                # option.
                west config -- manifest.group-filter "$RDD2_WEST_GROUP_FILTER"
              else
                west config -d manifest.group-filter 2>/dev/null || true
              fi
            }

            if [ "$workspace" != "$source_workspace" ]; then
              printf 'using managed cerebri_rdd2 west workspace: %s\n' "$workspace" >&2
              export WEST_TOPDIR="$workspace"
              rdd2_prepare_managed_workspace "$app" "$workspace"
              cd "$workspace"
              rdd2_apply_group_filter
              exec west update "$@"
            fi

            if [ ! -d "$source_workspace/.west" ]; then
              cd "$source_workspace"
              west init -l "$app"
            else
              actual_manifest="$(rdd2_active_manifest "$source_workspace")"
              if [ -z "$actual_manifest" ]; then
                printf 'error: could not read the active west manifest in %s\n' "$source_workspace" >&2
                exit 1
              fi
              actual_manifest="$(realpath "$actual_manifest")"
              if [ "$actual_manifest" != "$expected_manifest" ]; then
                printf 'error: refusing to update source workspace with foreign manifest %s\n' "$actual_manifest" >&2
                printf '       expected %s\n' "$expected_manifest" >&2
                printf 'set RDD2_WEST_WORKSPACE to select another isolated workspace\n' >&2
                exit 1
              fi
            fi

            cd "$source_workspace"
            rdd2_apply_group_filter
            exec west update "$@"
          '';

          rdd2-trajectory-compare = mkWestApp "rdd2-trajectory-compare" ''
            ${commonScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            reference_label="''${RDD2_TRAJECTORY_REFERENCE_LABEL:-modelica}"
            reference="''${RDD2_TRAJECTORY_REFERENCE:-$RDD2_MODELICA_MODELS_ROOT/artifacts/vehicles/rdd2/mission-trajectory.csv}"
            sil="''${RDD2_TRAJECTORY_SIL:-$app/artifacts/sil/mission-trajectory.csv}"
            bil="''${RDD2_TRAJECTORY_BIL:-$app/artifacts/bil/work/mission-trajectory.csv}"
            output="''${RDD2_TRAJECTORY_OUTPUT:-$app/artifacts/trajectory-comparison}"

            exec "${pythonEnv}/bin/python" \
              "$RDD2_MODELICA_MODELS_ROOT/tools/trajectory_compare.py" \
              --reference "$reference_label=$reference" \
              --candidate "sil=$sil" \
              --candidate "bil=$bil" \
              --output "$output" \
              --duration-delta-max-s "''${RDD2_TRAJECTORY_DURATION_DELTA_MAX_S:-0.03}" \
              --position-rmse-max-m "''${RDD2_TRAJECTORY_POSITION_RMSE_MAX_M:-3.75}" \
              --position-p95-max-m "''${RDD2_TRAJECTORY_POSITION_P95_MAX_M:-10.0}" \
              --altitude-rmse-max-m "''${RDD2_TRAJECTORY_ALTITUDE_RMSE_MAX_M:-0.02}" \
              --attitude-p95-max-deg "''${RDD2_TRAJECTORY_ATTITUDE_P95_MAX_DEG:-5.0}" \
              "$@"
          '';

          rdd2-fastdyn-ci = mkCargoApp "rdd2-fastdyn-ci" ''
            ${commonScript}

            # Default to this repository's pinned providers, which is what an
            # acceptance run must use, but honour a caller that named one. A
            # developer iterating on models has to be able to reach this path;
            # forcing the pin unconditionally made the workspace's central
            # workflow unreachable through the only BIL entry point.
            #
            # The two cases are not equivalent and the run says which it is, so
            # an edited-input pass is never mistaken for a qualifying one.
            if [ -n "''${RDD2_MODELICA_MODELS_ROOT:-}" ] ||
               [ -n "''${RDD2_RUMOCA_EXECUTABLE:-}" ]; then
              printf '[deps] NON-QUALIFYING run: caller-selected providers\n' >&2
            else
              printf '[deps] qualifying run: repository-pinned providers\n' >&2
            fi
            app="$(rdd2_find_app)"
            rdd2_ensure_workspace "$app" "${rdd2-west-update}/bin/rdd2-west-update"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            ${flightCompilerScript}
            rdd2_require_pinned_modelica_checkout
            rdd2_build_fastdyn_firmware "$app"

            if ! rdd2_fastdyn_runtime_ready; then
              printf '[deps] preparing the pinned FastDyn runtime\n'
              "${rdd2-fastdyn-setup}/bin/rdd2-fastdyn-setup"
              rdd2_export_common "$app"
              rdd2_require_fastdyn_runtime
            fi

            rdd2_prepare_fmi_plant

            cd "$app"
            cargo build --release --locked --package cerebri-rdd2-xtask
            exec "$app/target/release/xtask" fastdyn-ci "$@"
          '';

          rdd2-fastdyn-setup = mkCargoApp "rdd2-fastdyn-setup" ''
            ${commonScript}

            if [ "$#" -ne 0 ]; then
              printf 'usage: rdd2-fastdyn-setup\n' >&2
              exit 2
            fi

            app="$(rdd2_find_app)"
            rdd2_ensure_workspace "$app" "${rdd2-west-update}/bin/rdd2-west-update"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"

            if [ ! -f "$FASTDYN_ROOT/setup.sh" ]; then
              printf 'error: missing pinned FastDyn checkout at %s\n' "$FASTDYN_ROOT" >&2
              printf '       run: rdd2-west-update\n' >&2
              exit 1
            fi

            mkdir -p "$FASTDYN_STATE"
            if [ -d "$FASTDYN_STATE/venv" ] &&
               ! "$FASTDYN_STATE/venv/bin/python" -c 'import sys' >/dev/null 2>&1; then
              printf 'Removing invalid generated FastDyn virtualenv at %s\n' \
                "$FASTDYN_STATE/venv"
              chmod -R u+w "$FASTDYN_STATE/venv" 2>/dev/null || true
              rm -rf -- "$FASTDYN_STATE/venv"
            fi
            cjson_prefix="$FASTDYN_ROOT/out/deps/cjson/install"
            export PKG_CONFIG_PATH="${
              lib.makeSearchPathOutput "dev" "lib/pkgconfig" (mkFastDynCiLibraries pkgs)
            }:$cjson_prefix/lib/pkgconfig:$cjson_prefix/lib64/pkgconfig''${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
            nix_library_path="${lib.makeLibraryPath (mkFastDynCiLibraries pkgs)}"
            export LIBRARY_PATH="$nix_library_path''${LIBRARY_PATH:+:$LIBRARY_PATH}"
            export LD_LIBRARY_PATH="$nix_library_path:$cjson_prefix/lib:$cjson_prefix/lib64''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

            "$FASTDYN_ROOT/setup.sh" \
              --venv "$FASTDYN_STATE/venv" \
              --python "${pythonEnv}/bin/python" \
              --build-qemu \
              --qemu-root "$FASTDYN_STATE/qemu" \
              --with-rumoca \
              --skip-optifuzz

            export PATH="$FASTDYN_STATE/venv/bin:$PATH"

            make -C "$FASTDYN_ROOT" \
              qemu_path="$FASTDYN_STATE/qemu" \
              DEV=true PHY=true FLIGHT_CONTROLLERS=true FMU=true
            cmake \
              -S "$FASTDYN_ROOT/boardrunner/boardrunner_sdk" \
              -B "$FASTDYN_ROOT/boardrunner/boardrunner_sdk/build" \
              -DFASTDYN_INCLUDE_DIR="$FASTDYN_ROOT/include" \
              -DQEMU_INCLUDE_DIR="$FASTDYN_STATE/qemu/include"
            cmake --build "$FASTDYN_ROOT/boardrunner/boardrunner_sdk/build" -j2

            printf '1:%s\n' "$(git -C "$FASTDYN_ROOT" rev-parse HEAD)" \
              >"$FASTDYN_STATE/runtime.revision"

            printf 'FastDyn runtime is ready at %s\n' "$FASTDYN_ROOT"
          '';

          rdd2-fastdyn-mission = mkCargoApp "rdd2-fastdyn-mission" ''
            ${commonScript}

            app="$(rdd2_find_app)"
            rdd2_ensure_workspace "$app" "${rdd2-west-update}/bin/rdd2-west-update"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            rdd2_require_pinned_modelica_checkout
            rdd2_build_fastdyn_firmware "$app"
            rdd2_prepare_fmi_plant
            export RDD2_FASTDYN_FIRMWARE_ELF="''${RDD2_FASTDYN_FIRMWARE_ELF:-$RDD2_FASTDYN_BUILD_DIR/zephyr/zephyr.elf}"

            cd "$app"
            cargo build --release --locked --package cerebri-rdd2-xtask
            exec "$app/target/release/xtask" fastdyn-mission "$@"
          '';

          rdd2-console = pkgs.writeShellApplication {
            name = "rdd2-console";
            runtimeInputs = [
              pkgs.coreutils
              pkgs.screen
            ];
            text = ''
              baud="''${RDD2_CONSOLE_BAUD:-115200}"
              device="''${RDD2_CONSOLE_DEVICE:-}"
              force_select=0
              state_dir="''${XDG_STATE_HOME:-$HOME/.local/state}/cerebri-rdd2"
              state_file="$state_dir/console-device"

              usage() {
                printf 'usage: rdd2-console [--select] [--device PATH] [--baud RATE]\n'
              }

              while [ "$#" -gt 0 ]; do
                case "$1" in
                  --select)
                    force_select=1
                    shift
                    ;;
                  --device)
                    [ "$#" -ge 2 ] || { usage >&2; exit 2; }
                    device="$2"
                    shift 2
                    ;;
                  --baud)
                    [ "$#" -ge 2 ] || { usage >&2; exit 2; }
                    baud="$2"
                    shift 2
                    ;;
                  -h|--help)
                    usage
                    exit 0
                    ;;
                  *)
                    printf 'error: unknown argument: %s\n' "$1" >&2
                    usage >&2
                    exit 2
                    ;;
                esac
              done

              if [ -z "$device" ] && [ "$force_select" -eq 0 ] && [ -r "$state_file" ]; then
                device="$(<"$state_file")"
                if [ ! -e "$device" ]; then
                  printf 'remembered serial device is disconnected: %s\n' "$device" >&2
                  device=""
                fi
              fi

              if [ -z "$device" ]; then
                shopt -s nullglob
                devices=(/dev/serial/by-id/*)

                if [ "''${#devices[@]}" -eq 0 ]; then
                  printf 'error: no stable serial devices found under /dev/serial/by-id\n' >&2
                  exit 1
                elif [ "''${#devices[@]}" -eq 1 ]; then
                  device="''${devices[0]}"
                else
                  if [ ! -t 0 ]; then
                    printf 'error: multiple serial devices found; rerun interactively or use --device PATH\n' >&2
                    exit 1
                  fi

                  printf 'Select the RDD2 serial device:\n'
                  for i in "''${!devices[@]}"; do
                    printf '  %d) %s -> %s\n' "$((i + 1))" "''${devices[$i]##*/}" "$(readlink -f "''${devices[$i]}")"
                  done

                  while true; do
                    read -r -p "Device [1-''${#devices[@]}]: " choice
                    if [[ "$choice" =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le "''${#devices[@]}" ]; then
                      device="''${devices[$((choice - 1))]}"
                      break
                    fi
                    printf 'Please enter a number from 1 to %d.\n' "''${#devices[@]}" >&2
                  done
                fi

                mkdir -p "$state_dir"
                printf '%s\n' "$device" > "$state_file"
                printf 'Remembering %s (change with rdd2-console --select).\n' "$device"
              fi

              if [ ! -r "$device" ] || [ ! -w "$device" ]; then
                printf 'error: serial device is not accessible: %s\n' "$device" >&2
                printf 'check dialout group membership and the NixOS udev setup in README.md\n' >&2
                exit 1
              fi

              exec screen "$device" "$baud"
            '';
          };

          rdd2-systemview =
            if system == "x86_64-linux" then
              mkWestApp "rdd2-systemview" ''
                ${commonScript}

                app="$(rdd2_find_app)"
                rdd2_export_common "$app"
                rdd2_require_workspace "$app"

                board="''${RDD2_BOARD:-mr_vmu_tropic}"
                board_slug="''${board//\//_}"
                build_dir="''${RDD2_BUILD_DIR:-$app/build-$board_slug}"
                elf="$build_dir/zephyr/zephyr.elf"

                if [ ! -r "$elf" ]; then
                  printf 'error: firmware ELF not found: %s\n' "$elf" >&2
                  printf 'build the firmware with rdd2-build first\n' >&2
                  exit 1
                fi

                rtt_addr="$(${zephyrSdk}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm "$elf" \
                  | sed -n 's/^\([0-9a-fA-F]\+\) [A-Za-z] _SEGGER_RTT$/0x\1/p' \
                  | head -n 1)"
                if [ -z "$rtt_addr" ]; then
                  printf 'error: _SEGGER_RTT not found in %s\n' "$elf" >&2
                  printf 'rebuild with CONFIG_SEGGER_SYSTEMVIEW enabled\n' >&2
                  exit 1
                fi

                usb_args=(-usb)
                if [ -n "''${RDD2_JLINK_SERIAL:-}" ]; then
                  usb_args=(-usb "$RDD2_JLINK_SERIAL")
                fi

                printf 'Starting SystemView for MIMXRT1064 via SWD; RTT control block %s\n' "$rtt_addr"
                exec "${systemView}/bin/SystemView" \
                  -single \
                  -recorder J-Link \
                  -device MIMXRT1064 \
                  "''${usb_args[@]}" \
                  -if SWD \
                  -speed "''${RDD2_JLINK_SPEED_KHZ:-4000}" \
                  -rttcbaddr "$rtt_addr" \
                  -start \
                  "$@"
              ''
            else
              pkgs.writeShellApplication {
                name = "rdd2-systemview";
                text = ''
                  printf 'error: SEGGER SystemView is supported by this flake only on x86_64-linux\n' >&2
                  exit 1
                '';
              };

          rdd2-systemview-capture = pkgs.writeShellApplication {
            name = "rdd2-systemview-capture";
            runtimeInputs = [ pkgs.coreutils ];
            text = ''
              trace_root="''${RDD2_TRACE_DIR:-$PWD/traces}"

              mkdir -p "$trace_root"
              trace_root="$(realpath "$trace_root")"
              capture_dir="$trace_root/$(date +%Y%m%d-%H%M%S)"
              if [ -e "$capture_dir" ]; then
                capture_dir="$capture_dir-$$"
              fi
              mkdir -p "$capture_dir"

              printf 'Output directory: %s\n' "$capture_dir"
              printf 'Accept the SFL dialog and wait for the SystemView window to finish opening.\n'

              ${rdd2-systemview}/bin/rdd2-systemview &
              systemview_pid=$!

              printf 'Press Enter to start recording...'
              IFS= read -r _
              if ! kill -0 "$systemview_pid" 2>/dev/null; then
                printf 'error: SystemView exited before recording could start\n' >&2
                wait "$systemview_pid" || true
                exit 1
              fi

              "${systemView}/bin/SystemView" -single -start
              printf 'Recording. Exercise the system now.\n'
              printf 'Press Enter to stop, export, and close SystemView...'
              IFS= read -r _

              "${systemView}/bin/SystemView" \
                -single \
                -stop \
                -save "$capture_dir/rdd2.SVDat" \
                -export "$capture_dir/rdd2-events.csv" \
                -export-contexts "$capture_dir/rdd2-contexts.csv" \
                -quit

              wait "$systemview_pid"
              printf 'SystemView trace saved to %s\n' "$capture_dir"
            '';
          };

          host-tools = pkgs.buildEnv {
            name = "cerebri-rdd2-host-tools";
            paths = baseTools ++ [
              rdd2-build
              rdd2-build-comms-stub
              rdd2-build-optical-flow
              rdd2-build-native-sim
              rdd2-test-gps-lockstep
              rdd2-flash
              rdd2-flash-comms-stub
              rdd2-flash-optical-flow
              rdd2-debug
              rdd2-menuconfig
              rdd2-console
              rdd2-systemview
              rdd2-systemview-capture
              rdd2-west-update
              rdd2-trajectory-compare
              rdd2-fastdyn-ci
              rdd2-fastdyn-setup
              rdd2-fastdyn-mission
            ];
          };
        in
        {
          zephyr-sdk = zephyrSdk;

          inherit
            host-tools
            rdd2-build
            rdd2-build-comms-stub
            rdd2-build-optical-flow
            rdd2-build-native-sim
            rdd2-test-gps-lockstep
            rdd2-flash
            rdd2-flash-comms-stub
            rdd2-flash-optical-flow
            rdd2-debug
            rdd2-menuconfig
            rdd2-console
            rdd2-systemview
            rdd2-systemview-capture
            rdd2-west-update
            rdd2-trajectory-compare
            rdd2-fastdyn-ci
            rdd2-fastdyn-setup
            rdd2-fastdyn-mission
            ;

          default = host-tools;
        }
      );

      apps = forAllSystems (
        system:
        let
          packages = self.packages.${system};
        in
        {
          build = {
            type = "app";
            program = "${packages.rdd2-build}/bin/rdd2-build";
            meta.description = "Build RDD2 firmware for mr_vmu_tropic";
          };

          build-comms-stub = {
            type = "app";
            program = "${packages.rdd2-build-comms-stub}/bin/rdd2-build-comms-stub";
            meta.description = "Build the non-flyable RDD2 communications bench firmware";
          };

          build-optical-flow = {
            type = "app";
            program = "${packages.rdd2-build-optical-flow}/bin/rdd2-build-optical-flow";
            meta.description = "Build RDD2 GPS plus optical-flow flight firmware";
          };

          build-native-sim = {
            type = "app";
            program = "${packages.rdd2-build-native-sim}/bin/rdd2-build-native-sim";
            meta.description = "Build RDD2 lockstep firmware for native_sim/native/64";
          };

          test-gps-lockstep = {
            type = "app";
            program = "${packages.rdd2-test-gps-lockstep}/bin/rdd2-test-gps-lockstep";
            meta.description = "Build native RDD2 GPS firmware and run its mandatory ingress lifecycle test";
          };

          flash = {
            type = "app";
            program = "${packages.rdd2-flash}/bin/rdd2-flash";
            meta.description = "Flash the RDD2 firmware build";
          };

          flash-comms-stub = {
            type = "app";
            program = "${packages.rdd2-flash-comms-stub}/bin/rdd2-flash-comms-stub";
            meta.description = "Flash the non-flyable RDD2 communications bench firmware";
          };

          flash-optical-flow = {
            type = "app";
            program = "${packages.rdd2-flash-optical-flow}/bin/rdd2-flash-optical-flow";
            meta.description = "Flash the RDD2 GPS plus optical-flow firmware build";
          };

          debug = {
            type = "app";
            program = "${packages.rdd2-debug}/bin/rdd2-debug";
            meta.description = "Attach gdb to the RDD2 firmware over J-Link";
          };

          menuconfig = {
            type = "app";
            program = "${packages.rdd2-menuconfig}/bin/rdd2-menuconfig";
            meta.description = "Run Zephyr menuconfig for RDD2";
          };

          console = {
            type = "app";
            program = "${packages.rdd2-console}/bin/rdd2-console";
            meta.description = "Open a remembered RDD2 serial console at 115200 baud";
          };

          systemview = {
            type = "app";
            program = "${packages.rdd2-systemview}/bin/rdd2-systemview";
            meta.description = "Start SEGGER SystemView for the RDD2 firmware";
          };

          systemview-capture = {
            type = "app";
            program = "${packages.rdd2-systemview-capture}/bin/rdd2-systemview-capture";
            meta.description = "Record and export an RDD2 SystemView trace";
          };

          west-update = {
            type = "app";
            program = "${packages.rdd2-west-update}/bin/rdd2-west-update";
            meta.description = "Initialize or update the RDD2 west workspace";
          };

          trajectory-compare = {
            type = "app";
            program = "${packages.rdd2-trajectory-compare}/bin/rdd2-trajectory-compare";
            meta.description = "Compare RDD2 mission trajectory logs and render overlays";
          };

          fastdyn-ci = {
            type = "app";
            program = "${packages.rdd2-fastdyn-ci}/bin/rdd2-fastdyn-ci";
            meta.description = "Run and validate the complete RDD2 FastDyn mission";
          };

          fastdyn-setup = {
            type = "app";
            program = "${packages.rdd2-fastdyn-setup}/bin/rdd2-fastdyn-setup";
            meta.description = "Prepare the pinned FastDyn and patched QEMU runtime";
          };

          fastdyn-mission = {
            type = "app";
            program = "${packages.rdd2-fastdyn-mission}/bin/rdd2-fastdyn-mission";
            meta.description = "Run the RDD2 FMI plant and firmware lockstep mission";
          };
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          pythonEnv = mkPythonEnv pkgs;
          packages = self.packages.${system};
          fastDynCiTools = mkFastDynCiTools pkgs;
          fastDynCiLibraries = mkFastDynCiLibraries pkgs;
        in
        {
          default = pkgs.mkShell {
            hardeningDisable = [
              "fortify"
              "fortify3"
            ];
            nativeBuildInputs = [
              packages.host-tools
              pkgs.clang-tools
            ]
            ++ fastDynCiTools;
            buildInputs = fastDynCiLibraries;

            shellHook = ''
              export WEST_PYTHON="''${WEST_PYTHON:-${pythonEnv}/bin/python}"
              export ZEPHYR_SDK_INSTALL_DIR="''${ZEPHYR_SDK_INSTALL_DIR:-${packages.zephyr-sdk}}"
              export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"
              export CMAKE_POLICY_VERSION_MINIMUM="''${CMAKE_POLICY_VERSION_MINIMUM:-3.5}"
              export LD_LIBRARY_PATH="${packages.host-tools}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

              rdd2_shell_find_app() {
                local dir
                dir="$(pwd -P)"

                while [ "$dir" != "/" ]; do
                  if [ -f "$dir/west.yml" ] && [ -f "$dir/prj.conf" ] && [ -f "$dir/CMakeLists.txt" ]; then
                    printf '%s\n' "$dir"
                    return 0
                  fi

                  if [ -f "$dir/cerebri_rdd2/west.yml" ] && [ -f "$dir/cerebri_rdd2/prj.conf" ]; then
                    printf '%s\n' "$dir/cerebri_rdd2"
                    return 0
                  fi

                  dir="$(dirname "$dir")"
                done

                return 1
              }

              if app="$(rdd2_shell_find_app 2>/dev/null)"; then
                source_workspace="$(dirname "$app")"
                expected_manifest="$(realpath "$app/west.yml")"
                actual_manifest="$(west manifest --path 2>/dev/null || true)"

                if [ -n "$actual_manifest" ]; then
                  actual_manifest="$(realpath "$actual_manifest")"
                fi

                if [ "$actual_manifest" = "$expected_manifest" ]; then
                  workspace="$source_workspace"
                elif [ -n "''${RDD2_WEST_WORKSPACE:-}" ]; then
                  workspace="$(realpath -m "$RDD2_WEST_WORKSPACE")"
                else
                  workspace="$app/.devenv/state/west"
                fi

                export RDD2_WORKSPACE_ROOT="$workspace"
                export RDD2_CEREBRI_MODULES_ROOT="''${RDD2_CEREBRI_MODULES_ROOT:-$workspace/modules/lib/cerebri_lockstep}"
                export RDD2_ZROS_ROOT="''${RDD2_ZROS_ROOT:-$workspace/modules/lib/zros}"
                export RDD2_CSYN_ROOT="''${RDD2_CSYN_ROOT:-$workspace/modules/lib/csyn}"
                export RDD2_MODELICA_MODELS_ROOT="''${RDD2_MODELICA_MODELS_ROOT:-$workspace/models/modelica_models}"
                export WEST_TOPDIR="$workspace"
                export FASTDYN_ROOT="''${FASTDYN_ROOT:-$workspace/modules/sim/fastdyn}"
                export FASTDYN_STATE="''${FASTDYN_STATE:-$app/.devenv/state/fastdyn}"
                export FASTDYN_EXECUTABLE="''${FASTDYN_EXECUTABLE:-$FASTDYN_STATE/venv/bin/fastdyn}"
                export FASTDYN_QEMU_PATH="''${FASTDYN_QEMU_PATH:-$FASTDYN_STATE/qemu/build/qemu-system-arm}"
                export FASTDYN_MONITOR_ELF="''${FASTDYN_MONITOR_ELF:-$FASTDYN_STATE/qemu/ws/monitor.elf}"
                # Default only, so a raw `west build` in this shell keeps a
                # compiler the caller selected instead of silently reverting
                # to the pinned one.
                if [ -z "''${RDD2_RUMOCA_EXECUTABLE:-}" ]; then
                  export RDD2_RUMOCA_EXECUTABLE="${rumoca.packages.${system}.default}/bin/rumoca"
                  pinned_rumoca_sha256="$(${pkgs.coreutils}/bin/sha256sum \
                    ${rumoca.packages.${system}.default}/bin/rumoca)"
                  pinned_rumoca_sha256="''${pinned_rumoca_sha256%% *}"
                  export RDD2_RUMOCA_EXECUTABLE_SHA256="$pinned_rumoca_sha256"
                fi
                if [ -d "$workspace/zephyr" ]; then
                  export ZEPHYR_BASE="$workspace/zephyr"
                elif [ -z "''${ZEPHYR_BASE:-}" ]; then
                  printf 'cerebri_rdd2 Nix shell: run rdd2-west-update before raw west builds\n' >&2
                fi

                # Editors need one stable path, while West keeps compilation
                # databases inside board-specific build directories. Prefer an
                # explicit selection; otherwise use native_sim because clangd
                # can parse its host flags directly. The database still has
                # every generated Zephyr, module, and eFMU include directory.
                compile_db="''${RDD2_COMPILE_COMMANDS:-}"
                if [ -z "$compile_db" ] && [ -n "''${RDD2_BUILD_DIR:-}" ]; then
                  compile_db="$RDD2_BUILD_DIR/compile_commands.json"
                fi
                if [ -z "$compile_db" ]; then
                  compile_db="$app/build-native_sim/compile_commands.json"
                fi
                ln -sfn "$(realpath -m "$compile_db")" "$app/compile_commands.json"
                export RDD2_COMPILE_COMMANDS="$app/compile_commands.json"
              elif [ -z "''${ZEPHYR_BASE:-}" ] && [ -d "$PWD/zephyr" ]; then
                export ZEPHYR_BASE="$PWD/zephyr"
              fi

              echo "cerebri_rdd2 Nix shell: clangd + Zephyr compile_commands configured"
              echo "cerebri_rdd2 Nix shell: rdd2-west-update, rdd2-build, rdd2-build-optical-flow, rdd2-build-native-sim, rdd2-test-gps-lockstep, rdd2-flash, rdd2-flash-optical-flow, rdd2-flash-comms-stub, rdd2-debug, rdd2-console, rdd2-systemview, rdd2-systemview-capture"
              echo "cerebri_rdd2 Nix shell: rdd2-fastdyn-setup, rdd2-fastdyn-ci, rdd2-fastdyn-mission"
            '';
          };
        }
      );

      nixosModules.default = import ./nix/nixos-module.nix { inherit self; };
      nixosModules.cerebri-rdd2 = self.nixosModules.default;
    };
}
