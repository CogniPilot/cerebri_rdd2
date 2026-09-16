# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flight_log_rotation)

set(FLIGHT_LOG_DIR "${CMAKE_CURRENT_LIST_DIR}/../../subsys/flight_log")

# The fake synapse, zros, and interface headers must shadow any that a module
# might contribute, so place them ahead of every other include directory.
target_include_directories(app BEFORE PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/fake_include"
)
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/src"
  "${FLIGHT_LOG_DIR}"
)

target_sources(app PRIVATE
  "${FLIGHT_LOG_DIR}/flight_log.c"
  src/fakes.c
  src/main.c
)

# The flight logger reads its tuning from Kconfig. The suite does not enable the
# CONFIG_RDD2_FLIGHT_LOG driver stack (it selects on-target SD hardware), so the
# tuning is supplied directly. The rotation and spare-step sizes are scaled down
# so several size-triggered rotations occur within the run while the ring stays
# at its real 131072-byte size.
target_compile_definitions(app PRIVATE
  CONFIG_RDD2_FLIGHT_LOG_LOG_LEVEL=1
  CONFIG_RDD2_FLIGHT_LOG_RING_BYTES=131072
  CONFIG_RDD2_FLIGHT_LOG_BLOCK_BYTES=4096
  CONFIG_RDD2_FLIGHT_LOG_ROTATE_BYTES=262144
  CONFIG_RDD2_FLIGHT_LOG_PREALLOC_STEP_BYTES=32768
  # Scale the reservation floor below the scaled rotation size so the inline
  # reservation path actually reserves (in the flight image the 256 MiB rotation
  # size sits far above the 4 MiB floor). The fake sink turns that reservation
  # into the injected multi-second card stall.
  FLIGHT_LOG_PREALLOC_FLOOR_BYTES=65536
  CONFIG_RDD2_FLIGHT_LOG_TIMEREF_PERIOD_MS=100
  CONFIG_RDD2_FLIGHT_LOG_FLUSH_PERIOD_MS=400
  CONFIG_RDD2_FLIGHT_LOG_RETRY_MS=200
  CONFIG_RDD2_FLIGHT_LOG_WRITER_STACK_SIZE=8192
  CONFIG_RDD2_FLIGHT_LOG_WRITER_PRIORITY=8
  CONFIG_RDD2_FLIGHT_LOG_FRONT_STACK_SIZE=8192
  CONFIG_RDD2_FLIGHT_LOG_FRONT_PRIORITY=4
)

# The eager (pre-deferral) rotation logic is selected for the comparison run.
if(RDD2_FLIGHT_LOG_EAGER_ROTATE)
  target_compile_definitions(app PRIVATE CONFIG_RDD2_FLIGHT_LOG_EAGER_ROTATE=1)
endif()
