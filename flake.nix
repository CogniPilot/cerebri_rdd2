{
  description = "Nix development and host support for the cerebri_rdd2 Zephyr app";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = lib.genAttrs supportedSystems;
      pkgsFor = system: import nixpkgs { inherit system; };
      mkPythonEnv =
        pkgs:
        pkgs.python3.withPackages (
          ps: with ps; [
            anytree
            intelhex
            jinja2
            jsonschema
            packaging
            pyelftools
            pykwalify
            pyserial
            pyyaml
            requests
            semver
            tqdm
            west
          ]
        );
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          pythonEnv = mkPythonEnv pkgs;
          hostCc = if system == "x86_64-linux" then pkgs.gcc_multi else pkgs.stdenv.cc;
          hostMultilibTools = lib.optionals (system == "x86_64-linux") [
            pkgs.glibc_multi.dev
          ];

          baseTools = [
            pkgs.ccache
            pkgs.cmake
            pkgs.coreutils
            pkgs.curl
            pkgs.dtc
            pkgs.file
            pkgs.findutils
            pkgs.gcc-arm-embedded
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
            hostCc
            pkgs.unzip
            pkgs.which
            pkgs.xz
            pkgs.zip
            pythonEnv
          ] ++ hostMultilibTools;

          commonScript = ''
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
              local cache_root
              local key

              if [ -n "''${RDD2_WEST_WORKSPACE:-}" ]; then
                realpath -m "$RDD2_WEST_WORKSPACE"
                return 0
              fi

              cache_root="''${XDG_CACHE_HOME:-$HOME/.cache}/cerebri-rdd2"
              key="$(printf '%s' "$app" | sha256sum | cut -c1-16)"
              printf '%s\n' "$cache_root/west-$key"
            }

            rdd2_workspace() {
              local app="$1"
              local source_workspace
              local expected_manifest
              local actual_manifest

              if [ -n "''${RDD2_WORKSPACE_ROOT:-}" ]; then
                realpath -m "$RDD2_WORKSPACE_ROOT"
                return 0
              fi

              source_workspace="$(rdd2_source_workspace "$app")"
              expected_manifest="$(realpath "$app/west.yml")"
              actual_manifest="$(rdd2_active_manifest "$app")"

              if [ -n "$actual_manifest" ]; then
                actual_manifest="$(realpath "$actual_manifest")"
              fi

              if [ -z "$actual_manifest" ] ||
                 [ "$actual_manifest" = "$expected_manifest" ] ||
                 [ "''${RDD2_ALLOW_FOREIGN_WEST:-0}" = "1" ]; then
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
                      commit -q -m 'Update cerebri_rdd2 manifest'
                fi
              )

              if [ ! -d "$workspace/.west" ]; then
                (cd "$workspace" && west init -l manifest)
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

            rdd2_require_module_paths() {
              local workspace="$1"
              local missing=0
              local path

              for path in \
                zephyr \
                modules/hal/cmsis \
                modules/hal/cmsis_6 \
                modules/hal/nxp \
                modules/fs/fatfs \
                modules/lib/cmsis-dsp \
                modules/lib/zenoh-pico \
                modules/lib/zros \
                modules/lib/zephyr_boards
              do
                if [ ! -d "$workspace/$path" ]; then
                  printf 'error: missing required west checkout: %s/%s\n' "$workspace" "$path" >&2
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
              export GNUARMEMB_TOOLCHAIN_PATH="''${GNUARMEMB_TOOLCHAIN_PATH:-${pkgs.gcc-arm-embedded}}"
              export RDD2_WORKSPACE_ROOT="$workspace"

              if [ -d "$workspace/zephyr" ]; then
                export ZEPHYR_BASE="$workspace/zephyr"
              fi
            }

            rdd2_require_workspace() {
              local app="$1"
              local workspace
              local source_workspace
              local expected_manifest
              local actual_manifest
              workspace="$(rdd2_workspace "$app")"
              source_workspace="$(rdd2_source_workspace "$app")"
              expected_manifest="$(realpath "$app/west.yml")"

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

              if [ "$workspace" = "$source_workspace" ] &&
                 [ "$actual_manifest" != "$expected_manifest" ] &&
                 [ "''${RDD2_ALLOW_FOREIGN_WEST:-0}" != "1" ]; then
                printf 'error: active west manifest is %s\n' "$actual_manifest" >&2
                printf '       cerebri_rdd2 expects %s\n' "$expected_manifest" >&2
                printf 'run nix run .#west-update to create/update the managed cerebri_rdd2 workspace\n' >&2
                return 1
              fi

              if [ -z "''${ZEPHYR_BASE:-}" ] || [ ! -d "$ZEPHYR_BASE" ]; then
                printf 'error: missing Zephyr checkout; expected %s/zephyr or ZEPHYR_BASE\n' "$workspace" >&2
                printf 'run: nix run .#west-update\n' >&2
                return 1
              fi

              rdd2_require_module_paths "$workspace"
            }
          '';

          mkWestApp =
            name: text:
            pkgs.writeShellApplication {
              inherit name;
              runtimeInputs = baseTools;
              inherit text;
            };

          rdd2-build = mkWestApp "rdd2-build" ''
            ${commonScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-gnuarmemb}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_BUILD_DIR:-$app/build-$board_slug}"

            cd "$workspace"
            exec west build -b "$board" -d "$build_dir" "$app" "$@"
          '';

          rdd2-build-native-sim = mkWestApp "rdd2-build-native-sim" ''
            ${commonScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-host}"

            board="''${RDD2_NATIVE_SIM_BOARD:-native_sim/native/64}"
            build_dir="''${RDD2_NATIVE_SIM_BUILD_DIR:-$app/build-native_sim}"

            cd "$workspace"
            exec west build -b "$board" -d "$build_dir" "$app" "$@"
          '';

          rdd2-flash = mkWestApp "rdd2-flash" ''
            ${commonScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-gnuarmemb}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_BUILD_DIR:-$app/build-$board_slug}"
            runner="''${RDD2_FLASH_RUNNER:-pyocd}"
            runner_args=()

            if [ -n "$runner" ]; then
              runner_args=(--runner "$runner")
            fi

            cd "$workspace"
            exec west flash -d "$build_dir" "''${runner_args[@]}" "$@"
          '';

          rdd2-menuconfig = mkWestApp "rdd2-menuconfig" ''
            ${commonScript}

            app="$(rdd2_find_app)"
            rdd2_export_common "$app"
            rdd2_require_workspace "$app"
            workspace="$RDD2_WORKSPACE_ROOT"

            export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-gnuarmemb}"

            board="''${RDD2_BOARD:-mr_vmu_tropic}"
            board_slug="''${board//\//_}"
            build_dir="''${RDD2_BUILD_DIR:-$app/build-$board_slug}"

            cd "$workspace"
            exec west build -b "$board" -d "$build_dir" -t menuconfig "$app" "$@"
          '';

          rdd2-west-update = mkWestApp "rdd2-west-update" ''
            ${commonScript}

            app="$(rdd2_find_app)"
            workspace="$(rdd2_workspace "$app")"
            source_workspace="$(rdd2_source_workspace "$app")"
            expected_manifest="$(realpath "$app/west.yml")"

            if [ "$workspace" != "$source_workspace" ]; then
              printf 'using managed cerebri_rdd2 west workspace: %s\n' "$workspace" >&2
              rdd2_prepare_managed_workspace "$app" "$workspace"
              cd "$workspace"
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
                printf 'unset RDD2_WORKSPACE_ROOT/RDD2_ALLOW_FOREIGN_WEST to use the managed workspace fallback\n' >&2
                exit 1
              fi
            fi

            cd "$source_workspace"
            exec west update "$@"
          '';

          host-tools = pkgs.buildEnv {
            name = "cerebri-rdd2-host-tools";
            paths = baseTools ++ [
              rdd2-build
              rdd2-build-native-sim
              rdd2-flash
              rdd2-menuconfig
              rdd2-west-update
            ];
          };
        in
        {
          inherit
            host-tools
            rdd2-build
            rdd2-build-native-sim
            rdd2-flash
            rdd2-menuconfig
            rdd2-west-update
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

          build-native-sim = {
            type = "app";
            program = "${packages.rdd2-build-native-sim}/bin/rdd2-build-native-sim";
            meta.description = "Build RDD2 SITL for native_sim/native/64";
          };

          flash = {
            type = "app";
            program = "${packages.rdd2-flash}/bin/rdd2-flash";
            meta.description = "Flash the RDD2 firmware build";
          };

          menuconfig = {
            type = "app";
            program = "${packages.rdd2-menuconfig}/bin/rdd2-menuconfig";
            meta.description = "Run Zephyr menuconfig for RDD2";
          };

          west-update = {
            type = "app";
            program = "${packages.rdd2-west-update}/bin/rdd2-west-update";
            meta.description = "Initialize or update the RDD2 west workspace";
          };
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          pythonEnv = mkPythonEnv pkgs;
          packages = self.packages.${system};
        in
        {
          default = pkgs.mkShell {
            packages = [ packages.host-tools ];

            shellHook = ''
              export WEST_PYTHON="''${WEST_PYTHON:-${pythonEnv}/bin/python}"
              export GNUARMEMB_TOOLCHAIN_PATH="''${GNUARMEMB_TOOLCHAIN_PATH:-${pkgs.gcc-arm-embedded}}"
              export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-gnuarmemb}"

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

                if [ -n "''${RDD2_WORKSPACE_ROOT:-}" ]; then
                  workspace="$(realpath -m "$RDD2_WORKSPACE_ROOT")"
                elif [ -z "$actual_manifest" ] ||
                     [ "$actual_manifest" = "$expected_manifest" ] ||
                     [ "''${RDD2_ALLOW_FOREIGN_WEST:-0}" = "1" ]; then
                  workspace="$source_workspace"
                elif [ -n "''${RDD2_WEST_WORKSPACE:-}" ]; then
                  workspace="$(realpath -m "$RDD2_WEST_WORKSPACE")"
                else
                  key="$(printf '%s' "$app" | sha256sum | cut -c1-16)"
                  workspace="''${XDG_CACHE_HOME:-$HOME/.cache}/cerebri-rdd2/west-$key"
                fi

                export RDD2_WORKSPACE_ROOT="$workspace"
                if [ -d "$workspace/zephyr" ]; then
                  export ZEPHYR_BASE="$workspace/zephyr"
                elif [ -z "''${ZEPHYR_BASE:-}" ]; then
                  printf 'cerebri_rdd2 Nix shell: run rdd2-west-update before raw west builds\n' >&2
                fi
              elif [ -z "''${ZEPHYR_BASE:-}" ] && [ -d "$PWD/zephyr" ]; then
                export ZEPHYR_BASE="$PWD/zephyr"
              fi

              echo "cerebri_rdd2 Nix shell: rdd2-west-update, rdd2-build, rdd2-build-native-sim, rdd2-flash"
            '';
          };
        }
      );

      nixosModules.default = import ./nix/nixos-module.nix { inherit self; };
      nixosModules.cerebri-rdd2 = self.nixosModules.default;
    };
}
