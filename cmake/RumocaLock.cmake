# SPDX-License-Identifier: Apache-2.0

# Exact Rumoca revision used to generate deployable eFMI controller code.
# The flake input `rumoca` in flake.nix pins the same revision and supplies
# RDD2_RUMOCA_EXECUTABLE to every nix build; this lock is what the build
# checks that executable against, and what the installer fallback would
# download when no executable is supplied.
#
# The installer entries below are empty because no Rumoca release carries
# this revision yet (the latest published release is v0.9.20). Until a
# v0.10.0 release exists, a build must supply RDD2_RUMOCA_EXECUTABLE; the
# installer path refuses with a message that says so. When the release is
# cut, fill in the install script and per-platform binary hashes here.
set(RDD2_RUMOCA_VERSION "v0.10.0")
set(RDD2_RUMOCA_REVISION "97eb3ab74b3e11264ab2000437eb47df1a57214d")
set(RDD2_RUMOCA_INSTALL_SCRIPT_URL
  "https://raw.githubusercontent.com/CogniPilot/rumoca/${RDD2_RUMOCA_VERSION}/infra/install/install.sh"
)
set(RDD2_RUMOCA_INSTALL_SCRIPT_SHA256 "")

set(RDD2_RUMOCA_LINUX_X86_64_SHA256 "")
set(RDD2_RUMOCA_LINUX_AARCH64_SHA256 "")
set(RDD2_RUMOCA_DARWIN_X86_64_SHA256 "")
set(RDD2_RUMOCA_DARWIN_AARCH64_SHA256 "")
