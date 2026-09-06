# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
find_package(Python REQUIRED COMPONENTS Interpreter)
project(microwallaby_nu87_web_nes_poc)

set(web_dir ${CMAKE_CURRENT_SOURCE_DIR}/web)
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)

target_include_directories(app PRIVATE
  include
  ${CMAKE_CURRENT_SOURCE_DIR}/../nu87_io_bringup/include
)

target_sources(app PRIVATE
  src/input_protocol.c
  src/joystick.c
  src/main.c
  src/web_server.c
  src/websocket_control.c
  src/wifi_sta.c
  ../nu87_io_bringup/src/input_filter.c
)

target_link_libraries(app PRIVATE zephyr_interface zephyr)

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(
  NAME http_resource_desc_mw_http_service
  KVMA RAM_REGION
  GROUP RODATA_REGION
)

generate_inc_file_for_target(app
  ${web_dir}/index.html
  ${gen_dir}/mw_index.html.gz.inc
  --gzip
)
generate_inc_file_for_target(app
  ${web_dir}/app.js
  ${gen_dir}/mw_app.js.gz.inc
  --gzip
)
generate_inc_file_for_target(app
  ${web_dir}/controller.js
  ${gen_dir}/mw_controller.js.gz.inc
  --gzip
)
generate_inc_file_for_target(app
  ${web_dir}/vendor/jsnes.min.js
  ${gen_dir}/mw_jsnes.min.js.gz.inc
  --gzip
)
