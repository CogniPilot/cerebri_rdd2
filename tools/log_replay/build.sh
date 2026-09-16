#!/usr/bin/env sh
# Build the host replay of the RDD2 navigation estimator eFMU.
#
#   MODELICA_MODELS=~/git/modelica_models ./build.sh
#
# The estimator production code is taken from the modelica_models estimator
# export. The estimator wiring (IMU preintegration, GNSS and optical-flow
# adapters) and the interface topic structs are compiled directly from the
# repository sources under src/, against the synapse_fbs C headers, so the
# replay runs the exact code the firmware carries.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src=$repo/src
processes=$src/processes

models=${MODELICA_MODELS:-$HOME/git/modelica_models}
efmu=${EFMU_PRODUCTION_CODE:-$models/artifacts/vehicles/rdd2/estimator/Vehicles_Rdd2_NavigationEstimator/ProductionCode}

# The fixed-layout synapse topic structs the adapters consume come from the
# synapse_fbs C package. Resolve it from RDD2_SYNAPSE_FBS_ROOT (the variable the
# flake apps export), falling back to the devenv-provisioned package under the
# checkout. Both point at the package directory that contains include/synapse.
synapse=${RDD2_SYNAPSE_FBS_ROOT:-$repo/.devenv/state/synapse_fbs-build/synapse_fbs-c}
synapse_include=$synapse/include
if [ ! -f "$synapse_include/synapse/sensors_reader.h" ]; then
  echo "error: synapse_fbs C headers not found under $synapse_include" >&2
  echo "       set RDD2_SYNAPSE_FBS_ROOT to the synapse_fbs-c package directory" >&2
  echo "       (the one that contains include/synapse), or provision it with" >&2
  echo "       scripts/synapse_fbs_package." >&2
  exit 1
fi

if [ ! -f "$efmu/Vehicles_Rdd2_NavigationEstimator.c" ]; then
  echo "error: estimator production code not found under $efmu" >&2
  echo "       set MODELICA_MODELS or EFMU_PRODUCTION_CODE to the export that" >&2
  echo "       carries Vehicles_Rdd2_NavigationEstimator.c." >&2
  exit 1
fi

cc=${CC:-cc}
exec "$cc" -O2 -w \
  -I"$src" -I"$processes" -I"$synapse_include" -I"$efmu" \
  -o "$here/replay" \
  "$here/replay.c" \
  "$processes/imu_preintegration.c" \
  "$processes/navigation_gps.c" \
  "$processes/navigation_optical_flow.c" \
  "$efmu/Vehicles_Rdd2_NavigationEstimator.c" \
  "$efmu/rumoca_galec_kernels.c" -lm
