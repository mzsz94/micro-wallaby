# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nu87_imu_bringup)
target_include_directories(app PRIVATE include)
target_sources(app PRIVATE src/main.c src/imu_device.c src/imu_calibration.c src/imu_storage.c)
if(CONFIG_SOC_SERIES_AMEBAD AND NOT CONFIG_WIFI_AMEBA)
  target_sources(app PRIVATE src/ameba_log_compat.c)
endif()
