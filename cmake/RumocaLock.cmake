# SPDX-License-Identifier: Apache-2.0

# Exact Rumoca revision used to generate deployable eFMI controller code.
# The flake input `rumoca` in flake.nix pins the same revision and supplies
# RDD2_RUMOCA_EXECUTABLE to every nix build; this lock is what the build
# checks that executable against, and what the installer fallback would
# download when no executable is supplied.
#
# The revision is the v0.10.0 tag. The installer entries below stay empty
# until the v0.10.0 release publishes its install script and per-platform
# binaries; until then a build either supplies RDD2_RUMOCA_EXECUTABLE or
# lets InstallRumoca.cmake build RDD2_RUMOCA_SOURCE_REV with cargo. When the
# release assets exist, fill in the install script and binary hashes.
set(RDD2_RUMOCA_VERSION "v0.10.0")
set(RDD2_RUMOCA_REVISION "e0c76ac8c2d9865041853804be5d754dc17005bd")
set(RDD2_RUMOCA_INSTALL_SCRIPT_URL
  "https://raw.githubusercontent.com/CogniPilot/rumoca/${RDD2_RUMOCA_VERSION}/infra/install/install.sh"
)
set(RDD2_RUMOCA_INSTALL_SCRIPT_SHA256 "")

# Without RDD2_RUMOCA_EXECUTABLE the build installs this Rumoca source revision
# with cargo, the same revision the flake input pins. Clearing it selects the
# release installer and the per-platform hashes below instead.
set(RDD2_RUMOCA_SOURCE_REV "${RDD2_RUMOCA_REVISION}")

set(RDD2_RUMOCA_LINUX_X86_64_SHA256 "")
set(RDD2_RUMOCA_LINUX_AARCH64_SHA256 "")
set(RDD2_RUMOCA_DARWIN_X86_64_SHA256 "")
set(RDD2_RUMOCA_DARWIN_AARCH64_SHA256 "")
