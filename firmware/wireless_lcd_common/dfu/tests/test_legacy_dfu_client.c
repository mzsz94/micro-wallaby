#include "legacy_dfu_client.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    legacy_dfu_client_t client;
    legacy_dfu_image_t image;
    legacy_dfu_config_t config;
    legacy_dfu_transport_t transport;
    uint8_t bytes[64], last[20];
    size_t last_length;
    legacy_dfu_channel_t last_channel;
    legacy_dfu_io_result_t io_result;
    uint32_t now;
    unsigned writes, reads, closes;
    bool read_failure;
} fixture_t;

static bool read_image(void *context, uint32_t offset, uint8_t *dst, size_t length)
{
    fixture_t *f = context;
    ++f->reads;
    assert(offset <= sizeof(f->bytes) && length <= sizeof(f->bytes) - offset);
    if (f->read_failure) return false;
    memcpy(dst, f->bytes + offset, length);
    return true;
}

static legacy_dfu_io_result_t write_packet(void *context, legacy_dfu_channel_t channel,
                                           const uint8_t *bytes, size_t length)
{
    fixture_t *f = context;
    assert(!f->client.write_in_flight);
    assert(length <= sizeof(f->last));
    ++f->writes;
    memcpy(f->last, bytes, length);
    f->last_channel = channel;
    f->last_length = length;
    return f->io_result;
}

static void close_transport(void *context) { ++((fixture_t *)context)->closes; }

static void setup(fixture_t *f)
{
    unsigned i;
    memset(f, 0, sizeof(*f));
    for (i = 0; i < sizeof(f->bytes); ++i) f->bytes[i] = (uint8_t)i;
    /* nRF52840 SP 0x20040000, Thumb reset handler 0x26009. */
    memcpy(f->bytes, (uint8_t[]){0, 0, 4, 0x20, 9, 0x60, 2, 0}, 8);
    f->image.image_size = sizeof(f->bytes);
    f->image.image_address = LEGACY_DFU_APP_START;
    f->image.app_version = UINT32_C(0x01020304);
    f->image.sd_fwid = LEGACY_DFU_SD_FWID;
    /* Independent fixture: Adafruit nrfutil calc_crc16 = 0xee75. */
    f->image.image_crc16 = UINT16_C(0xEE75);
    legacy_dfu_default_config(&f->config);
    f->transport.context = f;
    f->transport.read = read_image;
    f->transport.write = write_packet;
    f->transport.close = close_transport;
    f->io_result = LEGACY_DFU_IO_ACCEPTED;
}

static void begin(fixture_t *f)
{
    assert(legacy_dfu_begin(&f->client, &f->transport, &f->image, &f->config, f->now));
    while (f->client.state == LEGACY_DFU_PREFLIGHT)
        legacy_dfu_poll(&f->client, ++f->now);
    if (f->client.state == LEGACY_DFU_START)
        assert(legacy_dfu_transport_ready(&f->client, ++f->now));
}

static void send(fixture_t *f, legacy_dfu_channel_t channel, const uint8_t *bytes, size_t length)
{
    unsigned previous = f->writes;
    legacy_dfu_poll(&f->client, ++f->now);
    assert(f->writes == previous + 1);
    assert(f->last_channel == channel && f->last_length == length);
    assert(memcmp(f->last, bytes, length) == 0);
    assert(f->client.write_in_flight);
}

static void done(fixture_t *f) { legacy_dfu_write_complete(&f->client, true, ++f->now); }
static void notify(fixture_t *f, const uint8_t *bytes, size_t length)
{
    legacy_dfu_notification(&f->client, bytes, length, ++f->now);
}
static void response(fixture_t *f, uint8_t op, uint8_t status)
{
    uint8_t bytes[] = {0x10, op, status};
    notify(f, bytes, sizeof(bytes));
}
static void receipt(fixture_t *f, uint32_t count)
{
    uint8_t bytes[] = {0x11, (uint8_t)count, (uint8_t)(count >> 8),
                       (uint8_t)(count >> 16), (uint8_t)(count >> 24)};
    notify(f, bytes, sizeof(bytes));
}
static void failed(fixture_t *f, legacy_dfu_error_t error)
{
    unsigned writes = f->writes;
    assert(f->client.state == LEGACY_DFU_FAILED && f->client.error == error);
    assert(legacy_dfu_terminal(&f->client));
    legacy_dfu_poll(&f->client, ++f->now);
    assert(f->writes == writes);
    assert(f->closes == 1);
}

