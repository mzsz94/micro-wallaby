/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../src/hci_transport_diagnostics.c"

static const uint8_t *input;
static size_t input_length, input_at;
static bool bad_read;
static uint32_t status_value;
static unsigned status_calls;

uint16_t __real_hci_uart_read(uint8_t *buffer, uint16_t length)
{
	assert(transport_lock_held == 1); /* Read and parser must share the lock. */
	if (bad_read) { return length + 1; }
	size_t take = length < input_length - input_at ? length : input_length - input_at;
	if (take != 0) { memcpy(buffer, input + input_at, take); }
	input_at += take;
	return (uint16_t)take;
}

uint32_t __real_UART_LineStatusGet(void *uart)
{
	assert(transport_lock_held == ((uintptr_t)uart == AMEBA_HCI_UART_ADDRESS));
	status_calls++;
	return status_value;
}

static void reset_stream(const uint8_t *data, size_t length)
{
	memset(&stream, 0, sizeof(stream));
	input = data; input_length = length; input_at = 0;
	bad_read = false;
	status_value = status_calls = 0;
}

static void consume(size_t chunk)
{
	uint8_t buffer[80];
	assert(chunk != 0 && chunk <= sizeof(buffer));
	while (input_at < input_length) {
		size_t at = input_at;
		uint16_t count = __wrap_hci_uart_read(buffer, (uint16_t)chunk);
		assert(count != 0 && count <= chunk);
		assert(count == input_at - at && memcmp(buffer, input + at, count) == 0);
	}
}

static void test_fragmentation_and_payload_ignored(void)
{
	const uint8_t packets[] = {
		4, 0x0e, 4, 1, 0x0c, 0x20, 0, /* command complete */
		4, 0x3e, 5, 2, 4, 0x3e, 2, 0xff, /* LE ADV, H4-like body */
		4, 0x3e, 2, 0x0d, 0, /* LE extended ADV */
		2, 0x01, 0, 5, 0, 4, 0x3e, 2, 0xff, 0, /* ACL */
		3, 0x02, 0, 2, 4, 0x3e, /* SCO */
		5, 0x03, 0, 3, 0xc0, 4, 0x3e, 2, /* ISO 14-bit length */
		1, 0x0c, 0x20, 0, /* zero-payload command */
	};
	for (size_t chunk = 1; chunk <= sizeof(packets); chunk++) {
		reset_stream(packets, sizeof(packets));
		consume(chunk);
		assert(stream.bytes == sizeof(packets) && stream.frames == 7);
		assert(stream.event_headers == 3 && stream.le_headers == 2);
		assert(stream.adv_headers == 2 && stream.adv_frames == 2);
		assert(stream.unknown_bytes == 0 && stream.invalid_lengths == 0);
		assert(stream.phase == STREAM_TYPE && stream.remaining == 0);
	}
}

static void test_truncated_unknown_and_large_frames(void)
{
	const uint8_t partial[] = {0, 0xff, 6, 4, 0x3e, 4, 2, 0x04};
	reset_stream(partial, sizeof(partial));
	consume(1);
	assert(stream.unknown_bytes == 3 && stream.frames == 0);
	assert(stream.adv_headers == 1 && stream.adv_frames == 0);
	assert(stream.phase == STREAM_BODY && stream.remaining == 2);
	const uint8_t tail[] = {0x3e, 0x02, 4, 0x3e, 0};
	input = tail; input_length = sizeof(tail); input_at = 0;
	consume(3);
	assert(stream.frames == 2 && stream.adv_frames == 1);
	assert(stream.invalid_lengths == 1 && stream.unknown_bytes == 3);
	/* An incomplete header is retained, not misidentified as a fresh type. */
	const uint8_t header[] = {2, 0, 0, 0xff};
	reset_stream(header, sizeof(header)); consume(2);
	assert(stream.phase == STREAM_HEADER && stream.remaining == 1);
	uint8_t *large = malloc(1 + UINT16_MAX + 4);
	assert(large != NULL);
	large[0] = 0xff;
	memset(large + 1, 4, UINT16_MAX); /* body must not create event headers */
	const uint8_t ending[] = {4, 0x3e, 1, 2};
	memcpy(large + 1 + UINT16_MAX, ending, sizeof(ending));
	input = large; input_length = 1 + UINT16_MAX + sizeof(ending); input_at = 0;
	consume(79);
	assert(stream.frames == 2 && stream.event_headers == 1);
	assert(stream.adv_frames == 1 && stream.unknown_bytes == 0);
	assert(stream.bytes == sizeof(header) + input_length);
	free(large);
}

static void test_passthrough_and_status(void)
{
	reset_stream(NULL, 0);
	assert(__wrap_hci_uart_read(NULL, 0) == 0);
	uint8_t guard = 0x5a;
	bad_read = true;
	assert(__wrap_hci_uart_read(&guard, 1) == 2);
	assert(guard == 0x5a && stream.read_errors == 1 && stream.bytes == 0);
	assert(__wrap_hci_uart_read(NULL, 0) == 1 && stream.read_errors == 2);
	status_value = UINT32_MAX;
	assert(__wrap_UART_LineStatusGet((void *)(uintptr_t)0x40008000) == status_value);
	assert(status_calls == 1 && stream.overruns == 0);
	assert(__wrap_UART_LineStatusGet((void *)(uintptr_t)AMEBA_HCI_UART_ADDRESS) == status_value);
	assert(status_calls == 2 && stream.overruns == 1 && stream.parity == 1);
	assert(stream.framing == 1 && stream.breaks == 1);
	status_value = 0;
	assert(__wrap_UART_LineStatusGet((void *)(uintptr_t)AMEBA_HCI_UART_ADDRESS) == 0);
	assert(status_calls == 3 && stream.overruns == 1);
}

