# SPDX-License-Identifier: Apache-2.0

# Exact Rumoca release used to generate deployable eFMI controller code.
# Keep the installer and per-platform binary hashes in the same lock so a
# version change cannot silently select different code-generation artifacts.
set(RDD2_RUMOCA_VERSION "v0.9.20")
set(RDD2_RUMOCA_INSTALL_SCRIPT_URL
  "https://raw.githubusercontent.com/CogniPilot/rumoca/${RDD2_RUMOCA_VERSION}/infra/install/install.sh"
)
set(RDD2_RUMOCA_INSTALL_SCRIPT_SHA256
  "ac15aa5ec90cb974387610fae7594d2704f2a3bd2e080cfe2517e896d30eb161"
)

set(RDD2_RUMOCA_LINUX_X86_64_SHA256
  "12727925c304c5188651e8b6b90f45071c8ad318e76b163c7adee160c649ba1a"
)
set(RDD2_RUMOCA_LINUX_AARCH64_SHA256
  "5a4008d0320b264d2bf180d83dd1d9dc9616719935b94c4910d1b75334a459a8"
)
set(RDD2_RUMOCA_DARWIN_X86_64_SHA256
  "274121445c4964de99fb12468e69c17aec4e7fb4006bd26b1043540997f3403f"
)
set(RDD2_RUMOCA_DARWIN_AARCH64_SHA256
  "11431617a6d57f655fb5a300e8e863e1a4cf6f99f534bde726da910b17174429"
)