static void start_wait(fixture_t *f)
{
    begin(f);
    send(f, LEGACY_DFU_CONTROL, (uint8_t[]){1, 4}, 2); done(f);
    send(f, LEGACY_DFU_PACKET, (uint8_t[]){0,0,0,0,0,0,0,0,64,0,0,0}, 12); done(f);
    assert(f->client.state == LEGACY_DFU_WAIT_START);
}

static void image_ready(fixture_t *f)
{
    start_wait(f);
    response(f, 1, 1);
    send(f, LEGACY_DFU_CONTROL, (uint8_t[]){2, 0}, 2); done(f);
    /* Captured from official Python Packet.generate_packet; not made by client. */
    send(f, LEGACY_DFU_PACKET,
         (uint8_t[]){0x52,0,0xFF,0xFF,4,3,2,1,1,0,0xB6,0,0x75,0xEE}, 14); done(f);
    send(f, LEGACY_DFU_CONTROL, (uint8_t[]){2, 1}, 2); done(f);
    response(f, 2, 1);
    send(f, LEGACY_DFU_CONTROL, (uint8_t[]){8, f->config.prn_packets, 0}, 3); done(f);
    /* Successful PRN configuration has no DFU notification. */
    send(f, LEGACY_DFU_CONTROL, (uint8_t[]){3}, 1); done(f);
    assert(f->client.state == LEGACY_DFU_SEND_IMAGE);
}

static void image_sent(fixture_t *f)
{
    uint32_t offset;
    for (offset = 0; offset < 64; offset += 20) {
        size_t length = offset == 60 ? 4 : 20;
        send(f, LEGACY_DFU_PACKET, f->bytes + offset, length);
        if (offset == 60) response(f, 3, 1); /* notification before local TX completion */
        done(f);
        if (f->client.state == LEGACY_DFU_WAIT_PRN) {
            unsigned writes = f->writes;
            legacy_dfu_poll(&f->client, ++f->now);
            assert(f->writes == writes); /* window truly stops packet writes */
            receipt(f, offset + 20);
        }
    }
    assert(f->client.state == LEGACY_DFU_VALIDATE);
    assert(f->client.sent_bytes == 64 && f->client.acknowledged_bytes == 64);
}

static void test_wire_and_prn_windows(void)
{
    uint8_t window;
    assert(legacy_dfu_crc16(0xFFFF, (const uint8_t *)"123456789", 9) == 0x29B1);
    for (window = 1; window <= 4; ++window) {
        fixture_t f;
        setup(&f); f.config.prn_packets = window;
        image_ready(&f);
        image_sent(&f);
        send(&f, LEGACY_DFU_CONTROL, (uint8_t[]){4}, 1);
        response(&f, 4, 1); done(&f); /* also cover control response race */
        send(&f, LEGACY_DFU_CONTROL, (uint8_t[]){5}, 1);
        assert(!legacy_dfu_cancel(&f.client));
        if (window != 2) done(&f); /* disconnect may precede local write callback */
        legacy_dfu_disconnected(&f.client, ++f.now);
        assert(f.client.state == LEGACY_DFU_REBOOT_OBSERVED && f.closes == 0);
    }
}

static void test_preflight(void)
{
    fixture_t f;
    unsigned i;
    const uint32_t sizes[] = {0, 4, 9, LEGACY_DFU_APP_MAX_SIZE + 4, UINT32_MAX};
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        setup(&f); f.image.image_size = sizes[i];
        assert(!legacy_dfu_begin(&f.client, &f.transport, &f.image, &f.config, 0));
        assert(f.client.error == LEGACY_DFU_ERROR_IMAGE_SIZE && f.writes == 0);
    }
    setup(&f); f.image.image_address = 0;
    assert(!legacy_dfu_begin(&f.client, &f.transport, &f.image, NULL, 0));
    setup(&f); f.image.sd_fwid = 0xFFFE;
    assert(!legacy_dfu_begin(&f.client, &f.transport, &f.image, NULL, 0));
    setup(&f); f.config.prn_packets = 0;
    assert(!legacy_dfu_begin(&f.client, &f.transport, &f.image, &f.config, 0));
    setup(&f); f.image.image_crc16 ^= 1; begin(&f);
    failed(&f, LEGACY_DFU_ERROR_IMAGE_CRC); assert(f.writes == 0);
    setup(&f); f.bytes[0] = 1; begin(&f);
    failed(&f, LEGACY_DFU_ERROR_IMAGE_VECTOR); assert(f.writes == 0);
    setup(&f); f.bytes[4] = 8; begin(&f);
    failed(&f, LEGACY_DFU_ERROR_IMAGE_VECTOR);
    setup(&f); f.read_failure = true; begin(&f);
    failed(&f, LEGACY_DFU_ERROR_READ);
    setup(&f); f.image.image_size = LEGACY_DFU_APP_MAX_SIZE;
    assert(legacy_dfu_begin(&f.client, &f.transport, &f.image, NULL, 0));
    assert(legacy_dfu_cancel(&f.client)); /* only boundary validation; no huge fixture reads */
}

