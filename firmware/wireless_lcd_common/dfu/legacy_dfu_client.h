#ifndef NU40_LEGACY_DFU_CLIENT_H
#define NU40_LEGACY_DFU_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEGACY_DFU_APP_START UINT32_C(0x26000)
#define LEGACY_DFU_APP_END UINT32_C(0xDC000) /* Exclusive, NU40 linker limit. */
#define LEGACY_DFU_APP_MAX_SIZE (LEGACY_DFU_APP_END - LEGACY_DFU_APP_START)
#define LEGACY_DFU_SD_FWID UINT16_C(0x00B6) /* S140 6.1.1 only. */
#define LEGACY_DFU_REVISION UINT16_C(0x0008)
#define LEGACY_DFU_PACKET_SIZE 20u
#define LEGACY_DFU_SERVICE_UUID "00001530-1212-efde-1523-785feabcd123"
#define LEGACY_DFU_CONTROL_UUID "00001531-1212-efde-1523-785feabcd123"
#define LEGACY_DFU_PACKET_UUID "00001532-1212-efde-1523-785feabcd123"
#define LEGACY_DFU_REVISION_UUID "00001534-1212-efde-1523-785feabcd123"

typedef enum {
    LEGACY_DFU_CONTROL, /* GATT Write Request (with ATT response). */
    LEGACY_DFU_PACKET   /* GATT Write Command (without ATT response). */
} legacy_dfu_channel_t;

typedef enum {
    LEGACY_DFU_IO_ACCEPTED, /* Copied/queued; exactly one write_complete must follow. */
    LEGACY_DFU_IO_BUSY,     /* Nothing sent or queued: safe to retry. */
    LEGACY_DFU_IO_ERROR
} legacy_dfu_io_result_t;

typedef enum {
    LEGACY_DFU_IDLE, LEGACY_DFU_PREFLIGHT,
    LEGACY_DFU_START, LEGACY_DFU_SIZE, LEGACY_DFU_WAIT_START,
    LEGACY_DFU_INIT_START, LEGACY_DFU_INIT_DATA, LEGACY_DFU_INIT_END,
    LEGACY_DFU_WAIT_INIT, LEGACY_DFU_SET_PRN, LEGACY_DFU_RECEIVE,
    LEGACY_DFU_SEND_IMAGE, LEGACY_DFU_WAIT_PRN, LEGACY_DFU_WAIT_IMAGE,
    LEGACY_DFU_VALIDATE, LEGACY_DFU_WAIT_VALIDATE,
    LEGACY_DFU_ACTIVATE, LEGACY_DFU_WAIT_REBOOT,
    LEGACY_DFU_REBOOT_OBSERVED, /* Not proof that the new application is healthy. */
    LEGACY_DFU_FAILED, LEGACY_DFU_CANCELLED
} legacy_dfu_state_t;

typedef enum {
    LEGACY_DFU_ERROR_NONE, LEGACY_DFU_ERROR_ARGUMENT,
    LEGACY_DFU_ERROR_IMAGE_SIZE, LEGACY_DFU_ERROR_IMAGE_VECTOR,
    LEGACY_DFU_ERROR_IMAGE_CRC, LEGACY_DFU_ERROR_READ,
    LEGACY_DFU_ERROR_TRANSPORT, LEGACY_DFU_ERROR_BUSY_LIMIT,
    LEGACY_DFU_ERROR_TIMEOUT, LEGACY_DFU_ERROR_DISCONNECTED,
    LEGACY_DFU_ERROR_PROTOCOL, LEGACY_DFU_ERROR_RECEIPT,
    LEGACY_DFU_ERROR_REMOTE
} legacy_dfu_error_t;

typedef struct {
    uint32_t image_size;    /* Raw, immutable application binary, multiple of four. */
    uint32_t image_address; /* Must be APP_START; no HEX, UF2, ZIP, SD or bootloader. */
    uint32_t app_version;   /* Metadata only: this bootloader has no anti-downgrade check. */
    uint16_t image_crc16;   /* CRC-16/CCITT-FALSE over exactly image_size bytes. */
    uint16_t sd_fwid;       /* Must be 0x00B6. */
} legacy_dfu_image_t;

