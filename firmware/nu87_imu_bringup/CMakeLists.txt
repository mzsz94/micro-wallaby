cmake_minimum_required(VERSION 3.20.0)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microwallaby_nu87_imu_bringup)
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE IMU_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE IMU_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(IMU_DIRTY)
  string(APPEND IMU_REVISION "+dirty")
endif()
file(WRITE ${PROJECT_BINARY_DIR}/imu-source-revision.txt "${IMU_REVISION}\n")
target_include_directories(app PRIVATE include)
target_sources(app PRIVATE src/main.c src/imu_core.c src/imu_sensor.c
  src/imu_calibration.c src/imu_storage.c)
if(CONFIG_SOC_SERIES_AMEBAD AND NOT CONFIG_WIFI_AMEBA)
  target_sources(app PRIVATE src/ameba_log_compat.c)
endif()