static void test_remote_and_malformed(void)
{
    fixture_t f;
    uint8_t status;
    unsigned i;
    const uint8_t invalid[][5] = {
        {0x10,2,1,0,0}, {0x10,1,0,0,0}, {0x10,1,7,0,0},
        {0x11,0,0,0,0}, {0x99,1,1,0,0}, {0x10,1,1,0,0}
    };
    const size_t lengths[] = {3,3,3,5,3,5};
    for (status = 2; status <= 6; ++status) {
        setup(&f); start_wait(&f); response(&f, 1, status);
        failed(&f, LEGACY_DFU_ERROR_REMOTE);
        assert(f.client.remote_status == status && f.client.remote_opcode == 1);
    }
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        setup(&f); start_wait(&f); notify(&f, invalid[i], lengths[i]);
        failed(&f, LEGACY_DFU_ERROR_PROTOCOL);
    }
    setup(&f); start_wait(&f); notify(&f, NULL, 0); failed(&f, LEGACY_DFU_ERROR_PROTOCOL);
    setup(&f); start_wait(&f); response(&f, 1, 1); response(&f, 1, 1);
    failed(&f, LEGACY_DFU_ERROR_PROTOCOL); /* duplicate response */
    setup(&f); image_ready(&f); response(&f, 3, 1);
    failed(&f, LEGACY_DFU_ERROR_PROTOCOL); /* premature receive success */
    setup(&f); image_ready(&f); image_sent(&f);
    send(&f, LEGACY_DFU_CONTROL, (uint8_t[]){4}, 1); done(&f); response(&f, 4, 5);
    failed(&f, LEGACY_DFU_ERROR_REMOTE); assert(f.client.remote_status == 5);
    setup(&f); image_ready(&f);
    send(&f, LEGACY_DFU_PACKET, f.bytes, 20); receipt(&f, 20); receipt(&f, 20);
    failed(&f, LEGACY_DFU_ERROR_PROTOCOL); /* bounded notification buffer */
}

static void test_receipts_and_mutation(void)
{
    fixture_t f;
    unsigned i;
    const uint32_t invalid[] = {0, 16, 24, 64, UINT32_MAX};
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        setup(&f); image_ready(&f); send(&f, LEGACY_DFU_PACKET, f.bytes, 20); done(&f);
        receipt(&f, invalid[i]); failed(&f, LEGACY_DFU_ERROR_RECEIPT);
    }
    setup(&f); image_ready(&f); send(&f, LEGACY_DFU_PACKET, f.bytes, 20);
    receipt(&f, 20); done(&f); /* legitimate PRN before TX completion */
    assert(f.client.acknowledged_bytes == 20 && f.client.state == LEGACY_DFU_SEND_IMAGE);
    receipt(&f, 20); failed(&f, LEGACY_DFU_ERROR_PROTOCOL);
    setup(&f); f.config.prn_packets = 4; image_ready(&f); f.bytes[63] ^= 1;
    for (i = 0; i < 4; ++i) {
        send(&f, LEGACY_DFU_PACKET, f.bytes + i * 20, i == 3 ? 4 : 20); done(&f);
    }
    failed(&f, LEGACY_DFU_ERROR_IMAGE_CRC); /* source changed after preflight */
    setup(&f); image_ready(&f); f.read_failure = true;
    legacy_dfu_poll(&f.client, ++f.now); failed(&f, LEGACY_DFU_ERROR_READ);
}

