/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

/* The Zephyr AmebaD port omits the host scan-state reports present in:
 * Ameba-AIoT/ameba-rtos@49e4ae8e7e760a843e75e48ade5e00bfff7e7419
 * component/bluetooth/rtk_coex/rtk_coex.{c,h}.
 * The pinned lib_coex.a accepts these exact eight-byte mailbox formats:
 * 0x30/0x2d: scan type; 0x30/0x2e: window then interval; 0x27: BT info;
 * 0x49: initiating window/interval; 0x46: connection setup phase.
 * Its rtk_coex_wl_chip_update_bt_mp_report and update_bt_info implementations
 * were checked against the vendor layouts. No antenna/PTA override is used.
 *
 * Observe HCI commands without altering them. Only successful Command Complete
 * events commit normal-scan state. Vendor notifications run on a dedicated
 * thread, serialized with the native H4 RX coexistence handler. TX observation
 * only updates bounded state; it never takes a mutex or calls vendor code.
 */
extern void rtk_coex_btc_bt_hci_notify(uint8_t *data, uint16_t length, uint8_t type);

enum { RESET, PARAMETERS, ENABLE, CREATE, CANCEL, COMMAND_COUNT };
struct pending_command {
	bool present, ambiguous;
	uint8_t data[7];
	uint32_t attempt;
};
struct scan_state {
	int enabled;
	bool parameters, link, dirty, native_link_seen;
	bool initiating, awaiting_terminal, setup, setup_owned, setup_dirty;
	uint16_t interval, window, handle;
	uint16_t init_interval, init_window;
	int64_t setup_deadline;
	uint8_t info[8], extra[8];
	uint32_t commits, rejected, batches, reports;
	uint32_t create_count, cancel_count, connection_count;
	uint32_t attempt;
	int create_status, cancel_status, connection_status;
};
static struct k_spinlock state_lock;
static struct pending_command pending[COMMAND_COUNT];
static struct scan_state state = {
	.enabled = -1, .handle = UINT16_MAX, .info = {0x27, 6}, .extra = {0x46, 6},
	.create_status = -1, .cancel_status = -1, .connection_status = -1,
};
K_SEM_DEFINE(scan_update, 0, 1);
/* Only the existing H4 RX thread and our reporter take this mutex. Original
 * RX coexistence processing already calls blocking vendor APIs. Serializing
 * it with the reporter closes the gap between checking link state and sending
 * BT-info, without holding an IRQ/spin lock across any vendor function.
 */
K_MUTEX_DEFINE(native_rx_guard);

void gateway_coex_scan_rx_begin(void)
{
	k_mutex_lock(&native_rx_guard, K_FOREVER);
}

void gateway_coex_scan_rx_end(void)
{
	k_mutex_unlock(&native_rx_guard);
}

static void increment(uint32_t *value)
{
	if (*value != UINT32_MAX) { ++*value; }
}

static int command_index(uint16_t opcode)
{
	switch (opcode) {
	case 0x0c03: return RESET;
	case 0x200b: return PARAMETERS;
	case 0x200c: return ENABLE;
	case 0x200d: return CREATE;
	case 0x200e: return CANCEL;
	default: return -1;
	}
}

