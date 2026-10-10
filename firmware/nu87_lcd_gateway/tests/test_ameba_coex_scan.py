"""Compile the production Ameba scan reporter; verify controller-gated mailboxes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "src" / "ameba_coex_scan.c"

KERNEL = r"""
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
struct k_spinlock { int unused; };
typedef unsigned int k_spinlock_key_t;
struct k_sem { unsigned int wakes; };
struct k_mutex { pthread_mutex_t mutex; };
#define K_MUTEX_DEFINE(name) struct k_mutex name = {PTHREAD_MUTEX_INITIALIZER}
#define K_SEM_DEFINE(name, initial, limit) struct k_sem name = {initial}
#define K_THREAD_DEFINE(name, size, entry, a, b, c, priority, options, delay) \
    void (*const name)(void *, void *, void *) = entry
#define K_FOREVER (-1)
typedef int k_timeout_t;
#define K_MSEC(ms) ((int)(ms))
int64_t k_uptime_get(void);
k_spinlock_key_t k_spin_lock(struct k_spinlock *);
void k_spin_unlock(struct k_spinlock *, k_spinlock_key_t);
void k_sem_give(struct k_sem *);
int k_sem_take(struct k_sem *, int);
int k_mutex_lock(struct k_mutex *, int);
int k_mutex_unlock(struct k_mutex *);
"""

HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#include "ameba_coex_scan.c"

static int locked, worker;
static unsigned int count;
static uint8_t reports[64][8];
static void (*during_report)(void);
static pthread_mutex_t spin = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int guard_owned;
static int64_t now;
int64_t k_uptime_get(void) { return now; }
k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
    pthread_mutex_lock(&spin);
    assert(lock == &state_lock && !locked); locked = 1; return 42;
}
void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
    assert(lock == &state_lock && locked && key == 42); locked = 0;
    pthread_mutex_unlock(&spin);
}
int k_mutex_lock(struct k_mutex *mutex, int timeout)
{
    (void)timeout;
    assert(mutex == &native_rx_guard && !guard_owned);
    int result = pthread_mutex_lock(&mutex->mutex); assert(!result);
    assert(!locked); guard_owned = 1; return 0;
}
int k_mutex_unlock(struct k_mutex *mutex)
{
    assert(mutex == &native_rx_guard && guard_owned && !locked);
    guard_owned = 0; return pthread_mutex_unlock(&mutex->mutex);
}
void k_sem_give(struct k_sem *sem) { assert(!locked); sem->wakes++; }
int k_sem_take(struct k_sem *sem, int timeout) { (void)sem; (void)timeout; return 0; }
void rtk_coex_btc_bt_hci_notify(uint8_t *data, uint16_t length, uint8_t type)
{
    assert(!locked && worker && guard_owned && length == 8 && type == 3 && count < 64);
    memcpy(reports[count++], data, 8);
    if (during_report) { void (*callback)(void) = during_report; during_report = NULL; callback(); }
}
static void receive(uint8_t type, const uint8_t *data, uint16_t length)
{
    gateway_coex_scan_rx_begin();
    gateway_coex_scan_rx(type, data, length);
    gateway_coex_scan_rx_end();
}
static void flush(void)
{
    worker = 1;
    while (report_pending()) { }
    worker = 0;
}
static void command(uint16_t opcode, const uint8_t *data, uint8_t length)
{
    uint8_t packet[32] = {(uint8_t)opcode, (uint8_t)(opcode >> 8), length};
    assert(length <= sizeof(packet) - 3);
    if (length) { memcpy(packet + 3, data, length); }
    gateway_coex_scan_tx(1, packet, length + 3);
}
static void complete(uint16_t opcode, uint8_t status)
{
    uint8_t packet[] = {0x0e, 4, 1, (uint8_t)opcode, (uint8_t)(opcode >> 8), status};
    receive(4, packet, sizeof(packet));
}
static void status(uint16_t opcode, uint8_t value)
{
    uint8_t packet[] = {0x0f, 4, value, 1, (uint8_t)opcode, (uint8_t)(opcode >> 8)};
    receive(4, packet, sizeof(packet));
}
static void parameters(void)
{
    const uint8_t data[] = {1, 0x60, 0, 0x30, 0, 1, 0};
    command(0x200b, data, sizeof(data)); complete(0x200b, 0);
}
static void enable(uint8_t value)
{
    const uint8_t data[] = {value, 0};
    command(0x200c, data, sizeof(data)); complete(0x200c, 0);
}
static void check(unsigned int index, const uint8_t *expected)
{
    assert(index < count && memcmp(reports[index], expected, 8) == 0);
}
static void basic(void)
{
    const uint8_t params[] = {1, 0x60, 0, 0x30, 0, 1, 0};
    const uint8_t on[] = {1, 0};
    command(0x200b, params, sizeof(params)); flush(); assert(count == 0);
    complete(0x200b, 0); flush(); assert(count == 0);
    command(0x200c, on, sizeof(on)); flush(); assert(count == 0);
    status(0x200c, 0); flush(); assert(count == 0); /* acceptance is not completion */
    complete(0x200c, 0); assert(count == 0); flush(); assert(count == 3);
    check(0, (uint8_t[]){0x30,0,0x2d,0x10,4,0,0,0});
    check(1, (uint8_t[]){0x30,0,0x2e,0x40,0x30,0,0x60,0});
    check(2, (uint8_t[]){0x27,6,0,0,0,0x20,0,0});
    enable(1); flush(); assert(count == 3); /* no duplicate report */
    enable(0); flush(); assert(count == 5);
    check(3, (uint8_t[]){0x30,0,0x2d,0x10,0,0,0,0});
    check(4, (uint8_t[]){0x27,6,0,0,0,0,0,0});
}
static void failures(void)
{
    const uint8_t on[] = {1, 0};
    parameters();
    command(0x200c, on, sizeof(on)); complete(0x200c, 0x0c); flush(); assert(count == 0);
    complete(0x200c, 0); flush(); assert(count == 0); /* rejected pending command cleared */
    command(0x200c, on, sizeof(on)); status(0x200c, 1); complete(0x200c, 0);
    flush(); assert(count == 0);
    command(0x200c, on, sizeof(on)); command(0x200c, on, sizeof(on));
    complete(0x200c, 0); complete(0x200c, 0); flush(); assert(count == 0);
    assert(state.rejected == 3 && state.enabled == -1);
    enable(1); flush(); assert(count == 3);
    command(0x0c03, NULL, 0); complete(0x0c03, 1); flush(); assert(count == 3 && state.enabled == 1);
    command(0x0c03, NULL, 0); flush(); assert(state.enabled == 1);
    complete(0x0c03, 0); flush(); assert(count == 5 && !state.parameters && state.enabled == 0);
    complete(0x200c, 0); flush(); assert(count == 5);
}
static void malformed(void)
{
    const uint8_t valid_tx[] = {0x0c,0x20,2,1,0};
    const uint8_t valid_rx[] = {0x0e,4,1,0x0c,0x20,0};
    for (size_t n = 0; n < sizeof(valid_tx); ++n) {
        uint8_t *packet = n ? malloc(n) : NULL;
        if (n) memcpy(packet, valid_tx, n);
        gateway_coex_scan_tx(1, packet, n); free(packet);
    }
    for (size_t n = 0; n < sizeof(valid_rx); ++n) {
        uint8_t *packet = n ? malloc(n) : NULL;
        if (n) memcpy(packet, valid_rx, n);
        receive(4, packet, n); free(packet);
    }
    gateway_coex_scan_tx(2, valid_tx, sizeof(valid_tx));
    receive(2, valid_rx, sizeof(valid_rx));
    gateway_coex_scan_tx(1, NULL, UINT16_MAX);
    receive(4, NULL, UINT16_MAX);
    complete(0x200c, 0); flush(); assert(count == 0 && state.enabled == -1);
    uint8_t invalid[] = {1,2,0,1,0,1,0};
    command(0x200b, invalid, sizeof(invalid)); complete(0x200b, 0);
    assert(!state.parameters && state.rejected == 1);
    char small[1] = {'x'}, json[384];
    assert(gateway_coex_scan_diagnostics(NULL, 12) == -EINVAL);
    assert(gateway_coex_scan_diagnostics(small, 1) == -ENOSPC && small[0] == 0);
    assert(gateway_coex_scan_diagnostics(json, sizeof(json)) > 0);
    assert(strstr(json, "\"enabled\":-1"));
    state.interval = state.window = state.init_interval = state.init_window = UINT16_MAX;
    state.commits = state.rejected = state.batches = state.reports = UINT32_MAX;
    state.create_count = state.cancel_count = state.connection_count = UINT32_MAX;
    state.create_status = state.cancel_status = state.connection_status = UINT8_MAX;
    assert(gateway_coex_scan_diagnostics(json, sizeof(json)) == 359);
    assert(strlen(json) == 359); /* includes all added connection diagnostics */
    assert(gateway_coex_scan_diagnostics(json, 360) == 359);
    assert(gateway_coex_scan_diagnostics(json, 359) == -ENOSPC && json[0] == 0);
}
static void native(void)
{
    parameters(); enable(1); flush(); count = 0;
    /* Native payload flags must survive; only scan-enabled is changed. */
    uint8_t native[] = {0xff,9,0x27,0,6,0xa1,0xb2,0xc3,0x84,0xe5,0xf6};
    receive(4, native, sizeof(native)); flush(); assert(count == 3);
    check(2, (uint8_t[]){0x27,6,0xa1,0xb2,0xc3,0xa4,0xe5,0xf6});
    uint8_t connected[21] = {0x3e,19,1,0,0x34,0x12};
    receive(4, connected, sizeof(connected));
    enable(0); flush(); assert(count == 4 && state.link);
    check(3, (uint8_t[]){0x30,0,0x2d,0x10,0,0,0,0}); /* no synthetic BT-info with link */
    uint8_t disconnected[] = {5,4,0,0x34,0x12,0x13};
    receive(4, disconnected, sizeof(disconnected));
    assert(!state.link);
    enable(1); flush(); assert(count == 6 && state.native_link_seen);
    check(4, (uint8_t[]){0x30,0,0x2d,0x10,4,0,0,0});
    check(5, (uint8_t[]){0x30,0,0x2e,0x40,0x30,0,0x60,0});
    /* Never reassert cached native link/profile flags after disconnect. */
    command(0x0c03, NULL, 0); complete(0x0c03, 0); flush(); assert(count == 8);
    check(7, (uint8_t[]){0x27,6,0,0,0,0,0,0});
}
static pthread_t native_thread;
static atomic_int native_waiting, native_entered;
static void *native_connection(void *unused)
{
    (void)unused;
    uint8_t connected[21] = {0x3e,19,1,0,0x34,0x12};
    atomic_store(&native_waiting, 1);
    gateway_coex_scan_rx_begin();
    atomic_store(&native_entered, 1);
    gateway_coex_scan_rx(4, connected, sizeof(connected));
    gateway_coex_scan_rx_end();
    return NULL;
}
static void connection_during_report(void)
{
    assert(guard_owned);
    assert(!pthread_create(&native_thread, NULL, native_connection, NULL));
    while (!atomic_load(&native_waiting)) sched_yield();
    /* The original native coex path cannot process a link/profile update
     * between taking a scan snapshot and applying its last mailbox. */
    assert(!atomic_load(&native_entered));
}
static void convergence(void)
{
    parameters(); enable(1); enable(0); flush(); assert(count == 2);
    enable(1); during_report = connection_during_report; flush();
    assert(!pthread_join(native_thread, NULL));
    assert(atomic_load(&native_entered) && state.link && count == 5);
    enable(0); flush();
    assert(state.enabled == 0 && !state.dirty && count == 6);
    check(5, (uint8_t[]){0x30,0,0x2d,0x10,0,0,0,0});
}
static void create_tx(void)
{
    uint8_t data[25] = {0x60,0,0x30,0};
    memset(data + 4, 0xa5, sizeof(data) - 4);
    command(0x200d, data, sizeof(data));
    /* No address/filter/connection parameters retained beyond first4 bytes. */
    assert(pending[CREATE].data[4] == 0 && pending[CREATE].data[5] == 0 && pending[CREATE].data[6] == 0);
}
static void create(void) { create_tx(); status(0x200d, 0); }
static void connection(uint8_t result, bool enhanced, uint16_t interval)
{
    uint8_t packet[33] = {0x3e, enhanced ? 31 : 19, enhanced ? 0x0a : 1, result, 0x34,0x12,0};
    unsigned int offset = enhanced ? 26 : 14;
    packet[offset] = (uint8_t)interval; packet[offset + 1] = (uint8_t)(interval >> 8);
    receive(4, packet, enhanced ? 33 : 21);
}
static void initiating(void)
{
    create_tx(); flush(); assert(count == 0 && !state.initiating);
    complete(0x200d, 0); flush(); assert(count == 0); /* only Command Status accepts Create */
    status(0x200d, 0); flush(); assert(count == 4 && state.initiating && state.setup);
    check(0, (uint8_t[]){0x49,6,0x30,0,0x60,0,0,0});
    check(1, (uint8_t[]){0x30,0,0x2d,0x10,2,0,0,0});
    check(2, (uint8_t[]){0x46,6,0,0,0,4,0,0});
    check(3, (uint8_t[]){0x27,6,0,0,0,0x20,0,0});
    command(0x200e, NULL, 0); status(0x200e, 0); flush(); assert(count == 4 && state.initiating);
    complete(0x200e, 0x0c); flush(); assert(count == 4 && state.initiating);
    command(0x200e, NULL, 0); complete(0x200e, 0); flush();
    assert(count == 7 && !state.initiating && !state.setup && state.awaiting_terminal);
    check(4, (uint8_t[]){0x30,0,0x2d,0x10,0,0,0,0});
    check(5, (uint8_t[]){0x46,6,0,0,0,0,0,0});
    connection(2, false, 0); flush();
    assert(!state.awaiting_terminal && !state.link && state.connection_status == 2);
    create_tx(); status(0x200d, 0x0c); flush();
    assert(!state.initiating && state.create_status == 0x0c);
}
static void setup_grace(void)
{
    /* Preserve all native extra-info fields; overlay setup bit only. */
    uint8_t extra[] = {0xff,9,0x46,0,6,0xa1,0xb2,0xc3,0x91,0xe5,0xf6};
    receive(4, extra, sizeof(extra));
    create(); flush();
    check(2, (uint8_t[]){0x46,6,0xa1,0xb2,0xc3,0x95,0xe5,0xf6});
    now = 1000; connection(0, true, 24); flush();
    assert(state.link && !state.initiating && state.setup && state.setup_deadline == 1223);
    assert(count == 6); /* scan-none, extra-info; no synthetic connected BT-info */
    now = 1222; flush(); assert(count == 6);
    now = 1223; flush(); assert(count == 8 && !state.setup && !state.setup_owned);
    check(7, (uint8_t[]){0x46,6,0xa1,0xb2,0xc3,0x91,0xe5,0xf6});
    /* Native setup changes after ownership ends must not be overridden. */
    extra[8] = 0x95; receive(4, extra, sizeof(extra)); flush(); assert(count == 8);
}
static void connection_cancel_races(void)
{
    create(); flush(); command(0x200e, NULL, 0);
    connection(0, false, 24); flush(); int64_t deadline = state.setup_deadline;
    unsigned int before = count;
    complete(0x200e, 0); flush();
    assert(state.link && state.setup && state.setup_deadline == deadline && count == before);
    command(0x0c03, NULL, 0); complete(0x0c03, 0); flush();
    create(); flush(); command(0x200e, NULL, 0); complete(0x200e, 0); flush();
    assert(!state.setup_owned && state.awaiting_terminal);
    connection(0, false, 24); flush();
    assert(state.link && state.setup && state.setup_owned && state.setup_deadline > now);
    now = state.setup_deadline; flush(); assert(!state.setup);
}
static void stale_attempt(void)
{
    create(); flush(); command(0x200e, NULL, 0);
    connection(2, false, 0); flush();
    create(); flush(); unsigned int before = count;
    complete(0x200e, 0); flush(); /* completion belongs to previous attempt */
    assert(state.initiating && state.setup && count == before);
    connection(0, false, 3200); flush();
    assert(state.setup_deadline == now + 1000); /* wide formula then cap */
    int64_t old_deadline = state.setup_deadline;
    command(0x0c03, NULL, 0); complete(0x0c03, 0); flush();
    create(); flush(); before = count;
    now = old_deadline + 1; flush();
    assert(state.initiating && state.setup && state.setup_deadline == 0 && count == before);
    char json[512];
    assert(gateway_coex_scan_diagnostics(json, sizeof(json)) > 0);
    assert(strstr(json, "\"initiating\":true") && strstr(json, "\"connection\":[2,0]"));
}
static void initiating_short_frames(void)
{
    uint8_t create_packet[28] = {0x0d,0x20,25,0x60,0,0x30,0};
    uint8_t result[33] = {0x3e,31,0x0a,0,0x34,0x12,0};
    for (size_t n = 0; n < sizeof(create_packet); ++n) {
        uint8_t *packet = n ? malloc(n) : NULL;
        if (n) memcpy(packet, create_packet, n);
        gateway_coex_scan_tx(1, packet, n); free(packet);
    }
    assert(!pending[CREATE].present);
    create(); flush(); unsigned int before = count;
    for (size_t n = 0; n < sizeof(result); ++n) {
        uint8_t *packet = n ? malloc(n) : NULL;
        if (n) memcpy(packet, result, n);
        receive(4, packet, n); free(packet);
    }
    flush(); assert(state.initiating && count == before && state.connection_count == 0);
    result[1] = 19; receive(4, result, 21); /* enhanced event cannot use legacy length */
    assert(state.initiating && state.connection_count == 0);
    connection(0x3e, true, 0); flush();
    assert(!state.initiating && !state.setup && !state.awaiting_terminal && !state.link);
}
static void normal_resume(void)
{
    parameters(); create(); flush(); command(0x200e, NULL, 0);
    connection(2, false, 0); flush();
    enable(1); flush(); unsigned int before = count;
    complete(0x200e, 0); flush();
    assert(state.enabled == 1 && !state.initiating && !state.setup && count == before);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "basic")) basic();
    else if (!strcmp(argv[1], "failures")) failures();
    else if (!strcmp(argv[1], "malformed")) malformed();
    else if (!strcmp(argv[1], "native")) native();
    else if (!strcmp(argv[1], "convergence")) convergence();
    else if (!strcmp(argv[1], "initiating")) initiating();
    else if (!strcmp(argv[1], "setup_grace")) setup_grace();
    else if (!strcmp(argv[1], "connection_cancel_races")) connection_cancel_races();
    else if (!strcmp(argv[1], "stale_attempt")) stale_attempt();
    else if (!strcmp(argv[1], "initiating_short_frames")) initiating_short_frames();
    else if (!strcmp(argv[1], "normal_resume")) normal_resume();
    else assert(0);
    assert(!locked && !worker);
    puts(argv[1]);
}
"""

class AmebaCoexScanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if not compiler:
            raise unittest.SkipTest("A C compiler is required")
        temporary = tempfile.TemporaryDirectory(prefix="ameba-coex-scan-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        header = directory / "zephyr" / "kernel.h"
        header.parent.mkdir(parents=True)
        header.write_text(KERNEL)
        harness = directory / "harness.c"
        harness.write_text(HARNESS)
        cls.executable = directory / "test"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-pthread",
             "-I", str(directory), "-I", str(SOURCE.parent), str(harness), "-o", str(cls.executable)],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), name)

    def test_only_successful_complete_emits_exact_vendor_mailboxes(self): self.run_case("basic")
    def test_failures_ambiguous_opcodes_and_successful_reset(self): self.run_case("failures")
    def test_short_frames_invalid_values_and_bounded_diagnostics(self): self.run_case("malformed")
    def test_native_profiles_preserved_and_connected_link_protected(self): self.run_case("native")
    def test_coalesced_updates_and_update_during_reporting_converge(self): self.run_case("convergence")
    def test_initiating_acceptance_and_cancel_use_correct_completion_types(self): self.run_case("initiating")
    def test_native_extra_fields_preserved_through_bounded_setup_grace(self): self.run_case("setup_grace")
    def test_connection_success_and_cancel_completion_races(self): self.run_case("connection_cancel_races")
    def test_old_cancel_and_grace_cannot_clear_a_new_attempt(self): self.run_case("stale_attempt")
    def test_initiating_short_frames_and_failure_completion(self): self.run_case("initiating_short_frames")
    def test_delayed_cancel_does_not_clear_resumed_normal_scan(self): self.run_case("normal_resume")


if __name__ == "__main__":
    unittest.main()