static void test_timeouts_busy_and_transport(void)
{
    fixture_t f;
    unsigned writes, i;
    setup(&f); f.now = UINT32_MAX - 10; start_wait(&f); writes = f.writes;
    f.now += f.config.start_timeout_ms;
    legacy_dfu_poll(&f.client, f.now); failed(&f, LEGACY_DFU_ERROR_TIMEOUT);
    assert(f.writes == writes); /* accepted commands are never retransmitted */
    setup(&f); begin(&f); f.io_result = LEGACY_DFU_IO_BUSY;
    for (i = 0; i <= f.config.max_busy_retries; ++i) {
        f.now += f.config.busy_retry_delay_ms;
        legacy_dfu_poll(&f.client, f.now);
    }
    failed(&f, LEGACY_DFU_ERROR_BUSY_LIMIT);
    assert(f.writes == (unsigned)f.config.max_busy_retries + 1);
    setup(&f); begin(&f); f.io_result = LEGACY_DFU_IO_BUSY;
    legacy_dfu_poll(&f.client, ++f.now); writes = f.writes;
    legacy_dfu_poll(&f.client, ++f.now); assert(f.writes == writes);
    f.io_result = LEGACY_DFU_IO_ACCEPTED; f.now += f.config.busy_retry_delay_ms;
    send(&f, LEGACY_DFU_CONTROL, (uint8_t[]){1,4}, 2); done(&f);
    setup(&f); begin(&f); f.io_result = LEGACY_DFU_IO_ERROR;
    legacy_dfu_poll(&f.client, ++f.now); failed(&f, LEGACY_DFU_ERROR_TRANSPORT);
    setup(&f); begin(&f); send(&f, LEGACY_DFU_CONTROL, (uint8_t[]){1,4}, 2);
    legacy_dfu_write_complete(&f.client, false, ++f.now); failed(&f, LEGACY_DFU_ERROR_TRANSPORT);
    setup(&f); image_ready(&f); send(&f, LEGACY_DFU_PACKET, f.bytes, 20); done(&f);
    f.now += f.config.response_timeout_ms;
    legacy_dfu_poll(&f.client, f.now); failed(&f, LEGACY_DFU_ERROR_TIMEOUT);
    setup(&f); f.config.session_timeout_ms = 1;
    assert(legacy_dfu_begin(&f.client, &f.transport, &f.image, &f.config, 0));
    legacy_dfu_poll(&f.client, 1); failed(&f, LEGACY_DFU_ERROR_TIMEOUT);
}

static void test_disconnect_cancel_and_late_events(void)
{
    fixture_t f;
    setup(&f); image_ready(&f); legacy_dfu_disconnected(&f.client, ++f.now);
    failed(&f, LEGACY_DFU_ERROR_DISCONNECTED);
    setup(&f); image_ready(&f); send(&f, LEGACY_DFU_PACKET, f.bytes, 20);
    assert(legacy_dfu_cancel(&f.client));
    assert(f.client.state == LEGACY_DFU_CANCELLED && f.closes == 1);
    done(&f); response(&f, 3, 1); legacy_dfu_disconnected(&f.client, ++f.now);
    assert(f.client.state == LEGACY_DFU_CANCELLED && f.closes == 1);
    setup(&f); begin(&f); done(&f); failed(&f, LEGACY_DFU_ERROR_PROTOCOL);
}

static void test_preflight_before_connection(void)
{
    fixture_t f;
    setup(&f);
    assert(legacy_dfu_begin(&f.client, &f.transport, &f.image, NULL, 0));
    assert(!legacy_dfu_transport_ready(&f.client, 0));
    while (f.client.state == LEGACY_DFU_PREFLIGHT) legacy_dfu_poll(&f.client, ++f.now);
    assert(f.client.state == LEGACY_DFU_START && f.writes == 0);
    f.now += 60000;
    legacy_dfu_poll(&f.client, f.now);
    assert(f.client.state == LEGACY_DFU_START && f.writes == 0);
    assert(legacy_dfu_transport_ready(&f.client, f.now));
    assert(!legacy_dfu_transport_ready(&f.client, f.now));
    send(&f, LEGACY_DFU_CONTROL, (uint8_t[]){1,4}, 2);
    assert(legacy_dfu_cancel(&f.client));
}

int main(void)
{
    test_wire_and_prn_windows();
    test_preflight();
    test_remote_and_malformed();
    test_receipts_and_mutation();
    test_timeouts_busy_and_transport();
    test_disconnect_cancel_and_late_events();
    test_preflight_before_connection();
    puts("legacy BLE DFU host tests passed");
    return 0;
}
