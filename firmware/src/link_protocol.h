/* Copyright 2026, FreeTouch */
#ifndef CV1_DM_PROTOCOL_H
#define CV1_DM_PROTOCOL_H
#include <stdint.h>

/* Independent implementation of hmd_rf 0x33f8 + 0x2214. No vendor code. */
static inline void dm_clear(uint8_t *p) { unsigned i; for (i=0;i<64;i++) p[i]=0; }
static inline void dm_connect(uint8_t *p) { dm_clear(p); p[0]=0x21; }
static inline void dm_config(uint8_t *p, uint32_t peer, const uint8_t *addr,
                             const uint8_t *key, uint16_t slot, uint8_t seq)
{
    unsigned i;
    dm_clear(p); p[0]=29; p[1]=seq; p[2]=0x26;
    for(i=0;i<4;i++) p[3+i]=(uint8_t)(peer>>(8*i));
    for(i=0;i<5;i++) p[7+i]=addr[i];
    p[12]=(uint8_t)slot; p[13]=(uint8_t)(slot>>8);
    for(i=0;i<16;i++) p[14+i]=key[i];
}
static inline void dm_finish(uint8_t *p, uint8_t seq)
{ dm_clear(p); p[0]=29; p[1]=seq; p[2]=0x42; }
static inline int dm_ack(const uint8_t *p, uint8_t opcode, uint8_t seq,
                          uint32_t crc, uint32_t ap)
{
    return crc && ap==1 && p[0]>=2 && p[0]<=32 &&
           (p[1]&1)==(seq&1u) && (p[2]>>1)==opcode;
}
#endif
