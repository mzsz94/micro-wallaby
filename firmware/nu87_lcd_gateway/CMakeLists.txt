# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nu87_lcd_gateway)
target_include_directories(app PRIVATE
  include
  ${ZEPHYR_BASE}/drivers/wifi/ameba
  ../wireless_lcd_common/include
  ../wireless_lcd_common/dfu
)
target_sources(app PRIVATE
  src/main.c
  src/wifi.c
  src/timer_core.c
  src/ameba_timer_thread.c
  src/ameba_irq.c
  src/radio_diagnostics.c
  src/hci_transport_diagnostics.c
  src/ameba_coex_scan.c
  src/http_server.c
  src/http_io.c
  src/http_parse.c
)
if(CONFIG_GATEWAY_NU40_KIT)
  target_sources(app PRIVATE src/ble_kit.c src/smp_relay.c src/alarm_relay.c src/robot_state.c src/ota_disabled.c)
else()
  target_sources(app PRIVATE src/ble_link.c src/ota.c
    ../wireless_lcd_common/dfu/legacy_dfu_client.c)
endif()

# Vendor timer callbacks may take mutexes. Keep them on a private daemon thread
# instead of the pinned SDK's k_timer interrupt callbacks, without editing SDKs.
foreach(timer_api create create_static delete delete_static start stop change_period is_timer_active get_id)
  zephyr_ld_options(-Wl,--wrap=rtos_timer_${timer_api})
endforeach()
# Keep the vendor's strong init_timer_wrapper: it initializes its callback map.

# Realtek HCI uses hardware priority 7; Zephyr reserves one priority level.
zephyr_ld_options(-Wl,--wrap=arch_irq_connect_dynamic)
# Retain the vendor error and heap snapshots before Zephyr maps it to -EAGAIN.
zephyr_ld_options(-Wl,--wrap=wifi_connect_zephyr)
# Observe RF calibration and raw RX counts without changing SDK behavior.
zephyr_ld_options(-Wl,--wrap=rtk_coex_btc_bt_rfk)
zephyr_ld_options(-Wl,--wrap=bt_coex_process_rx_frame)

# Count UART frames before the vendor H4 event pool, preserving the wire bytes.
zephyr_ld_options(-Wl,--wrap=hci_uart_read)
zephyr_ld_options(-Wl,--wrap=UART_LineStatusGet)
