#!/usr/bin/env bash
#
# Launch the UNMODIFIED MR-VMU-Tropic flight image under FastDyn/QEMU with the
# inertial rehosting path (LPSPI3 + ICM45686 + GPIO3). Attaches the LPUART6
# console to a host pty and points the ICM45686 model at a sample file.
#
# The image is the standard board build (nix run .#build), NOT a fastdyn/lockstep
# build. No arming, RC, motors or plant: the estimator gate only needs IMU plus
# the wire inputs.
#
# Required environment:
#   FASTDYN_ROOT          FastDyn checkout (has fastdyn-env, build/libfastdyn.so,
#                         boardrunner/boardrunner_sdk/build/*.so)
#   FASTDYN_QEMU_PATH     patched qemu-system-arm
#   RDD2_WORKSPACE_ROOT   West workspace root (firmware sources)
# Optional:
#   RDD2_FLIGHT_BUILD_DIR flight build dir (default build-mr_vmu_tropic-flight-bil)
#   BIL_ICM_SAMPLE_FILE   raw sample stream for the ICM45686 model (see below).
#                         If unset the model emits a static +1 g on sensor Z.
#   FASTDYN_LIBSTDCXX_DIR directory holding libstdc++.so.6 for the fastdyn venv
#   BIL_RUN_SECONDS       wall-clock run duration before shutdown (default 20)
#
# Sample file record layout (little-endian, 36 bytes, contiguous, no header),
# produced by tools/log_replay/wire_replay.py --imu-out:
#   uint64 t_ns; float ax ay az (m/s^2); float gx gy gz (rad/s); float temp (C)
# all in ICM45686 sensor axes, replayed at the ODR and looped at end of file.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${FASTDYN_ROOT:?set FASTDYN_ROOT to the FastDyn checkout}"
: "${FASTDYN_QEMU_PATH:?set FASTDYN_QEMU_PATH to the patched qemu-system-arm}"
: "${RDD2_WORKSPACE_ROOT:?set RDD2_WORKSPACE_ROOT to the West workspace root}"

flight_build="${RDD2_FLIGHT_BUILD_DIR:-$repo_root/build-mr_vmu_tropic-flight-bil}"
config="$repo_root/fastdyn/mr_vmu_tropic_flight.toml"
work_dir="${FASTDYN_RDD2_WORK_DIR:-$repo_root/artifacts/bil/flight-work}"
# `fastdyn run -o` deletes and recreates its output directory at startup, so the
# console log and the fastdyn work tree must not share a directory.
fastdyn_out_dir="$work_dir/fastdyn"
console_link="${BIL_CONSOLE_LINK:-$work_dir/lpuart6_console}"
run_seconds="${BIL_RUN_SECONDS:-20}"

if [ ! -f "$flight_build/zephyr/zephyr.elf" ]; then
	echo "flight image not found: $flight_build/zephyr/zephyr.elf" >&2
	echo "build it first, e.g.:" >&2
	echo "  RDD2_SYNAPSE_FBS_ROOT=... RDD2_BUILD_DIR=$flight_build nix run .#build -- -p always" >&2
	exit 1
fi

mkdir -p "$work_dir"

# libstdc++ for the venv numpy/lxml imports (nix cc runtime).
if [ -n "${FASTDYN_LIBSTDCXX_DIR:-}" ]; then
	export LD_LIBRARY_PATH="$FASTDYN_LIBSTDCXX_DIR:${LD_LIBRARY_PATH:-}"
fi

# LPUART6 is the Zephyr console/shell. The lpuart6 scroll opens /tmp/lpuart6_pty;
# bridge it to a readable console pty with socat.
socat_pid=""
if command -v socat >/dev/null 2>&1; then
	rm -f /tmp/lpuart6_pty "$console_link"
	socat pty,raw,echo=0,link=/tmp/lpuart6_pty pty,raw,echo=0,link="$console_link" &
	socat_pid=$!
	sleep 1
	echo "[bil] console pty: $console_link (guest side /tmp/lpuart6_pty)"
else
	echo "[bil] socat not found; console output will be dropped by the lpuart6 scroll" >&2
fi

# Capture the console to a log in the background.
if [ -n "$socat_pid" ] && [ -e "$console_link" ]; then
	( cat "$console_link" > "$work_dir/console.log" 2>/dev/null & echo $! > "$work_dir/console_cat.pid" )
fi

cleanup() {
	[ -n "${fastdyn_pid:-}" ] && kill "$fastdyn_pid" 2>/dev/null
	[ -f "$work_dir/console_cat.pid" ] && kill "$(cat "$work_dir/console_cat.pid")" 2>/dev/null
	[ -n "$socat_pid" ] && kill "$socat_pid" 2>/dev/null
}
trap cleanup EXIT INT TERM

export FASTDYN_INSTALL_ROOT="$FASTDYN_ROOT"
export FASTDYN_MONITOR_ELF="${FASTDYN_MONITOR_ELF:-$flight_build/zephyr/zephyr.elf}"
export RDD2_FASTDYN_BUILD_DIR="$flight_build"
export CEREBRI_RDD2_ROOT="$repo_root"
export FASTDYN_QEMU_MEMORY_DIR="$work_dir/memory"
mkdir -p "$fastdyn_out_dir"
export BIL_ICM_SAMPLE_FILE="${BIL_ICM_SAMPLE_FILE:-}"
mkdir -p "$FASTDYN_QEMU_MEMORY_DIR"

fastdyn_bin="${FASTDYN_EXECUTABLE:-$FASTDYN_ROOT/fastdyn-env/bin/fastdyn}"

echo "[bil] launching flight image: $flight_build/zephyr/zephyr.elf"
echo "[bil] config: $config"
echo "[bil] work dir: $work_dir"
cd "$FASTDYN_ROOT"
"$fastdyn_bin" run -c "$config" -o "$fastdyn_out_dir" --no-run-processes &
fastdyn_pid=$!

echo "[bil] running for ${run_seconds}s (console -> $work_dir/console.log)"
sleep "$run_seconds"
echo "[bil] done; last console lines:"
tail -40 "$work_dir/console.log" 2>/dev/null || true
