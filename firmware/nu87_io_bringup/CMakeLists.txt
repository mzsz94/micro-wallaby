# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microwallaby_nu87_io_bringup)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE
  src/app_store.c
  src/behavior_sm.c
  src/diag_stats.c
  src/health_log.c
  src/indicator.c
  src/input_filter.c
  src/main.c
  src/safety_ctrl.c
  src/sensor_fast.c
)
