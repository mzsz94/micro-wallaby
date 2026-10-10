#include "legacy_dfu_client.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t legacy_dfu_crc16(uint16_t crc, const uint8_t *bytes, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) {
        unsigned bit;
        crc ^= (uint16_t)((uint16_t)bytes[i] << 8);
        for (bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc & UINT16_C(0x8000)) != 0
                ? ((uint32_t)crc << 1) ^ UINT32_C(0x1021)
                : (uint32_t)crc << 1);
        }
    }
    return crc;
}

void legacy_dfu_default_config(legacy_dfu_config_t *config)
{
    if (config == NULL) return;
    config->start_timeout_ms = 60000;
    config->response_timeout_ms = 10000;
    config->session_timeout_ms = 1800000;
    config->busy_retry_delay_ms = 20;
    config->max_busy_retries = 16;
    config->prn_packets = 1;
}

bool legacy_dfu_terminal(const legacy_dfu_client_t *client)
{
    return client == NULL || client->state == LEGACY_DFU_IDLE ||
           client->state == LEGACY_DFU_FAILED || client->state == LEGACY_DFU_CANCELLED ||
           client->state == LEGACY_DFU_REBOOT_OBSERVED;
}

static void fail(legacy_dfu_client_t *client, legacy_dfu_error_t error)
{
    if (legacy_dfu_terminal(client)) return;
    client->state = LEGACY_DFU_FAILED;
    client->error = error;
    client->write_in_flight = false;
    client->packet_prepared = false;
    client->notification_length = 0;
    client->transport.close(client->transport.context);
}

static void enter(legacy_dfu_client_t *client, legacy_dfu_state_t state, uint32_t now)
{
    client->state = state;
    client->state_started_ms = now;
    client->packet_prepared = false;
    client->busy_retries = 0;
}

static bool config_valid(const legacy_dfu_config_t *config)
{
    return config->start_timeout_ms > 0 && config->start_timeout_ms < UINT32_C(0x80000000) &&
           config->response_timeout_ms > 0 && config->response_timeout_ms < UINT32_C(0x80000000) &&
           config->session_timeout_ms > 0 && config->session_timeout_ms < UINT32_C(0x80000000) &&
           config->busy_retry_delay_ms > 0 && config->max_busy_retries <= 32 &&
           config->prn_packets >= 1 && config->prn_packets <= 4;
}

bool legacy_dfu_begin(legacy_dfu_client_t *client,
                      const legacy_dfu_transport_t *transport,
                      const legacy_dfu_image_t *image,
                      const legacy_dfu_config_t *config, uint32_t now_ms)
{
    if (client == NULL) return false;
    memset(client, 0, sizeof(*client));
    client->state = LEGACY_DFU_FAILED;
    client->error = LEGACY_DFU_ERROR_ARGUMENT;
    if (transport == NULL || image == NULL || transport->read == NULL ||
        transport->write == NULL || transport->close == NULL) return false;
    client->transport = *transport;
    client->image = *image;
    if (config != NULL) client->config = *config;
    else legacy_dfu_default_config(&client->config);
    if (!config_valid(&client->config) || image->image_address != LEGACY_DFU_APP_START ||
        image->sd_fwid != LEGACY_DFU_SD_FWID) return false;
    if (image->image_size < 8 || image->image_size > LEGACY_DFU_APP_MAX_SIZE ||
        (image->image_size & 3u) != 0) {
        client->error = LEGACY_DFU_ERROR_IMAGE_SIZE;
        return false;
    }
    client->error = LEGACY_DFU_ERROR_NONE;
    client->checked_crc = UINT16_C(0xFFFF);
    client->sent_crc = UINT16_C(0xFFFF);
    client->session_started_ms = now_ms;
    enter(client, LEGACY_DFU_PREFLIGHT, now_ms);
    return true;
}

static bool expired(legacy_dfu_client_t *client, uint32_t now)
{
    uint32_t timeout = client->config.response_timeout_ms;
    if (legacy_dfu_terminal(client)) return true;
    if (client->state == LEGACY_DFU_WAIT_START || client->state == LEGACY_DFU_SIZE)
        timeout = client->config.start_timeout_ms;
    if ((uint32_t)(now - client->session_started_ms) >= client->config.session_timeout_ms ||
        (client->state != LEGACY_DFU_PREFLIGHT &&
         !(client->state == LEGACY_DFU_START && !client->transport_verified) &&
         (uint32_t)(now - client->state_started_ms) >= timeout)) {
        fail(client, LEGACY_DFU_ERROR_TIMEOUT);
        return true;
    }
    return false;
}