static uint16_t read16(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

void gateway_coex_scan_tx(uint8_t type, const uint8_t *data, uint16_t length)
{
	if (type != 1 || data == NULL || length < 3 || data[2] != length - 3) { return; }
	int index = command_index(read16(data));
	static const uint8_t expected[COMMAND_COUNT] = {0, 7, 2, 25, 0};
	/* Keep only scan interval/window for Create Connection: no peer address. */
	static const uint8_t retained[COMMAND_COUNT] = {0, 7, 2, 4, 0};
	if (index < 0 || data[2] != expected[index]) { return; }
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	/* An overlapping opcode cannot be matched safely by HCI completion alone.
	 * Suppress that update instead of assigning the wrong command's values.
	 */
	if (pending[index].present) { pending[index].ambiguous = true; }
	pending[index].present = true;
	pending[index].attempt = state.attempt;
	memcpy(pending[index].data, data + 3, retained[index]);
	if (index == CREATE) { increment(&state.create_count); state.create_status = -1; }
	if (index == CANCEL) { increment(&state.cancel_count); state.cancel_status = -1; }
	k_spin_unlock(&state_lock, key);
}

static bool complete_command(int index, uint8_t status, bool complete)
{
	if (index < 0 || !pending[index].present) { return false; }
	/* Create Connection is asynchronous: Command Status accepts the procedure;
	 * its terminal result arrives as LE Connection Complete. Cancel is a
	 * synchronous command and must wait for Command Complete instead.
	 */
	if (index == CREATE && complete) { return false; }
	if (index != CREATE && !complete && status == 0) { return false; }
	if (index == CREATE) { state.create_status = status; }
	if (index == CANCEL) { state.cancel_status = status; }
	struct pending_command command = pending[index];
	memset(&pending[index], 0, sizeof(pending[index]));
	if (status != 0 || command.ambiguous) {
		increment(&state.rejected);
		return false;
	}
	bool changed = false;
	if (index == RESET) {
		memset(pending, 0, sizeof(pending));
		state.enabled = 0;
		state.parameters = false;
		state.interval = state.window = 0;
		state.link = false;
		state.native_link_seen = false;
		state.initiating = state.awaiting_terminal = false;
		state.setup_dirty = state.setup_owned;
		state.setup = false;
		state.setup_deadline = 0;
		state.init_interval = state.init_window = 0;
		state.handle = UINT16_MAX;
		memset(state.info, 0, sizeof(state.info));
		state.info[0] = 0x27;
		state.info[1] = 6;
		memset(state.extra, 0, sizeof(state.extra));
		state.extra[0] = 0x46;
		state.extra[1] = 6;
		changed = true;
	} else if (index == PARAMETERS) {
		uint16_t interval = read16(command.data + 1);
		uint16_t window = read16(command.data + 3);
		if (command.data[0] > 1 || interval < 4 || interval > 0x4000 ||
		    window < 4 || window > interval) {
			increment(&state.rejected);
			return false;
		}
		changed = !state.parameters || interval != state.interval || window != state.window;
		state.parameters = true;
		state.interval = interval;
		state.window = window;
	} else if (index == ENABLE) {
		if (command.data[0] > 1) { increment(&state.rejected); return false; }
		changed = state.enabled != command.data[0];
		state.enabled = command.data[0];
	} else if (index == CREATE) {
		uint16_t interval = read16(command.data);
		uint16_t window = read16(command.data + 2);
		if (interval < 4 || interval > 0x4000 || window < 4 || window > interval ||
		    state.awaiting_terminal || state.link) {
			increment(&state.rejected);
			return false;
		}
		state.enabled = 0;
		state.initiating = state.awaiting_terminal = true;
		++state.attempt;
		state.init_interval = interval;
		state.init_window = window;
		state.setup = state.setup_owned = state.setup_dirty = true;
		state.setup_deadline = 0;
		changed = true;
	} else if (index == CANCEL && state.awaiting_terminal && command.attempt == state.attempt) {
		/* A successful connection may have won the race before Cancel's
		 * response. In that case its post-connect grace must remain intact.
		 * Keep awaiting_terminal until the controller finishes the procedure.
		 */
		state.initiating = false;
		state.setup = false;
		state.setup_dirty = state.setup_owned;
		state.setup_deadline = 0;
		changed = true;
	}
	increment(&state.commits);
	return changed && state.enabled >= 0;
}

void gateway_coex_scan_rx(uint8_t type, const uint8_t *data, uint16_t length)
{
	if (type != 4 || data == NULL || length < 2 || data[1] != length - 2) { return; }
	bool changed = false;
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	if (data[0] == 0x0e && data[1] >= 4) {
		changed = complete_command(command_index(read16(data + 3)), data[5], true);
	} else if (data[0] == 0x0f && data[1] == 4) {
		changed = complete_command(command_index(read16(data + 4)), data[2], false);
	}
	/* Preserve the native controller's profile/link bytes when overlaying its
	 * scan flag. This helper does not invent connection profiles.
	 */
	const uint8_t *info = NULL;
	uint8_t mailbox = 0;
	if (data[0] == 0xff && data[1] == 9 && data[3] == 0) {
		mailbox = data[2];
		info = data + 4;
	} else if (data[0] == 0x0e && data[1] == 12 &&
		   read16(data + 3) == 0xfc8f && data[5] == 0) {
		mailbox = data[6];
		info = data + 7;
	}
	if (info != NULL && info[0] == 6) {
		if (mailbox == 0x27 || mailbox == 0x23) {
			memcpy(state.info + 1, info, 7);
			bool scanning = state.enabled == 1 || state.initiating;
			changed |= !state.native_link_seen && state.enabled >= 0 &&
				((info[4] >> 5) & 1) != scanning;
		} else if (mailbox == 0x46 || mailbox == 0x47) {
			memcpy(state.extra + 1, info, 7);
			if (state.setup_owned && ((info[4] >> 2) & 1) != state.setup) {
				state.setup_dirty = true;
				changed = true;
			}
		}
	}
	/* NU87 gateway supports one link. While it exists, leave the native BT-info
	 * path in charge; a normal-scan report must not clear its link flags.
	 */
	if (data[0] == 0x3e && ((data[1] == 19 && data[2] == 1) ||
				     (data[1] == 31 && data[2] == 0x0a))) {
		increment(&state.connection_count);
		state.connection_status = data[3];
		if (state.awaiting_terminal && (data[3] != 0 || data[6] == 0)) {
			state.awaiting_terminal = state.initiating = false;
			state.enabled = 0;
			state.setup = data[3] == 0;
			if (state.setup) { state.setup_owned = true; }
			state.setup_dirty = state.setup_owned;
			/* Vendor grace formula, interval in 1.25 ms units; at most 1s.
			 * A deadline in our worker avoids ISR timer/vendor callbacks.
			 */
			uint16_t interval = read16(data + (data[2] == 1 ? 14 : 26));
			uint32_t grace = interval ? (((uint32_t)interval * 7 + 11) * 5) / 4 : 0;
			if (grace > 1000) { grace = 1000; }
			if (state.setup && grace != 0) { state.setup_deadline = k_uptime_get() + grace; }
			else { state.setup = false; state.setup_deadline = 0; }
			changed = true;
		}
		if (data[3] == 0) {
			state.link = true;
			state.native_link_seen = true;
			state.handle = read16(data + 4);
		}
	} else if (data[0] == 5 && data[1] == 4 && data[2] == 0 &&
		   state.link && read16(data + 3) == state.handle) {
		state.link = false;
		state.handle = UINT16_MAX;
		if (state.setup_owned) {
			state.setup = false;
			state.setup_deadline = 0;
			state.setup_dirty = true;
			changed = true;
		}
	}
	if (changed) { state.dirty = true; }
	k_spin_unlock(&state_lock, key);
	if (changed) { k_sem_give(&scan_update); }
}

static bool report_pending(void)
{
	k_mutex_lock(&native_rx_guard, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	if (state.setup_deadline != 0 && k_uptime_get() >= state.setup_deadline) {
		state.setup = false;
		state.setup_deadline = 0;
		state.setup_dirty = state.dirty = true;
	}
	if (!state.dirty) {
		k_spin_unlock(&state_lock, key);
		k_mutex_unlock(&native_rx_guard);
		return false;
	}
	struct scan_state snapshot = state;
	state.dirty = false;
	state.setup_dirty = false;
	if (snapshot.setup_dirty && !snapshot.setup) { state.setup_owned = false; }
	k_spin_unlock(&state_lock, key);

	unsigned int reports = 0;
	if (snapshot.initiating) {
		/* Vendor leaves scan_start (byte6) zero; the pinned receiver uses
		 * window/interval and sets initiating bit1 itself. Do not invent bits.
		 */
		uint8_t initiation[8] = {0x49, 6,
			(uint8_t)snapshot.init_window, (uint8_t)(snapshot.init_window >> 8),
			(uint8_t)snapshot.init_interval, (uint8_t)(snapshot.init_interval >> 8), 0, 0};
		rtk_coex_btc_bt_hci_notify(initiation, sizeof(initiation), 3);
		++reports;
	}
	uint8_t bitmap = snapshot.initiating ? 2 : (snapshot.enabled == 1 ? 4 : 0);
	uint8_t scan_type[8] = {0x30, 0, 0x2d, 0x10, bitmap};
	rtk_coex_btc_bt_hci_notify(scan_type, sizeof(scan_type), 3);
	++reports;
	if (!snapshot.initiating && snapshot.enabled == 1 && snapshot.parameters) {
		uint8_t parameters[8] = {0x30, 0, 0x2e, 0x40,
			(uint8_t)snapshot.window, (uint8_t)(snapshot.window >> 8),
			(uint8_t)snapshot.interval, (uint8_t)(snapshot.interval >> 8)};
		rtk_coex_btc_bt_hci_notify(parameters, sizeof(parameters), 3);
		++reports;
	}
	if (snapshot.setup_dirty) {
		snapshot.extra[5] = (snapshot.extra[5] & ~0x04) | (snapshot.setup ? 0x04 : 0);
		rtk_coex_btc_bt_hci_notify(snapshot.extra, sizeof(snapshot.extra), 3);
		++reports;
	}
	/* After a link has existed, native reports may retain stale profile bits
	 * through disconnect. Leave BT-info entirely to that path until Reset;
	 * never reassert cached connection flags on a later discovery scan.
	 */
	if (!snapshot.native_link_seen) {
		snapshot.info[5] = (snapshot.info[5] & ~0x20) | (bitmap ? 0x20 : 0);
		rtk_coex_btc_bt_hci_notify(snapshot.info, sizeof(snapshot.info), 3);
		++reports;
	}
	key = k_spin_lock(&state_lock);
	increment(&state.batches);
	while (reports-- != 0) { increment(&state.reports); }
	k_spin_unlock(&state_lock, key);
	k_mutex_unlock(&native_rx_guard);
	return true;
}

static void scan_worker(void *a, void *b, void *c)
{
	(void)a; (void)b; (void)c;
	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		int64_t deadline = state.setup_deadline;
		k_spin_unlock(&state_lock, key);
		int64_t remaining = deadline - k_uptime_get();
		k_timeout_t timeout = deadline == 0 ? K_FOREVER : K_MSEC(remaining > 0 ? remaining : 0);
		k_sem_take(&scan_update, timeout);
		while (report_pending()) { }
	}
}
/* H4 RX is priority 10: let already queued native events run before reports. */
K_THREAD_DEFINE(coex_scan_thread, 4096, scan_worker, NULL, NULL, NULL, 11, 0, 0);

int gateway_coex_scan_diagnostics(char *json, size_t capacity)
{
	if (json == NULL || capacity == 0) { return -EINVAL; }
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	struct scan_state snapshot = state;
	k_spin_unlock(&state_lock, key);
	int length = snprintf(json, capacity,
		"{\"enabled\":%d,\"parameters\":%s,\"interval\":%u,\"window\":%u,"
		"\"link\":%s,\"native_link_seen\":%s,\"pending\":%s,\"commits\":%u,\"rejected\":%u,\"batches\":%u,\"reports\":%u,"
		"\"initiating\":%s,\"initInterval\":%u,\"initWindow\":%u,\"setup\":%s,"
		"\"create\":[%u,%d],\"cancel\":[%u,%d],\"connection\":[%u,%d]}",
		snapshot.enabled, snapshot.parameters ? "true" : "false",
		(unsigned int)snapshot.interval, (unsigned int)snapshot.window,
		snapshot.link ? "true" : "false", snapshot.native_link_seen ? "true" : "false",
		snapshot.dirty ? "true" : "false",
		(unsigned int)snapshot.commits, (unsigned int)snapshot.rejected,
		(unsigned int)snapshot.batches, (unsigned int)snapshot.reports,
		snapshot.initiating ? "true" : "false", (unsigned int)snapshot.init_interval,
		(unsigned int)snapshot.init_window, snapshot.setup ? "true" : "false",
		(unsigned int)snapshot.create_count, snapshot.create_status,
		(unsigned int)snapshot.cancel_count, snapshot.cancel_status,
		(unsigned int)snapshot.connection_count, snapshot.connection_status);
	if (length < 0 || (size_t)length >= capacity) { json[0] = '\0'; return -ENOSPC; }
	return length;
}
