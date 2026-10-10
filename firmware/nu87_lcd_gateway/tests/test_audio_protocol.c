/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "gateway_audio.h"
static struct gateway_audio_ring ring;
static uint8_t frame[176], out[176 * 64];
static void packet(uint32_t session, uint32_t sequence)
{
 memset(frame, 0, sizeof(frame)); frame[0] = 'W'; frame[1] = 'A'; frame[2] = 1;
 for (int i = 0; i < 4; i++) { frame[4+i] = session >> (i*8); frame[8+i] = sequence >> (i*8); }
}
int main(void)
{
 gateway_audio_reset(&ring, 17); packet(17,0);
 assert(gateway_audio_push(&ring, frame,176));
 assert(!gateway_audio_push(&ring, frame,176)); /* duplicate */
 packet(17,2); assert(gateway_audio_push(&ring,frame,176));
 assert(ring.received == 2 && ring.dropped == 2);
 packet(16,3); assert(!gateway_audio_push(&ring,frame,176)); /* old session */
 packet(17,3); assert(!gateway_audio_push(&ring,frame,175));
 frame[14]=89;assert(!gateway_audio_push(&ring,frame,176));frame[14]=0;
 frame[175]=0x10;assert(!gateway_audio_push(&ring,frame,176));frame[175]=0;
 assert(gateway_audio_pop(&ring,out,175)==0);
 assert(gateway_audio_pop(&ring,out,sizeof(out))==352);
 assert(gateway_audio_u32(out+8)==0 && gateway_audio_u32(out+176+8)==2);
 gateway_audio_reset(&ring, 18);
 for(uint32_t i=0;i<70;i++){packet(18,i);assert(gateway_audio_push(&ring,frame,176));}
 assert(ring.queued==64&&ring.received==70&&ring.dropped==6);
 assert(gateway_audio_pop(&ring,out,sizeof(out))==176*32);
 assert(gateway_audio_u32(out+8)==6&&gateway_audio_u32(out+31*176+8)==37);
 assert(gateway_audio_pop(&ring,out,sizeof(out))==176*32&&ring.queued==0);
 ring.running=false;packet(18,70);assert(!gateway_audio_push(&ring,frame,176));
 ring.dropped=UINT32_MAX-2;gateway_audio_add(&ring.dropped,9);assert(ring.dropped==UINT32_MAX);
 puts("PASS audio frame validation, stale session/sequence, gaps, bounded overflow and drain");
 return 0;
}