bool legacy_dfu_transport_ready(legacy_dfu_client_t *client, uint32_t now_ms)
{
    if (client == NULL || expired(client, now_ms) || client->state != LEGACY_DFU_START ||
        client->transport_verified) return false;
    client->transport_verified = true;
    client->state_started_ms = now_ms;
    return true;
}

static void preflight(legacy_dfu_client_t *client, uint32_t now)
{
    uint32_t remaining = client->image.image_size - client->checked_bytes;
    size_t length = remaining < LEGACY_DFU_PACKET_SIZE ? (size_t)remaining : LEGACY_DFU_PACKET_SIZE;
    if (!client->transport.read(client->transport.context, client->checked_bytes,
                                client->packet, length)) {
        fail(client, LEGACY_DFU_ERROR_READ);
        return;
    }
    if (client->checked_bytes == 0) {
        uint32_t sp = get32(client->packet);
        uint32_t reset = get32(client->packet + 4);
        if (sp <= UINT32_C(0x20000000) || sp > UINT32_C(0x20040000) || (sp & 7u) != 0 ||
            (reset & 1u) == 0 || (reset & ~UINT32_C(1)) < LEGACY_DFU_APP_START ||
            (reset & ~UINT32_C(1)) >= LEGACY_DFU_APP_START + client->image.image_size) {
            fail(client, LEGACY_DFU_ERROR_IMAGE_VECTOR);
            return;
        }
    }
    client->checked_crc = legacy_dfu_crc16(client->checked_crc, client->packet, length);
    client->checked_bytes += (uint32_t)length;
    if (client->checked_bytes == client->image.image_size) {
        if (client->checked_crc != client->image.image_crc16)
            fail(client, LEGACY_DFU_ERROR_IMAGE_CRC);
        else enter(client, LEGACY_DFU_START, now);
    }
}

static bool prepare(legacy_dfu_client_t *client)
{
    uint8_t *p = client->packet;
    client->channel = LEGACY_DFU_CONTROL;
    client->packet_length = 1;
    switch (client->state) {
    case LEGACY_DFU_START:
        p[0] = 0x01; p[1] = 0x04; client->packet_length = 2;
        break;
    case LEGACY_DFU_SIZE:
        client->channel = LEGACY_DFU_PACKET;
        memset(p, 0, 12);
        put32(p + 8, client->image.image_size);
        client->packet_length = 12;
        break;
    case LEGACY_DFU_INIT_START:
    case LEGACY_DFU_INIT_END:
        p[0] = 0x02; p[1] = client->state == LEGACY_DFU_INIT_END ? 1 : 0;
        client->packet_length = 2;
        break;
    case LEGACY_DFU_INIT_DATA:
        client->channel = LEGACY_DFU_PACKET;
        put16(p, UINT16_C(0x0052));
        put16(p + 2, UINT16_C(0xFFFF));
        put32(p + 4, client->image.app_version);
        put16(p + 8, 1);
        put16(p + 10, LEGACY_DFU_SD_FWID);
        put16(p + 12, client->image.image_crc16);
        client->packet_length = 14;
        break;
    case LEGACY_DFU_SET_PRN:
        p[0] = 0x08; put16(p + 1, client->config.prn_packets); client->packet_length = 3;
        break;
    case LEGACY_DFU_RECEIVE:
        p[0] = 0x03;
        break;
    case LEGACY_DFU_SEND_IMAGE: {
        uint32_t remaining = client->image.image_size - client->sent_bytes;
        client->channel = LEGACY_DFU_PACKET;
        client->packet_length = (uint8_t)(remaining < LEGACY_DFU_PACKET_SIZE ? remaining : LEGACY_DFU_PACKET_SIZE);
        if (!client->transport.read(client->transport.context, client->sent_bytes,
                                    p, client->packet_length)) {
            fail(client, LEGACY_DFU_ERROR_READ);
            return false;
        }
        break;
    }
    case LEGACY_DFU_VALIDATE: p[0] = 0x04; break;
    case LEGACY_DFU_ACTIVATE: p[0] = 0x05; break;
    default: return false;
    }
    client->packet_prepared = true;
    return true;
}

