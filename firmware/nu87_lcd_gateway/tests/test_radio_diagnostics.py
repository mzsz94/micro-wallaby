"""Exercise the production radio diagnostics with a locked heap and vendor API."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "src" / "radio_diagnostics.c"

HARNESS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "wifi_intf_drv_to_app_basic.h"
#include "rtw_coex_host_api.h"

void gateway_radio_diagnostics(const char *stage);
int __wrap_wifi_connect_zephyr(uint8_t *, uint8_t, uint8_t *, uint8_t, uint8_t);
int __wrap_rtk_coex_btc_bt_rfk(void *, uint16_t);
void __wrap_bt_coex_process_rx_frame(uint8_t, uint8_t *, uint16_t);
int gateway_hci_diagnostics(char *, size_t);
void bt_vendor_process_tx_frame(uint8_t, uint8_t *, uint16_t);
void gateway_radio_coex_diagnostics(void);

struct k_heap _system_heap;
static int locked, stats_result, vendor_result, status_result;
static unsigned int stats_calls, vendor_calls, status_calls;
static uint8_t *expected_ssid, *expected_psk;
static uint8_t expected_ssid_len, expected_psk_len, expected_channel;
static char output[4096];
static size_t output_length;
static struct k_spinlock *held_lock;
static void *expected_rfk;
static uint16_t expected_rfk_length;
static int rfk_result;
static unsigned int rfk_calls, rx_calls;
static uint8_t expected_rx_type, *expected_rx_data;
static uint16_t expected_rx_length;
static unsigned int coex_calls[6];
static unsigned int scan_tx_calls, scan_rx_calls;
static int native_guard;

k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
    assert(lock != NULL && !locked);
    held_lock = lock;
    locked = 1;
    return 42;
}

void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
    assert(lock == held_lock && locked && key == 42);
    locked = 0;
}

int sys_heap_runtime_stats_get(struct sys_heap *heap, struct sys_memory_stats *stats)
{
    assert(heap == &_system_heap.heap && locked && held_lock == &_system_heap.lock);
    stats_calls++;
    if (stats_result == 0) {
        stats->free_bytes = 50000 - vendor_calls * 1000;
        stats->allocated_bytes = 15000 + vendor_calls * 1000;
        stats->max_allocated_bytes = 20000 + vendor_calls * 1000;
    }
    return stats_result;
}

void mock_log(const char *format, ...)
{
    /* Logging is allowed to allocate: it must never hold the heap lock. */
    assert(!locked);
    va_list args;
    va_start(args, format);
    int count = vsnprintf(output + output_length, sizeof(output) - output_length,
                          format, args);
    va_end(args);
    assert(count >= 0 && (size_t)count + 1 < sizeof(output) - output_length);
    output_length += (size_t)count;
    output[output_length++] = '\n';
    output[output_length] = '\0';
}

int __real_wifi_connect_zephyr(uint8_t *ssid, uint8_t ssid_len,
                             uint8_t *psk, uint8_t psk_len, uint8_t channel)
{
    assert(!locked && stats_calls == 1 && vendor_calls == 0 && status_calls == 0);
    assert(ssid == expected_ssid && psk == expected_psk);
    assert(ssid_len == expected_ssid_len && psk_len == expected_psk_len);
    assert(channel == expected_channel);
    vendor_calls++;
    return vendor_result;
}

int32_t wifi_get_join_status(uint8_t *status)
{
    assert(!locked && vendor_calls == 1 && status_calls == 0);
    status_calls++;
    if (status_result == 0) {
        *status = vendor_result == 0 ? 9 : 10;
    }
    return status_result;
}

int __real_rtk_coex_btc_bt_rfk(void *parameter, uint16_t length)
{
    assert(!locked && parameter == expected_rfk && length == expected_rfk_length);
    rfk_calls++;
    return rfk_result;
}

void __real_bt_coex_process_rx_frame(uint8_t type, uint8_t *data, uint16_t length)
{
    assert(!locked && native_guard && type == expected_rx_type && data == expected_rx_data);
    assert(length == expected_rx_length);
    rx_calls++;
}

void gateway_coex_scan_tx(uint8_t type, const uint8_t *data, uint16_t length)
{
    (void)type; (void)data; (void)length;
    assert(!locked);
    scan_tx_calls++;
}

void gateway_coex_scan_rx(uint8_t type, const uint8_t *data, uint16_t length)
{
    assert(!locked && native_guard && rx_calls == scan_rx_calls + 1);
    assert(type == expected_rx_type && data == expected_rx_data && length == expected_rx_length);
    scan_rx_calls++;
}
void gateway_coex_scan_rx_begin(void) { assert(!locked && !native_guard); native_guard = 1; }
void gateway_coex_scan_rx_end(void) { assert(!locked && native_guard); native_guard = 0; }

bool rtk_coex_com_coex_is_enabled(void) { assert(!locked); coex_calls[0]++; return true; }
uint8_t rtk_coex_wl_get_rfe_type(void) { assert(!locked); coex_calls[1]++; return 3; }
uint8_t rtk_coex_wl_get_ant_div(void) { assert(!locked); coex_calls[2]++; return 0; }
uint8_t rtk_coex_wl_get_bt_pos(void) { assert(!locked); coex_calls[3]++; return 1; }
uint8_t rtk_coex_wl_get_ant_type(void) { assert(!locked); coex_calls[4]++; return 2; }
void rtk_coex_com_state_get(void) { assert(!locked); coex_calls[5]++; }

static void check_rfk(void *parameter, uint16_t length, int result, int type)
{
    char expected[128], json[768];
    expected_rfk = parameter;
    expected_rfk_length = length;
    rfk_result = result;
    unsigned int before = rfk_calls;
    assert(__wrap_rtk_coex_btc_bt_rfk(parameter, length) == result);
    assert(rfk_calls == before + 1);
    snprintf(expected, sizeof(expected), "BT RF calibration type=%d result=%d", type, result);
    assert(strstr(output, expected));
    assert(gateway_hci_diagnostics(json, sizeof(json)) == (int)strlen(json));
    snprintf(expected, sizeof(expected), "\"rfk_calls\":%u,\"last_rfk_type\":%d,\"last_rfk_result\":%d",
             rfk_calls, type, result);
    assert(strstr(json, expected));
}

static void check_rx(uint8_t type, uint8_t *data, uint16_t length)
{
    expected_rx_type = type;
    expected_rx_data = data;
    expected_rx_length = length;
    unsigned int before = rx_calls;
    size_t before_log = output_length;
    __wrap_bt_coex_process_rx_frame(type, data, length);
    assert(rx_calls == before + 1);
    /* RX counts never log addresses, names, payloads or per-packet lines. */
    assert(output_length == before_log);
}

static void test_rx(void)
{
    uint8_t short_event[] = {0x3e};
    uint8_t incomplete[] = {0x3e, 2, 0x02};
    uint8_t empty_meta[] = {0x3e, 0};
    uint8_t command_event[] = {0x0e, 1, 1};
    uint8_t other_meta[] = {0x3e, 1, 0x01};
    uint8_t advertising[] = {0x3e, 2, 0x02, 0};
    uint8_t saved[sizeof(advertising)];
    memcpy(saved, advertising, sizeof(saved));
    check_rx(0x02, NULL, UINT16_MAX); /* Non-event: never inspect data. */
    check_rx(0x04, NULL, 0);
    check_rx(0x04, NULL, 99);
    check_rx(0x04, short_event, 0);
    check_rx(0x04, short_event, 1);
    check_rx(0x04, incomplete, sizeof(incomplete));
    check_rx(0x04, empty_meta, sizeof(empty_meta));
    check_rx(0x04, command_event, sizeof(command_event));
    check_rx(0x04, other_meta, sizeof(other_meta));
    check_rx(0x04, advertising, sizeof(advertising));
    assert(memcmp(advertising, saved, sizeof(saved)) == 0);
    char json[768];
    assert(gateway_hci_diagnostics(json, sizeof(json)) == (int)strlen(json));
    assert(strstr(json, "{\"rx_packets\":10,\"rx_events\":9,\"le_events\":2,\"adv_events\":1,"
                       "\"malformed_events\":6,\"rfk_calls\":0,\"last_rfk_type\":-1,\"last_rfk_result\":-1,") == json);
    size_t exact_size = strlen(json) + 1;
    assert(gateway_hci_diagnostics(json, exact_size) == (int)exact_size - 1);
    assert(gateway_hci_diagnostics(json, exact_size - 1) == -ENOSPC && json[0] == '\0');
    assert(gateway_hci_diagnostics(json, 1) == -ENOSPC && json[0] == '\0');
    json[0] = 'x';
    assert(gateway_hci_diagnostics(json, 0) == -EINVAL && json[0] == 'x');
    assert(gateway_hci_diagnostics(NULL, sizeof(json)) == -EINVAL);
}

static void check_tx(uint8_t *data, uint16_t length)
{
    uint8_t copy[32];
    assert(length <= sizeof(copy));
    memcpy(copy, data, length);
    size_t before_log = output_length;
    bt_vendor_process_tx_frame(0x01, data, length);
    assert(memcmp(data, copy, length) == 0 && before_log == output_length);
}

static void complete_command(uint16_t opcode, uint8_t status, bool command_status)
{
    uint8_t event[6];
    event[0] = command_status ? 0x0f : 0x0e;
    event[1] = 4;
    if (command_status) {
        event[2] = status;
        event[3] = 1;
        event[4] = (uint8_t)opcode;
        event[5] = (uint8_t)(opcode >> 8);
    } else {
        event[2] = 1;
        event[3] = (uint8_t)opcode;
        event[4] = (uint8_t)(opcode >> 8);
        event[5] = status;
    }
    check_rx(0x04, event, sizeof(event));
}

static void test_tx(void)
{
    uint8_t short_command[] = {0x0c};
    uint8_t truncated[] = {0x0c, 0x20, 2, 1};
    uint8_t wrong_length[] = {0x0c, 0x20, 0};
    uint8_t unknown[] = {0x0d, 0x20, 0};
    char before[768], after[768];
    assert(gateway_hci_diagnostics(before, sizeof(before)) > 0);
    bt_vendor_process_tx_frame(0x02, NULL, UINT16_MAX);
    bt_vendor_process_tx_frame(0x01, NULL, UINT16_MAX);
    check_tx(short_command, 0);
    check_tx(short_command, 1);
    check_tx(truncated, sizeof(truncated));
    check_tx(wrong_length, sizeof(wrong_length));
    check_tx(unknown, sizeof(unknown));
    assert(gateway_hci_diagnostics(after, sizeof(after)) > 0);
    assert(strcmp(before, after) == 0);

    uint8_t reset[] = {0x03, 0x0c, 0};
    uint8_t mask[] = {0x01, 0x0c, 8, 0, 0, 0, 0, 0, 0, 0, 0x20};
    uint8_t le_mask[] = {0x01, 0x20, 8, 2, 0, 0, 0, 0, 0, 0, 0};
    uint8_t parameters[] = {0x0b, 0x20, 7, 1, 0x60, 0, 0x30, 0, 1, 0};
    uint8_t enable[] = {0x0c, 0x20, 2, 1, 0};
    check_tx(reset, sizeof(reset));
    complete_command(0x0c03, 0, false);
    check_tx(mask, sizeof(mask));
    complete_command(0x0c01, 0, false);
    check_tx(le_mask, sizeof(le_mask));
    complete_command(0x2001, 0, false);
    check_tx(parameters, sizeof(parameters));
    complete_command(0x200b, 0, false);
    check_tx(enable, sizeof(enable));
    assert(gateway_hci_diagnostics(after, sizeof(after)) > 0);
    assert(strstr(after, "\"scan_enable\":[1,-1]"));
    complete_command(0x200c, 0, false);
    assert(gateway_hci_diagnostics(after, sizeof(after)) > 0);
    assert(strstr(after, "\"commands\":{\"reset\":[1,0],\"mask\":[1,0],\"le_mask\":[1,0],"
                         "\"scan_params\":[1,0],\"scan_enable\":[1,0]}"));
    assert(strstr(after, "\"scan\":{\"meta_mask\":1,\"adv_mask\":1,\"type\":1,\"interval\":96,"
                         "\"window\":48,\"own_addr\":1,\"filter\":0,\"enable\":1,\"duplicates\":0}"));

    /* Passive mode and duplicate filtering are observed without issuing any commands. */
    parameters[3] = 0;
    parameters[4] = 0x34; parameters[5] = 0x12;
    parameters[6] = 0x78; parameters[7] = 0x01;
    parameters[8] = 0; parameters[9] = 1;
    check_tx(parameters, sizeof(parameters));
    complete_command(0x200b, 12, true);
    enable[3] = 0; enable[4] = 1;
    check_tx(enable, sizeof(enable));
    complete_command(0x200c, 5, false);
    uint8_t short_complete[] = {0x0e, 4, 1, 0x0c, 0x20};
    check_rx(0x04, short_complete, sizeof(short_complete));
    assert(gateway_hci_diagnostics(after, sizeof(after)) > 0);
    assert(strstr(after, "\"scan_params\":[2,12],\"scan_enable\":[2,5]"));
    assert(strstr(after, "\"type\":0,\"interval\":4660,\"window\":376,\"own_addr\":0,"
                         "\"filter\":1,\"enable\":0,\"duplicates\":1"));
    check_tx(reset, sizeof(reset));
    assert(gateway_hci_diagnostics(after, sizeof(after)) > 0);
    assert(strstr(after, "\"reset\":[2,-1]"));
    assert(strstr(after, "\"meta_mask\":-1,\"adv_mask\":-1"));
    assert(strstr(after, "\"enable\":-1"));
    assert(output_length == 0);
}

static void check_connect(int result, int status_error, int null_arguments)
{
    uint8_t ssid[] = "PRIVATE-NETWORK-DO-NOT-LOG";
    uint8_t psk[] = "PRIVATE-PASSWORD-DO-NOT-LOG";
    char address[64];
    stats_calls = vendor_calls = status_calls = 0;
    output_length = 0;
    output[0] = '\0';
    vendor_result = result;
    status_result = status_error;
    expected_ssid = null_arguments ? NULL : ssid;
    expected_psk = null_arguments ? NULL : psk;
    expected_ssid_len = null_arguments ? 0 : sizeof(ssid) - 1;
    expected_psk_len = null_arguments ? 0 : sizeof(psk) - 1;
    expected_channel = null_arguments ? 0 : 149;
    assert(__wrap_wifi_connect_zephyr(expected_ssid, expected_ssid_len,
        expected_psk, expected_psk_len, expected_channel) == result);
    assert(stats_calls == 2 && vendor_calls == 1 && status_calls == 1 && !locked);
    assert(strstr(output, "before_wifi_connect: free=50000 allocated=15000 max=20000"));
    assert(strstr(output, "after_wifi_connect: free=49000 allocated=16000 max=21000"));
    char expected[128];
    snprintf(expected, sizeof(expected), "vendor result=%d join_status=%u status_result=%d",
             result, status_error ? 255U : (result == 0 ? 9U : 10U), status_error);
    assert(strstr(output, expected));
    assert(!strstr(output, (char *)ssid) && !strstr(output, (char *)psk));
    snprintf(address, sizeof(address), "%p", (void *)ssid);
    assert(!strstr(output, address));
    snprintf(address, sizeof(address), "%p", (void *)psk);
    assert(!strstr(output, address));
    assert(strcmp((char *)ssid, "PRIVATE-NETWORK-DO-NOT-LOG") == 0);
    assert(strcmp((char *)psk, "PRIVATE-PASSWORD-DO-NOT-LOG") == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "forwarding") == 0) {
        check_connect(0, 0, 0);
        check_connect(-5, 0, 0);
        check_connect(-22, -12, 0);
        check_connect(7, 0, 1);
    } else if (strcmp(argv[1], "rfk") == 0) {
        struct bt_rfk_param parameter = {BT_IQK, 0xde, 0xad, 0xbe, 0xef};
        check_rfk(&parameter, sizeof(parameter), 0, BT_IQK);
        check_rfk(&parameter, sizeof(parameter), -1, BT_IQK);
        parameter.type = BT_LOK;
        check_rfk(&parameter, sizeof(parameter), 7, BT_LOK);
        check_rfk(&parameter, 0, -22, -1);
        uint8_t one_byte_type[] = {BT_LOK};
        check_rfk(one_byte_type, sizeof(one_byte_type), -22, BT_LOK);
        check_rfk(NULL, UINT16_MAX, -5, -1);
        assert(parameter.rfk_data1 == 0xde && parameter.rfk_data4 == 0xef);
        assert(!strstr(output, "deadbeef") && !strstr(output, "0x"));
    } else if (strcmp(argv[1], "rx") == 0) {
        test_rx();
    } else if (strcmp(argv[1], "tx") == 0) {
        test_tx();
    } else if (strcmp(argv[1], "coex") == 0) {
        gateway_radio_coex_diagnostics();
        for (unsigned int i = 0; i < sizeof(coex_calls) / sizeof(coex_calls[0]); i++) {
            assert(coex_calls[i] == 1);
        }
        assert(strcmp(output, "Coex enabled=1 rfe=3 ant_div=0 bt_pos=1 ant_type=2\n") == 0);
    } else {
        assert(strcmp(argv[1], "stats_failure") == 0);
        stats_result = -22;
        gateway_radio_diagnostics(NULL);
        assert(!locked && stats_calls == 1);
        assert(strcmp(output, "Heap unknown: stats error -22\n") == 0);
    }
    puts(argv[1]);
    return 0;
}
"""


class RadioDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if compiler is None:
            raise unittest.SkipTest("A host C compiler is required")
        temporary = tempfile.TemporaryDirectory(prefix="radio-diagnostics-test-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        headers = {
            "zephyr/sys/sys_heap.h": """#pragma once
#include <stddef.h>
struct sys_heap { int placeholder; };
struct sys_memory_stats { size_t free_bytes, allocated_bytes, max_allocated_bytes; };
int sys_heap_runtime_stats_get(struct sys_heap *, struct sys_memory_stats *);
""",
            "zephyr/kernel.h": """#pragma once
#include <stdint.h>
#include <zephyr/sys/sys_heap.h>
struct k_spinlock { int placeholder; };
typedef unsigned int k_spinlock_key_t;
struct k_heap { struct sys_heap heap; int wait_q; struct k_spinlock lock; };
k_spinlock_key_t k_spin_lock(struct k_spinlock *);
void k_spin_unlock(struct k_spinlock *, k_spinlock_key_t);
""",
            "zephyr/logging/log.h": """#pragma once
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(name, level) typedef int mock_log_module
void mock_log(const char *, ...);
#define LOG_INF(...) mock_log(__VA_ARGS__)
#define LOG_WRN(...) mock_log(__VA_ARGS__)
""",
            "wifi_intf_drv_to_app_basic.h": """#pragma once
#include <stdint.h>
int32_t wifi_get_join_status(uint8_t *);
int wifi_connect_zephyr(uint8_t *, uint8_t, uint8_t *, uint8_t, uint8_t);
""",
            "rtw_coex_host_api.h": """#pragma once
#include <stdint.h>
enum bt_rfk_type { BT_LOK = 1, BT_IQK = 6 };
/* The pinned HCI DWARF has a short enum and a five-byte structure. */
struct bt_rfk_param { uint8_t type, rfk_data1, rfk_data2, rfk_data3, rfk_data4; };
int rtk_coex_btc_bt_rfk(void *, uint16_t);
""",
            "gateway_link.h": "#include <stddef.h>\nvoid gateway_radio_diagnostics(const char *stage);\n"
                              "int gateway_hci_diagnostics(char *, size_t);\n"
                              "void gateway_radio_coex_diagnostics(void);\n"
                              "void gateway_coex_scan_tx(uint8_t, const uint8_t *, uint16_t);\n"
                              "void gateway_coex_scan_rx(uint8_t, const uint8_t *, uint16_t);\n"
                              "void gateway_coex_scan_rx_begin(void);\n"
                              "void gateway_coex_scan_rx_end(void);\n",
        }
        for name, content in headers.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        harness = directory / "harness.c"
        harness.write_text(HARNESS, encoding="utf-8")
        cls.executable = directory / "radio_diagnostics"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2",
             "-I", str(directory), str(SOURCE), str(harness), "-o", str(cls.executable)],
            capture_output=True, text=True, timeout=60, check=False,
        )
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run(
            [str(self.executable), name], capture_output=True, text=True, timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), name)

    def test_vendor_forwarding_locked_snapshots_and_credential_redaction(self):
        self.run_case("forwarding")

    def test_stats_error_unlocks_and_does_not_log_uninitialized_stats(self):
        self.run_case("stats_failure")

    def test_rfk_preserves_arguments_results_and_logs_only_numeric_metadata(self):
        self.run_case("rfk")

    def test_rx_forwarding_short_frames_and_bounded_json(self):
        self.run_case("rx")

    def test_tx_masks_scan_parameters_completion_errors_and_malformed_commands(self):
        self.run_case("tx")

    def test_coexistence_snapshot_only_calls_readers_outside_locks(self):
        self.run_case("coex")


if __name__ == "__main__":
    unittest.main()