static unsigned long field(const char *json, const char *name)
{
	char label[40];
	assert(snprintf(label, sizeof(label), "\"%s\":", name) > 0);
	const char *value = strstr(json, label);
	assert(value != NULL);
	return strtoul(value + strlen(label), NULL, 10);
}

static atomic_uint readers_done;
static void *reader(void *argument)
{
	uint8_t buffer[31];
	uint16_t size = (uint16_t)(uintptr_t)argument;
	while (__wrap_hci_uart_read(buffer, size) != 0) {}
	atomic_fetch_add(&readers_done, 1);
	return NULL;
}

static void test_concurrent_reads_and_snapshots(void)
{
	enum { PACKETS = 40000, THREADS = 4 };
	uint8_t *packets = malloc(PACKETS * 5);
	assert(packets != NULL);
	for (unsigned i = 0; i < PACKETS; i++) {
		const uint8_t one[] = {4, 0x3e, 2, 2, (uint8_t)i};
		memcpy(packets + i * sizeof(one), one, sizeof(one));
	}
	reset_stream(packets, PACKETS * 5);
	atomic_store(&readers_done, 0);
	pthread_t readers[THREADS];
	for (uintptr_t i = 0; i < THREADS; i++) {
		assert(pthread_create(&readers[i], NULL, reader, (void *)(1 + i * 10)) == 0);
	}
	char json[512];
	do {
		assert(gateway_hci_transport_diagnostics(json, sizeof(json)) > 0);
		unsigned long bytes = field(json, "uartBytes"), frames = field(json, "frames");
		assert(bytes >= frames * 5 && bytes <= PACKETS * 5);
		assert(field(json, "advFrames") == frames);
		assert(field(json, "advHeaders") >= frames && field(json, "advHeaders") <= frames + 1);
		assert(field(json, "eventHeaders") >= field(json, "advHeaders"));
		assert(field(json, "unknownBytes") == 0 && field(json, "readErrors") == 0);
	} while (atomic_load(&readers_done) != THREADS);
	for (unsigned i = 0; i < THREADS; i++) { assert(pthread_join(readers[i], NULL) == 0); }
	assert(stream.bytes == PACKETS * 5 && stream.frames == PACKETS);
	assert(stream.adv_frames == PACKETS && stream.phase == STREAM_TYPE);
	free(packets);
}

static void test_bounded_snapshot_and_saturation(void)
{
	reset_stream(NULL, 0);
	stream.bytes = stream.frames = stream.event_headers = stream.le_headers = UINT32_MAX;
	stream.adv_headers = stream.adv_frames = stream.unknown_bytes = stream.invalid_lengths = UINT32_MAX;
	stream.read_errors = stream.overruns = stream.parity = stream.framing = stream.breaks = UINT32_MAX;
	const uint8_t packet[] = {0xff, 4, 0x3e, 1, 2};
	input = packet; input_length = sizeof(packet); consume(5);
	assert(stream.bytes == UINT32_MAX && stream.frames == UINT32_MAX && stream.unknown_bytes == UINT32_MAX);
	status_value = UINT32_MAX;
	(void)__wrap_UART_LineStatusGet((void *)(uintptr_t)AMEBA_HCI_UART_ADDRESS);
	assert(stream.overruns == UINT32_MAX);
	char json[512];
	int length = gateway_hci_transport_diagnostics(json, sizeof(json));
	assert(length > 0 && length < (int)sizeof(json) && length == (int)strlen(json));
	assert(strstr(json, "4294967295") != NULL);
	assert(gateway_hci_transport_diagnostics(json, (size_t)length + 1) == length);
	assert(gateway_hci_transport_diagnostics(json, (size_t)length) == -ENOSPC && json[0] == '\0');
	struct { unsigned char before; char text[1]; unsigned char after; } guard = {0x5a, {'x'}, 0xa5};
	assert(gateway_hci_transport_diagnostics(guard.text, sizeof(guard.text)) == -ENOSPC);
	assert(guard.before == 0x5a && guard.text[0] == '\0' && guard.after == 0xa5);
	assert(gateway_hci_transport_diagnostics(NULL, 512) == -EINVAL);
	assert(gateway_hci_transport_diagnostics(json, 0) == -EINVAL);
}

int main(void)
{
	test_fragmentation_and_payload_ignored();
	test_truncated_unknown_and_large_frames();
	test_passthrough_and_status();
	test_concurrent_reads_and_snapshots();
	test_bounded_snapshot_and_saturation();
	puts("PASS HCI UART diagnostics: fragmented/large/truncated streams, byte/status passthrough, concurrent read ordering, atomic bounded snapshots");
	return 0;
}