void legacy_dfu_poll(legacy_dfu_client_t *client, uint32_t now_ms)
{
    legacy_dfu_io_result_t result;
    if (client == NULL || expired(client, now_ms) || client->write_in_flight) return;
    if (client->state == LEGACY_DFU_PREFLIGHT) {
        preflight(client, now_ms);
        return;
    }
    if (!client->transport_verified) return;
    if (client->busy_retries != 0 &&
        (uint32_t)(now_ms - client->last_attempt_ms) < client->config.busy_retry_delay_ms) return;
    if (!client->packet_prepared && !prepare(client)) return;
    client->last_attempt_ms = now_ms;
    result = client->transport.write(client->transport.context, client->channel,
                                     client->packet, client->packet_length);
    if (result == LEGACY_DFU_IO_ACCEPTED) {
        client->write_in_flight = true;
        if (client->state == LEGACY_DFU_ACTIVATE) client->activation_accepted = true;
    } else if (result == LEGACY_DFU_IO_BUSY) {
        if (client->busy_retries >= client->config.max_busy_retries)
            fail(client, LEGACY_DFU_ERROR_BUSY_LIMIT);
        else ++client->busy_retries;
    } else fail(client, LEGACY_DFU_ERROR_TRANSPORT);
}

static uint8_t response_opcode(legacy_dfu_state_t state)
{
    switch (state) {
    case LEGACY_DFU_START: case LEGACY_DFU_SIZE: case LEGACY_DFU_WAIT_START: return 1;
    case LEGACY_DFU_INIT_START: case LEGACY_DFU_INIT_DATA:
    case LEGACY_DFU_INIT_END: case LEGACY_DFU_WAIT_INIT: return 2;
    case LEGACY_DFU_RECEIVE: case LEGACY_DFU_SEND_IMAGE:
    case LEGACY_DFU_WAIT_PRN: case LEGACY_DFU_WAIT_IMAGE: return 3;
    case LEGACY_DFU_VALIDATE: case LEGACY_DFU_WAIT_VALIDATE: return 4;
    case LEGACY_DFU_SET_PRN: return 8;
    default: return 0;
    }
}

void legacy_dfu_notification(legacy_dfu_client_t *client, const uint8_t *data,
                             size_t length, uint32_t now_ms)
{
    uint8_t opcode;
    if (client == NULL || expired(client, now_ms)) return;
    if (data == NULL || (length != 3 && length != 5)) {
        fail(client, LEGACY_DFU_ERROR_PROTOCOL);
        return;
    }
    /* Errors can arrive before local TX completion. Preserve the precise remote
     * status while the operation that provoked it is still the current state. */
    if (length == 3 && data[0] == 0x10 && data[2] >= 2 && data[2] <= 6 &&
        data[1] == response_opcode(client->state)) {
        client->remote_opcode = data[1];
        client->remote_status = data[2];
        fail(client, LEGACY_DFU_ERROR_REMOTE);
        return;
    }
    if (client->write_in_flight) {
        if (client->notification_length != 0) fail(client, LEGACY_DFU_ERROR_PROTOCOL);
        else {
            memcpy(client->notification, data, length);
            client->notification_length = (uint8_t)length;
        }
        return;
    }
    if (data[0] == 0x11 && length == 5) {
        if (client->state != LEGACY_DFU_WAIT_PRN) fail(client, LEGACY_DFU_ERROR_PROTOCOL);
        else if (get32(data + 1) != client->sent_bytes)
            fail(client, LEGACY_DFU_ERROR_RECEIPT);
        else {
            client->acknowledged_bytes = client->sent_bytes;
            client->window_packets = 0;
            enter(client, LEGACY_DFU_SEND_IMAGE, now_ms);
        }
        return;
    }
    opcode = response_opcode(client->state);
    if (length != 3 || data[0] != 0x10 || opcode == 0 || data[1] != opcode ||
        data[2] < 1 || data[2] > 6) {
        fail(client, LEGACY_DFU_ERROR_PROTOCOL);
        return;
    }
    if (data[2] != 1) {
        client->remote_opcode = data[1];
        client->remote_status = data[2];
        fail(client, LEGACY_DFU_ERROR_REMOTE);
        return;
    }
    switch (client->state) {
    case LEGACY_DFU_WAIT_START: enter(client, LEGACY_DFU_INIT_START, now_ms); break;
    case LEGACY_DFU_WAIT_INIT: enter(client, LEGACY_DFU_SET_PRN, now_ms); break;
    case LEGACY_DFU_WAIT_IMAGE:
        client->acknowledged_bytes = client->sent_bytes;
        enter(client, LEGACY_DFU_VALIDATE, now_ms);
        break;
    case LEGACY_DFU_WAIT_VALIDATE: enter(client, LEGACY_DFU_ACTIVATE, now_ms); break;
    default: fail(client, LEGACY_DFU_ERROR_PROTOCOL); break;
    }
}

