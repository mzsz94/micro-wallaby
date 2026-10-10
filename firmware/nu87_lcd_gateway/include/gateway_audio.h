/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_GATEWAY_AUDIO_H
#define WALLABY_GATEWAY_AUDIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GATEWAY_AUDIO_FRAME_SIZE 176U
#define GATEWAY_AUDIO_RING_FRAMES 64U
#define GATEWAY_AUDIO_DRAIN_FRAMES 32U
#define GATEWAY_AUDIO_MIN_MTU 179U
#define GATEWAY_AUDIO_CTRL_UUID_VAL BT_UUID_128_ENCODE(0x90e2c003, 0x3c0d, 0x4ef3, 0x9d78, 0x574cd1a64087)
#define GATEWAY_AUDIO_DATA_UUID_VAL BT_UUID_128_ENCODE(0x90e2c004, 0x3c0d, 0x4ef3, 0x9d78, 0x574cd1a64087)

/* Caller supplies synchronization. Independent ADPCM frames never inherit a
 * predictor from a lost frame. No partial/truncated notifications are queued. */
struct gateway_audio_ring {
 uint8_t frames[GATEWAY_AUDIO_RING_FRAMES][GATEWAY_AUDIO_FRAME_SIZE];
 uint32_t session, next_sequence, received, dropped;
 size_t head, queued;
 bool available, running;
 int error;
};

static inline uint32_t gateway_audio_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void gateway_audio_add(uint32_t *value, uint32_t amount)
{ *value = UINT32_MAX - *value < amount ? UINT32_MAX : *value + amount; }

static inline bool gateway_audio_frame_valid(const uint8_t *p, size_t size)
{
 return p != NULL && size == GATEWAY_AUDIO_FRAME_SIZE && p[0] == 'W' && p[1] == 'A' &&
  p[2] == 1 && p[3] == 0 && p[14] <= 88 && p[15] == 0 && (p[175] & 0xf0) == 0;
}

static inline void gateway_audio_reset(struct gateway_audio_ring *r, uint32_t session)
{
 r->session = session; r->next_sequence = 0; r->received = r->dropped = 0;
 r->head = r->queued = 0; r->running = true; r->error = 0;
}

static inline bool gateway_audio_push(struct gateway_audio_ring *r, const uint8_t *p, size_t size)
{
 if (!r->running) { return false; }
 if (!gateway_audio_frame_valid(p, size) || gateway_audio_u32(p + 4) != r->session) {
  gateway_audio_add(&r->dropped, 1); return false;
 }
 uint32_t sequence = gateway_audio_u32(p + 8);
 /* Sessions are bounded; never let sequence wrap re-admit stale data. */
 if (sequence < r->next_sequence || sequence == UINT32_MAX) {
  gateway_audio_add(&r->dropped, 1); return false;
 }
 gateway_audio_add(&r->dropped, sequence - r->next_sequence);
 r->next_sequence = sequence + 1;
 gateway_audio_add(&r->received, 1);
 if (r->queued == GATEWAY_AUDIO_RING_FRAMES) {
  r->head = (r->head + 1) % GATEWAY_AUDIO_RING_FRAMES;
  r->queued--; gateway_audio_add(&r->dropped, 1);
 }
 memcpy(r->frames[(r->head + r->queued) % GATEWAY_AUDIO_RING_FRAMES], p, size);
 r->queued++;
 return true;
}

static inline size_t gateway_audio_pop(struct gateway_audio_ring *r, uint8_t *out, size_t capacity)
{
 size_t count = capacity / GATEWAY_AUDIO_FRAME_SIZE;
 if (count > GATEWAY_AUDIO_DRAIN_FRAMES) { count = GATEWAY_AUDIO_DRAIN_FRAMES; }
 if (count > r->queued) { count = r->queued; }
 if (out == NULL) { return 0; }
 for (size_t i = 0; i < count; i++) {
  memcpy(out + i * GATEWAY_AUDIO_FRAME_SIZE, r->frames[r->head], GATEWAY_AUDIO_FRAME_SIZE);
  r->head = (r->head + 1) % GATEWAY_AUDIO_RING_FRAMES;
 }
 r->queued -= count;
 return count * GATEWAY_AUDIO_FRAME_SIZE;
}

int gateway_audio_status(char *json, size_t capacity);
int gateway_audio_start(uint32_t session);
int gateway_audio_stop(void);
int gateway_audio_pause(void);
bool gateway_audio_available(void);
size_t gateway_audio_chunk(uint8_t *data, size_t capacity);
#endif
