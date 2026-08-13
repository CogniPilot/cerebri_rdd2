# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

get_filename_component(_workspace_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ZEPHYR_EXTRA_MODULES
  "${_workspace_root}/.devenv/state/west/modules/lib/zros"
  "${_workspace_root}/.devenv/state/west/modules/lib/csyn"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mission_shell)

target_include_directories(app PRIVATE
  "${_workspace_root}/src"
  "${_workspace_root}/subsys/gnss_source"
  "${_workspace_root}/.devenv/state/west/modules/lib/zros/include"
)
target_sources(app PRIVATE src/main.c)