void legacy_dfu_write_complete(legacy_dfu_client_t *client, bool success, uint32_t now_ms)
{
    uint8_t pending[5];
    uint8_t pending_length;
    if (client == NULL || expired(client, now_ms)) return;
    if (!client->write_in_flight) {
        fail(client, LEGACY_DFU_ERROR_PROTOCOL);
        return;
    }
    client->write_in_flight = false;
    if (!success) {
        fail(client, LEGACY_DFU_ERROR_TRANSPORT);
        return;
    }
    pending_length = client->notification_length;
    memcpy(pending, client->notification, pending_length);
    client->notification_length = 0;
    switch (client->state) {
    case LEGACY_DFU_START: enter(client, LEGACY_DFU_SIZE, now_ms); break;
    case LEGACY_DFU_SIZE: enter(client, LEGACY_DFU_WAIT_START, now_ms); break;
    case LEGACY_DFU_INIT_START: enter(client, LEGACY_DFU_INIT_DATA, now_ms); break;
    case LEGACY_DFU_INIT_DATA: enter(client, LEGACY_DFU_INIT_END, now_ms); break;
    case LEGACY_DFU_INIT_END: enter(client, LEGACY_DFU_WAIT_INIT, now_ms); break;
    case LEGACY_DFU_SET_PRN: enter(client, LEGACY_DFU_RECEIVE, now_ms); break;
    case LEGACY_DFU_RECEIVE: enter(client, LEGACY_DFU_SEND_IMAGE, now_ms); break;
    case LEGACY_DFU_SEND_IMAGE:
        client->sent_crc = legacy_dfu_crc16(client->sent_crc, client->packet, client->packet_length);
        client->sent_bytes += client->packet_length;
        ++client->window_packets;
        if (client->sent_bytes == client->image.image_size) {
            if (client->sent_crc != client->image.image_crc16)
                fail(client, LEGACY_DFU_ERROR_IMAGE_CRC);
            else enter(client, LEGACY_DFU_WAIT_IMAGE, now_ms);
        } else if (client->window_packets == client->config.prn_packets)
            enter(client, LEGACY_DFU_WAIT_PRN, now_ms);
        else enter(client, LEGACY_DFU_SEND_IMAGE, now_ms);
        break;
    case LEGACY_DFU_VALIDATE: enter(client, LEGACY_DFU_WAIT_VALIDATE, now_ms); break;
    case LEGACY_DFU_ACTIVATE: enter(client, LEGACY_DFU_WAIT_REBOOT, now_ms); break;
    default: fail(client, LEGACY_DFU_ERROR_PROTOCOL); break;
    }
    if (pending_length != 0)
        legacy_dfu_notification(client, pending, pending_length, now_ms);
}

void legacy_dfu_disconnected(legacy_dfu_client_t *client, uint32_t now_ms)
{
    if (client == NULL || expired(client, now_ms)) return;
    if (client->activation_accepted &&
        (client->state == LEGACY_DFU_ACTIVATE || client->state == LEGACY_DFU_WAIT_REBOOT)) {
        client->write_in_flight = false;
        client->notification_length = 0;
        enter(client, LEGACY_DFU_REBOOT_OBSERVED, now_ms);
    } else fail(client, LEGACY_DFU_ERROR_DISCONNECTED);
}

bool legacy_dfu_cancel(legacy_dfu_client_t *client)
{
    if (client == NULL || client->activation_accepted) return false;
    if (legacy_dfu_terminal(client)) return true;
    client->state = LEGACY_DFU_CANCELLED;
    client->write_in_flight = false;
    client->packet_prepared = false;
    client->notification_length = 0;
    client->transport.close(client->transport.context);
    return true;
}
