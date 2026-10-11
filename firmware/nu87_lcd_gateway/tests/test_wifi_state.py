#!/usr/bin/env python3
"""Exercise production Wi-Fi state functions against deterministic host mocks.

Function bodies are extracted unchanged from wifi.c each run, rather than
maintaining a second state-machine implementation. This checks cancellation and
readiness decisions; kernel scheduling and vendor radio behavior need board tests.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "src" / "wifi.c"


def source_block(source: str, signature: str, *, enum: bool = False) -> str:
    """Select a complete definition using the source's top-level brace style."""
    match = re.search(rf"(?ms)^{re.escape(signature)}\b.*?^\}}", source)
    if match is None:
        raise AssertionError(f"Production definition not found: {signature}")
    return match.group(0) + (";" if enum else "")


MOCKS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <arpa/inet.h>
#define net_in_addr in_addr
#define NET_IPV4_ADDR_LEN 16
#define NET_AF_INET AF_INET
#define NET_DHCPV4_BOUND 7
#define MSEC_PER_SEC 1000
#define CONFIG_GATEWAY_WIFI_CONNECT_RETRY_SECONDS 10
#define CONFIG_GATEWAY_WIFI_CONNECT_TIMEOUT_SECONDS 60
#define CONFIG_GATEWAY_WIFI_SSID "test"
#define CONFIG_GATEWAY_WIFI_PSK "testpass"
#define CONFIG_GATEWAY_WIFI_CHANNEL 0
#define DISCONNECT_TIMEOUT_MS 10000
#define WIFI_CHANNEL_ANY 255
#define WIFI_FREQ_BAND_2_4_GHZ 1
#define WIFI_SECURITY_TYPE_NONE 0
#define NET_REQUEST_WIFI_CONNECT 1
#define K_FOREVER 0
#define K_SECONDS(x) ((x) * 1000)
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) ((void)(x))
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
struct k_work { int unused; };
struct k_work_delayable { int unused; };
struct k_work_q { int unused; };
struct net_if {
    struct { struct { int state; struct in_addr requested_ip; } dhcpv4; } config;
};
struct wifi_connect_req_params {
    const uint8_t *ssid, *psk;
    size_t ssid_length, psk_length;
    int security, channel, band;
};
static struct net_if iface, *sta_iface = &iface;
static struct k_work_q wifi_work_queue;
static struct k_work disconnect_work;
static struct k_work_delayable connect_timeout_work;
static int state_lock, ipv4_ready;
static uint32_t generation, cancel_generation, timeout_generation;
static int64_t disconnect_deadline_ms, timeout_deadline_ms, now;
static unsigned disconnect_idle_samples;
static bool association_complete, connect_result_ok, address_valid;
static bool channel_hint_tried;
static char ipv4_address[16];
static int radio_result, radio_state, query_count, schedules, retries;
static int stops, carrier_off, queued_cancellations, cancelled_timeouts, ready_logs;
static int connect_behavior;
static unsigned connect_calls;
static int last_connect_channel;
static void k_mutex_lock(void *p, int t) { (void)p; (void)t; }
static void k_mutex_unlock(void *p) { (void)p; }
static void atomic_clear(int *p) { *p = 0; }
static void atomic_set(int *p, int v) { *p = v; }
static int atomic_get(int *p) { return *p; }
static int64_t k_uptime_get(void) { return now; }
static int wifi_get_join_status(uint8_t *out) {
    query_count++; *out = radio_state; return radio_result;
}
static int net_addr_pton(int family, const char *str, void *out) {
    return inet_pton(family, str, out) == 1 ? 0 : -1;
}
static void schedule_disconnect_check(void) { schedules++; }
static void schedule_reconnect(void) { retries++; }
static void net_dhcpv4_stop(struct net_if *p) { stops++; p->config.dhcpv4.state = 0; }
static void net_eth_carrier_off(struct net_if *p) { (void)p; carrier_off++; }
static int k_work_reschedule(struct k_work_delayable *p, int64_t t) {
    (void)p; (void)t; return 0;
}
static int k_work_submit_to_queue(struct k_work_q *q, struct k_work *w) {
    (void)q; (void)w; queued_cancellations++; return 0;
}
static int k_work_cancel_delayable(struct k_work_delayable *w) {
    (void)w; cancelled_timeouts++; return 0;
}
static int configured_security(void) { return WIFI_SECURITY_TYPE_NONE; }
static int prepare_sta_link_address(void) { return 0; }
static bool net_if_is_admin_up(struct net_if *p) { (void)p; return true; }
static int net_if_up(struct net_if *p) { (void)p; return 0; }
static void log_ready_address(void) { ready_logs++; }
static int net_mgmt(int request, struct net_if *p, void *params, size_t size);
"""


TESTS = r"""
static void current_lease(void) {
    connect_result_ok = address_valid = true;
    strcpy(ipv4_address, "192.168.0.20");
    inet_pton(AF_INET, ipv4_address, &iface.config.dhcpv4.requested_ip);
    iface.config.dhcpv4.state = NET_DHCPV4_BOUND;
}
static int net_mgmt(int request, struct net_if *p, void *params, size_t size) {
    (void)p; (void)params; (void)size;
    assert(request == NET_REQUEST_WIFI_CONNECT);
    struct wifi_connect_req_params *wifi = params;
    connect_calls++;
    last_connect_channel = wifi->channel;
    assert(wifi->channel == (CONFIG_GATEWAY_WIFI_CHANNEL && connect_calls == 1 ?
                            CONFIG_GATEWAY_WIFI_CHANNEL : WIFI_CHANNEL_ANY));
    if (connect_behavior == 3) {
        return -EAGAIN; /* The AP is absent from the old channel. */
    }
    if (connect_behavior == 1) {
        /* The real timeout fires while the blocking vendor connect runs. */
        now = timeout_deadline_ms;
        connect_timeout_handler(NULL);
        return -EIO;
    }
    if (connect_behavior == 2) {
        /* DHCP notifications can arrive before the synchronous call returns. */
        phase = WIFI_PHASE_WAIT_DHCP;
        current_lease();
        assert(!mark_ready_if_complete_locked());
    }
    return 0;
}
static void reset(void) {
    phase = WIFI_PHASE_DISCONNECTING;
    generation = cancel_generation = 3; now = 0;
    disconnect_deadline_ms = 10000; disconnect_idle_samples = 0;
    radio_result = 0; radio_state = 9;
    query_count = schedules = retries = stops = carrier_off = 0;
    queued_cancellations = cancelled_timeouts = ready_logs = connect_behavior = 0;
    association_complete = connect_result_ok = address_valid = false;
    channel_hint_tried = false; connect_calls = 0; last_connect_channel = 0;
    ipv4_ready = 0; memset(&iface, 0, sizeof(iface)); ipv4_address[0] = 0;
}
int main(void) {
    reset();
    disconnect_check_handler(NULL);
    assert(retries == 0 && schedules == 1);
    radio_state = 11; now = 250; disconnect_check_handler(NULL);
    assert(retries == 0);
    now = 500; disconnect_check_handler(NULL);
    assert(retries == 1 && stops == 1 && carrier_off == 1);
    assert(phase == WIFI_PHASE_RETRY_WAIT);

    /* Query errors invalidate an earlier idle observation. */
    reset(); radio_state = 11; disconnect_check_handler(NULL); radio_result = -1;
    disconnect_check_handler(NULL);
    assert(disconnect_idle_samples == 0 && retries == 0);
    radio_result = 0; disconnect_check_handler(NULL); assert(retries == 0);
    disconnect_check_handler(NULL); assert(retries == 1);

    /* Unknown, FAIL and active radio states never permit a retry at timeout. */
    const int unconfirmed[] = {0, 1, 2, 7, 9, 10};
    for (size_t i = 0; i < sizeof(unconfirmed) / sizeof(unconfirmed[0]); i++) {
        reset(); radio_state = unconfirmed[i]; now = 10000;
        disconnect_check_handler(NULL);
        assert(phase == WIFI_PHASE_RETRY_BLOCKED && retries == 0 && schedules == 0);
    }
    reset(); generation++; disconnect_check_handler(NULL);
    assert(query_count == 0 && retries == 0 && schedules == 0);

    /* Late DHCP callbacks cannot publish an old connection or old lease. */
    reset(); phase = WIFI_PHASE_WAIT_DHCP; current_lease();
    assert(!mark_ready_if_complete_locked() && ipv4_ready == 0);
    association_complete = true; iface.config.dhcpv4.state = 0;
    assert(!mark_ready_if_complete_locked() && ipv4_ready == 0);
    iface.config.dhcpv4.state = NET_DHCPV4_BOUND; radio_state = 11;
    assert(!mark_ready_if_complete_locked() && ipv4_ready == 0);
    radio_state = 9;
    inet_pton(AF_INET, "192.168.0.21", &iface.config.dhcpv4.requested_ip);
    assert(!mark_ready_if_complete_locked() && ipv4_ready == 0);
    inet_pton(AF_INET, ipv4_address, &iface.config.dhcpv4.requested_ip);
    assert(mark_ready_if_complete_locked() && ipv4_ready == 1);
    assert(phase == WIFI_PHASE_READY && !mark_ready_if_complete_locked());

    /* Critical regression: a negative connect return cannot bypass cancellation. */
    reset(); phase = WIFI_PHASE_RETRY_WAIT; connect_behavior = 1;
    connect_work_handler(NULL);
    assert(phase == WIFI_PHASE_CANCEL_PENDING && queued_cancellations == 1);
    assert(retries == 0 && cancelled_timeouts == 0 && ipv4_ready == 0);

    /* Normal notification ordering still reaches READY once association returns. */
    reset(); phase = WIFI_PHASE_RETRY_WAIT; connect_behavior = 2;
    connect_work_handler(NULL);
    assert(phase == WIFI_PHASE_READY && ipv4_ready == 1 && ready_logs == 1);
    assert(cancelled_timeouts == 1 && retries == 0);

    /* An old hotspot channel must not strand all later association attempts. */
    reset(); phase = WIFI_PHASE_CANCEL_PENDING;
    connect_work_handler(NULL);
    assert(connect_calls == 0 && !channel_hint_tried);
    phase = WIFI_PHASE_RETRY_WAIT; connect_behavior = 3;
    connect_work_handler(NULL);
    assert(phase == WIFI_PHASE_RETRY_WAIT && retries == 1 && connect_calls == 1);
    assert(last_connect_channel == (CONFIG_GATEWAY_WIFI_CHANNEL ?
                                   CONFIG_GATEWAY_WIFI_CHANNEL : WIFI_CHANNEL_ANY));
    connect_work_handler(NULL);
    assert(connect_calls == 2 && last_connect_channel == WIFI_CHANNEL_ANY);
    connect_behavior = 2;
    connect_work_handler(NULL);
    assert(phase == WIFI_PHASE_READY && ipv4_ready == 1 && connect_calls == 3);
    assert(last_connect_channel == WIFI_CHANNEL_ANY);
    puts("Wi-Fi cancellation and current-lease source-function tests passed");
}
"""


class WifiStateTests(unittest.TestCase):
    def test_production_state_functions(self):
        self.check_channel(0)

    def test_explicit_observed_ap_channel(self):
        self.check_channel(1)
        self.check_channel(14)

    def check_channel(self, channel):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("A host C compiler is required")
        source = SOURCE.read_text()
        definitions = [
            source_block(source, "enum ameba_join_status", enum=True),
            source_block(source, "enum wifi_phase", enum=True),
            "static enum wifi_phase phase;",
        ]
        for signature in (
            "static void clear_ipv4_locked",
            "static bool mark_ready_if_complete_locked",
            "static int arm_connect_timeout_locked",
            "static void disconnect_check_handler",
            "static void connect_timeout_handler",
            "static void connect_work_handler",
        ):
            definitions.append(source_block(source, signature))
        with tempfile.TemporaryDirectory(prefix="nu87-wifi-state-") as temporary:
            directory = Path(temporary)
            test_source = directory / "test.c"
            executable = directory / "test"
            mocks = MOCKS.replace("#define CONFIG_GATEWAY_WIFI_CHANNEL 0",
                                  f"#define CONFIG_GATEWAY_WIFI_CHANNEL {channel}")
            test_source.write_text(mocks + "\n".join(definitions) + TESTS)
            compiled = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", str(test_source), "-o", str(executable)],
                text=True, capture_output=True, timeout=60,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run(
                [str(executable)], text=True, capture_output=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