typedef struct {
    uint32_t start_timeout_ms;
    uint32_t response_timeout_ms;
    uint32_t session_timeout_ms;
    uint16_t busy_retry_delay_ms;
    uint8_t max_busy_retries; /* Retries only IO_BUSY, never an accepted write. */
    uint8_t prn_packets;      /* 1..4; default 1 is deliberately conservative. */
} legacy_dfu_config_t;

typedef struct {
    void *context;
    bool (*read)(void *context, uint32_t offset, uint8_t *dst, size_t length);
    legacy_dfu_io_result_t (*write)(void *context, legacy_dfu_channel_t channel,
                                    const uint8_t *data, size_t length);
    void (*close)(void *context); /* Stop queued work and disconnect on error/cancel. */
} legacy_dfu_transport_t;

/* Caller-owned fixed storage; fields are inspectable but must not be modified. */
typedef struct {
    legacy_dfu_state_t state;
    legacy_dfu_error_t error;
    uint8_t remote_opcode;
    uint8_t remote_status;
    uint32_t sent_bytes;
    uint32_t acknowledged_bytes;
    legacy_dfu_transport_t transport;
    legacy_dfu_image_t image;
    legacy_dfu_config_t config;
    uint32_t session_started_ms, state_started_ms, last_attempt_ms;
    uint32_t checked_bytes;
    uint16_t checked_crc, sent_crc;
    uint8_t packet[LEGACY_DFU_PACKET_SIZE];
    uint8_t notification[5];
    uint8_t packet_length, notification_length, busy_retries, window_packets;
    legacy_dfu_channel_t channel;
    bool write_in_flight, packet_prepared, activation_accepted, transport_verified;
} legacy_dfu_client_t;

void legacy_dfu_default_config(legacy_dfu_config_t *config);
uint16_t legacy_dfu_crc16(uint16_t crc, const uint8_t *bytes, size_t length);

/* Begin a fresh session only after previous transport events have been drained.
 * Preflight reads may run before entering the target's bootloader. No BLE writes
 * are possible until transport_ready; no read or write occurs until poll.
 * All functions/callbacks must be serialized on one task and are non-reentrant.
 * now_ms is a monotonic wrapping uint32_t millisecond counter. */
bool legacy_dfu_begin(legacy_dfu_client_t *client,
                      const legacy_dfu_transport_t *transport,
                      const legacy_dfu_image_t *image,
                      const legacy_dfu_config_t *config, uint32_t now_ms);
/* Once preflight reaches START, the adapter must verify target identity, legacy
 * service/revision 0x0008, and enabled control notifications, then call this.
 * Waiting for readiness consumes the session timeout, not the response timeout. */
bool legacy_dfu_transport_ready(legacy_dfu_client_t *client, uint32_t now_ms);
/* Bounded work: one 20-byte preflight read or at most one write per call. */
void legacy_dfu_poll(legacy_dfu_client_t *client, uint32_t now_ms);
/* Exactly once after ACCEPTED: ATT result for control, local TX completion for
 * packet. Do not call synchronously from write(). Queued BLE events must preserve
 * order; notification may precede write_complete and is buffered once. */
void legacy_dfu_write_complete(legacy_dfu_client_t *client, bool success, uint32_t now_ms);
void legacy_dfu_notification(legacy_dfu_client_t *client, const uint8_t *data,
                             size_t length, uint32_t now_ms);
void legacy_dfu_disconnected(legacy_dfu_client_t *client, uint32_t now_ms);
/* False after activation was accepted: cancellation cannot undo activation. */
bool legacy_dfu_cancel(legacy_dfu_client_t *client);
bool legacy_dfu_terminal(const legacy_dfu_client_t *client);

#ifdef __cplusplus
}
#endif
#endif
