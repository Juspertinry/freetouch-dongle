/* Copyright 2026, FreeTouch */
/* FreeTouch dongle: a nice!nano standing in for the Rift CV1 headset radio, polling CV1 Touch every 2 ms
 * and passing their encrypted reports to the host over USB CDC (docs/PROTOCOL.md). */
#include <stddef.h>
#include "nrf52.h"
#include "link_protocol.h"
#include "tusb.h"
void tusb_hal_nrf_power_event(uint32_t event);

/* Reported as status code 5; bump on every release image. */
#ifndef DM_FW_REV
#define DM_FW_REV 0x0302u
#endif
#define LED (1u<<15)
#define GPIO_SET REG(0x50000508u)
#define GPIO_CLR REG(0x5000050cu)
#define GPIO_DIR REG(0x50000518u)
#define SLOT_US 2000u
/* Optional GPIO held high around each poll TX so a second radio can lock to it; P0.31 is free on the nice!nano. */
#ifndef DM_SYNC_PIN
#define DM_SYNC_PIN 0u
#endif
/* Exposure period every poll announces, us (headset sends 19200). 'X'+LE32 sets 5000..65535, the field is 16 bits. */
#ifndef DM_EXPOSURE_PERIOD_US
#define DM_EXPOSURE_PERIOD_US 19200u
#endif
static uint32_t exposure_period_us=DM_EXPOSURE_PERIOD_US;
#define TABLE_INDEX 24u             /* channel 54, same single-bit map method */
#define LINK_CHANNEL 54u

static uint32_t fault, crc, ap, rssi, tx_end, channel;
static uint8_t tx[64] __attribute__((aligned(4)));
static uint8_t rx[64] __attribute__((aligned(4)));
static int fast_rx;
static uint8_t link_sid, controller_sid;

/* Link identity (radio address + AES key), made from the RNG on first boot. It lives in the bootloader's
 * user-data page, which UF2 updates never write, so pairings survive firmware updates. */
#define IDENTITY_PAGE  0x000F3000u
#define IDENTITY_MAGIC 0x44495446u /* "FTID" */
struct identity { uint32_t magic, version; uint8_t address[8]; uint8_t key[16]; uint32_t check; };
_Static_assert(sizeof(struct identity)==36, "identity layout");
static uint8_t address[5], key[16];
/* Hop increment must match the controller's, which it never reports and which varies with identity (9 old lab, 13 from 0x0300).
 * Auto mode tries each until replies come at full rate (a wrong one gives ~1 in 37); 'H' 0..36 fixes it, 255 is auto. */
#define HOP_AUTO 0xffu
static volatile uint8_t hop_inc, hop_mode=HOP_AUTO, hop_learning;
static uint8_t hop_stored(void);
/* 'V' haptics (opcode 3): it never times out, so the host sends amp 0 to stop, and the dongle
 * sends it itself when the host lets go of the port. Unacknowledged, so it goes out on three polls. */
static volatile uint8_t haptic_repeat[2];
/* 'P' pairs the next controller for up to 60 s; status 6: 1 pairing, 2 paired, 3 failed, 4 timed out. */
static volatile uint8_t pair_requested;
static volatile int32_t exposure_phase_target_us;
static int32_t exposure_phase_us;
/* 'Q'+n reply slot for the next pairing: 1..3 = 1000/1250/1500 us, 4 = 750 us. 0 picks by hand,
 * which gives two same-hand Touch the same slot; the controller stores it, the dongle does not. */
static volatile uint8_t pair_slot;
/* 'U'+LE16 overrides the slot with a raw offset, since slots are only conventions.
 * Bounded so the reply ends before the RX window closes 60 us ahead of the next poll. */
static volatile uint16_t pair_us;
#define PAIR_US_MIN 400u
#define PAIR_US_MAX 1620u
#define SLOT_OFFSET_US(n) ((n)==4u?750u:750u+250u*(n))
/* Reply ends measured at 1313/1563 us for the 1000/1250 slots (~1813 for 1500); split halfway. */
static unsigned reply_slot(uint32_t end_us)
{
    return end_us<1188u?4u:end_us<1438u?1u:end_us<1688u?2u:3u;
}
static uint8_t haptic_sid[2], haptic_amp[2];
static uint16_t haptic_freq[2];
#define ALL_CHANNELS ((((uint64_t)1)<<37)-1u)
/* Unshifted 37-bit map shared with the controller. */
static uint64_t channel_map=ALL_CHANNELS;
static uint8_t channel_remap[37];
static int channel_map_ready;
static void set_channel_map(uint64_t mask)
{
    uint8_t used[37]; unsigned n=0,i;
    mask &= ALL_CHANNELS;
    for(i=0;i<37;i++) if(mask & ((uint64_t)1<<i)) used[n++]=(uint8_t)i;
    if(n<8) return; /* retain diversity; reject empty/accidentally short maps */
    for(i=0;i<37;i++) channel_remap[i]=(mask & ((uint64_t)1<<i))?(uint8_t)i:used[i%n];
    channel_map=mask; channel_map_ready=1;
}
/* Like the headset, drop channels that keep losing replies: every 5 s one under half the link's rate goes,
 * at least 20 stay, and a dropped one returns after 30 s. */
#define ADAPT_MIN_EXPECTED 40u
#define ADAPT_KEEP 20u
#define ADAPT_RETRY_POLLS 15000u
static uint64_t base_map=ALL_CHANNELS;
static uint32_t ch_expected[37], ch_got[37], ch_dropped_at[37];
static void adapt_channels(uint32_t polls)
{
    uint64_t map=channel_map, bit; uint32_t te=0, tg=0; unsigned i, n=0;
    for(i=0;i<37;i++) { te+=ch_expected[i]; tg+=ch_got[i]; }
    if(te>=8u*ADAPT_MIN_EXPECTED && tg) {
        for(i=0;i<37;i++) {
            bit=(uint64_t)1<<i;
            if(!(map&bit)) {
                if(polls-ch_dropped_at[i]>=ADAPT_RETRY_POLLS) map|=bit&base_map;
            } else if(ch_expected[i]>=ADAPT_MIN_EXPECTED &&
                      (uint64_t)ch_got[i]*2u*te<(uint64_t)tg*ch_expected[i]) {
                map&=~bit; ch_dropped_at[i]=polls;
            }
        }
        for(i=0;i<37;i++) if(map&((uint64_t)1<<i)) n++;
        if(n>=ADAPT_KEEP && map!=channel_map) set_channel_map(map);
    }
    for(i=0;i<37;i++) ch_expected[i]=ch_got[i]=0;
}
static uint32_t stream_sequence,stream_drops;
/* 1 for the 5 s repeat, 2 when a host opened the port or sent 'I'. */
static volatile uint8_t identity_due;
static void stream_status(uint32_t code,uint32_t a,uint32_t b);
static uint32_t hop_status(void);

static uint32_t now(void) { TIMER0_TASKS_CAPTURE0=1; return TIMER0_CC0; }
static int before(uint32_t deadline) { return (int32_t)(now()-deadline)<0; }
static void reboot(void);
/* 'K' poll layout bits (20-byte payload, byte 11 = 0, session byte 0) match a sniffed CV1 headset; 0 restores the 0x020A poll. */
static uint8_t poll_mode=3u;
/* 'A': 1 sends the active-timeout keep-alive (now, every 30 min, on rejoin); 0 lets controllers doze and sleep on their own. */
static volatile uint8_t keepalive_on=1u, keepalive_kick;
/* 'Z': PowerState(0) for 2 s; the Touch powers off about a minute after the last request and only a button wakes it.
 * The byte is ignored, kept so older hosts still parse. */
static volatile uint32_t sleep_seq;
#define SLEEP_SEQ_POLLS 1000u
/* 'L' IR LED pulse width, us. 35 is the Touch's own default, the closest known match to the headset's. */
static uint32_t ir_width_us=35u;
static volatile int ir_dirty;
/* 'R'+LE32 sends one FRReadConfigBlock (offset | length<<16); replies go to the host untouched. */
static volatile int read_pending;
static uint32_t read_arg;
static uint8_t read_op=0x0au; /* 'O'+byte from the host overrides it */
/* Read session byte: 0 = controller-reported, 0xff = link search candidate, else literal. */
static uint8_t read_sid;
static uint8_t read_b11=0xffu; /* 0xff preserves the poll tick */
static void stream_raw(uint32_t code,const uint8_t *data,unsigned len,uint32_t b);
static void stream_identity(void);
static void usb_service(void)
{
    static int map_pos=-1; static uint64_t map_arg;
    static int haptic_pos=-1; static uint8_t haptic_arg[4];
    static int arg_pos=-1, op_next, sid_next, b11_next, hop_next, sleep_next, slot_next, poll_next, keep_next; static uint32_t arg; static uint8_t cmd;
    static int us_pos=-1; static uint32_t us_arg;
    static int was_connected;
    tud_task();
    {
        int connected=tud_cdc_connected();
        if(connected && !was_connected) identity_due=2u;
        was_connected=connected;
    }
    {
        /* 1200 baud touch asks for the UF2 bootloader. */
        cdc_line_coding_t lc;
        tud_cdc_n_get_line_coding(0,&lc);
        if(lc.bit_rate==1200u) reboot();
    }
    while(tud_cdc_available()) {
        uint8_t c=(uint8_t)tud_cdc_read_char();
        if(haptic_pos>=0) {
            haptic_arg[haptic_pos]=c;
            if(++haptic_pos==4) {
                unsigned k=haptic_arg[0]&1u; /* sessions 2 and 3 */
                haptic_sid[k]=haptic_arg[0];
                haptic_amp[k]=haptic_arg[1];
                haptic_freq[k]=(uint16_t)(haptic_arg[2]|haptic_arg[3]<<8);
                haptic_repeat[k]=3;
                haptic_pos=-1;
            }
            continue;
        }
        if(map_pos>=0) {
            map_arg |= (uint64_t)c<<(8*map_pos);
            if(++map_pos==5) { base_map=map_arg&ALL_CHANNELS; set_channel_map(base_map); map_pos=-1; }
            continue;
        }
        if(op_next) { read_op=c; op_next=0; continue; }
        if(sid_next) { read_sid=c; sid_next=0; continue; }
        if(b11_next) { read_b11=c; b11_next=0; continue; }
        if(hop_next) {
            if(c==HOP_AUTO) { if(hop_mode!=HOP_AUTO) { hop_mode=HOP_AUTO; hop_inc=hop_stored(); } }
            else { hop_mode=(uint8_t)(c%37u); hop_inc=hop_mode; hop_learning=0; }
            hop_next=0; continue;
        }
        if(sleep_next) { (void)c; sleep_seq=SLEEP_SEQ_POLLS; sleep_next=0; continue; }
        if(slot_next) { pair_slot=c<=4u?c:0u; pair_us=0; slot_next=0; continue; }
        if(poll_next) { poll_mode=(uint8_t)(c&7u); poll_next=0; continue; }
        if(keep_next) { keepalive_on=c?1u:0u; if(c) keepalive_kick=1u; keep_next=0; continue; }
        if(us_pos>=0) {
            us_arg|=(uint32_t)c<<(8*us_pos);
            if(++us_pos==2) {
                pair_us=(uint16_t)(us_arg>=PAIR_US_MIN && us_arg<=PAIR_US_MAX ? us_arg : 0u);
                us_pos=-1;
            }
            continue;
        }
        if(arg_pos>=0) {
            arg|=(uint32_t)c<<(8*arg_pos);
            if(++arg_pos==4) {
                /* stay under the 19200 us camera period */
                if(cmd=='L' && arg>=2u && arg<=15000u && arg!=ir_width_us) { ir_width_us=arg; ir_dirty=2; }
                if(cmd=='R') { read_arg=arg; read_pending=1; }
                if(cmd=='E' && (int32_t)arg>=-33333 && (int32_t)arg<=33333)
                    exposure_phase_target_us=(int32_t)arg;
                if(cmd=='X' && arg>=5000u && arg<=65535u) exposure_period_us=arg;
                arg_pos=-1;
            }
            continue;
        }
        if(c=='b' || c=='B') reboot();
        if(c=='L' || c=='R' || c=='E' || c=='X') { cmd=c; arg_pos=0; arg=0; }
        if(c=='U') { us_pos=0; us_arg=0; }
        if(c=='I') identity_due=2u;
        if(c=='O') op_next=1;
        if(c=='S') sid_next=1;
        if(c=='Y') b11_next=1;
        if(c=='H') hop_next=1;
        if(c=='M') { map_pos=0; map_arg=0; }
        if(c=='V') haptic_pos=0;
        if(c=='Z') sleep_next=1;
        if(c=='P') pair_requested=1u;
        if(c=='Q') slot_next=1;
        if(c=='K') poll_next=1;
        if(c=='A') keep_next=1;
    }
    if(identity_due && tud_cdc_connected() && tud_cdc_write_available()>=3u*54u) {
        stream_identity();
        /* a new or asking host gets firmware and hopping now, not at the next 5 s batch */
        if(identity_due>1u) {
            stream_status(5,DM_FW_REV|((uint32_t)(exposure_period_us/100u)<<16),FICR_DEVICEID0);
            stream_status(8,(uint32_t)channel_map,hop_status());
        }
        identity_due=0;
    }
}
static void stream_input(const uint8_t *data,uint32_t poll_timestamp,unsigned slot)
{
    /* Payload is the encrypted 34-byte frame; decryption stays on the host.
     * Byte 17 carries the reply slot too, since two same-hand Touch share a sid. */
    uint8_t out[54]; unsigned i;
    uint32_t words[4]={0x49315643u,stream_sequence++,poll_timestamp,
        crc|(ap<<8)|(channel<<16)|(rssi<<24)};
    for(i=0;i<16;i++)out[i]=((uint8_t *)words)[i];
    out[16]=34;out[17]=(uint8_t)((slot<<4)|(controller_sid&15u));
    out[18]=(uint8_t)stream_drops;out[19]=(uint8_t)(stream_drops>>8);
    for(i=0;i<34;i++)out[20+i]=data[i];
    if(tud_cdc_connected() && tud_cdc_write_available()>=sizeof out) {
        tud_cdc_write(out,sizeof out);tud_cdc_write_flush();
    } else stream_drops++;
}
static void stream_status(uint32_t code,uint32_t a,uint32_t b)
{
    uint8_t out[54]={0}; unsigned i;
    uint32_t words[5]={0x53315643u,code,a,b,stream_drops};
    for(i=0;i<20;i++)out[i]=((uint8_t *)words)[i];
    if(tud_cdc_connected() && tud_cdc_write_available()>=sizeof out) {
        tud_cdc_write(out,sizeof out);tud_cdc_write_flush();
    }
}
/* CV1R carries up to 48 raw reply bytes (config reads are 37, too long for CV1S).
 * The last byte is the reply slot, since both same-hand Touch answer a shared-session read. */
static void stream_reply(const uint8_t *data,unsigned len,unsigned slot)
{
    uint8_t out[54]={'C','V','1','R'}; unsigned i;
    if(len>48) len=48;
    out[53]=(uint8_t)slot;
    out[4]=(uint8_t)len;
    for(i=0;i<len;i++)out[5+i]=data[i];
    if(tud_cdc_connected() && tud_cdc_write_available()>=sizeof out) {
        tud_cdc_write(out,sizeof out);tud_cdc_write_flush();
    }
}
static void stream_raw(uint32_t code,const uint8_t *data,unsigned len,uint32_t b)
{
    uint8_t out[54]={0}; unsigned i;
    uint32_t words[5]={0x53315643u,code,len,b,stream_drops};
    if(len>34) len=34;
    for(i=0;i<20;i++)out[i]=((uint8_t *)words)[i];
    for(i=0;i<len;i++)out[20+i]=data[i];
    if(tud_cdc_connected() && tud_cdc_write_available()>=sizeof out) {
        tud_cdc_write(out,sizeof out);tud_cdc_write_flush();
    }
}
/* Status 9: address[5] then key[16], which the host needs to decrypt reports and sync the sensors. */
static void stream_identity(void)
{
    uint8_t id[21]; unsigned i;
    for(i=0;i<5;i++) id[i]=address[i];
    for(i=0;i<16;i++) id[5+i]=key[i];
    stream_raw(9u,id,sizeof id,DM_FW_REV);
}
/* the host holds DTR while it has the port open */
static int host_attached(void) { return tud_cdc_connected(); }
static void reboot(void)
{
    GPIO_CLR=LED; POWER_GPREGRET=DFU_MAGIC_UF2;
    __asm volatile("dsb" ::: "memory");
    SCB_AIRCR=AIRCR_SYSRESET;
    for(;;) {}
}

/* --- identity ------------------------------------------------------------ */
#define RNG_TASKS_START  REG(0x4000D000UL)
#define RNG_TASKS_STOP   REG(0x4000D004UL)
#define RNG_EVENTS_VALRDY REG(0x4000D100UL)
#define RNG_CONFIG       REG(0x4000D504UL)
#define RNG_VALUE        REG(0x4000D508UL)
static uint8_t random_byte(void)
{
    uint32_t end=now()+10000u;
    RNG_EVENTS_VALRDY=0;
    while(!RNG_EVENTS_VALRDY && before(end)) {}
    return (uint8_t)RNG_VALUE;
}
static uint32_t crc32(const uint8_t *p,unsigned n)
{
    uint32_t c=~0u; unsigned i,k;
    for(i=0;i<n;i++) { c^=p[i]; for(k=0;k<8;k++) c=(c>>1)^(0xedb88320u&(0u-(c&1u))); }
    return ~c;
}
/* Bytes the radio handles badly; alternating bits look like preamble and lock a receiver early. */
static int bad_address_byte(uint8_t b) { return b==0x00u||b==0xffu||b==0x55u||b==0xaau; }
static int nvm_wait(void)
{
    uint32_t end=now()+200000u;
    while(!NVMC_READY && before(end)) {}
    return NVMC_READY!=0;
}
static int identity_valid(const struct identity *id)
{
    unsigned i;
    if(id->magic!=IDENTITY_MAGIC || id->version!=1u ||
       id->check!=crc32((const uint8_t *)id,offsetof(struct identity,check)))
        return 0;
    for(i=0;i<5;i++) if(bad_address_byte(id->address[i])) return 0;
    return 1;
}
/* Runs before USB and the radio start, since a page erase stalls the CPU ~85 ms. */
static void load_identity(void)
{
    const struct identity *stored=(const struct identity *)IDENTITY_PAGE;
    struct identity id; unsigned i;
    if(!identity_valid(stored)) {
        const uint32_t *w=(const uint32_t *)&id;
        for(i=0;i<sizeof id;i++) ((uint8_t *)&id)[i]=0;
        id.magic=IDENTITY_MAGIC; id.version=1u;
        RNG_CONFIG=1u;
        RNG_TASKS_START=1;
        for(i=0;i<5;i++) { uint8_t b; do b=random_byte(); while(bad_address_byte(b)); id.address[i]=b; }
        for(i=0;i<16;i++) id.key[i]=random_byte();
        RNG_TASKS_STOP=1;
        id.check=crc32((const uint8_t *)&id,offsetof(struct identity,check));
        NVMC_CONFIG=2; nvm_wait();
        NVMC_ERASEPAGE=IDENTITY_PAGE; nvm_wait();
        NVMC_CONFIG=1; nvm_wait();
        for(i=0;i<sizeof id/4u;i++) { REG(IDENTITY_PAGE+4u*i)=w[i]; nvm_wait(); }
        NVMC_CONFIG=0; nvm_wait();
        if(!identity_valid(stored)) stored=&id; /* flash refused: run on it unsaved */
    }
    for(i=0;i<5;i++) address[i]=stored->address[i];
    for(i=0;i<16;i++) key[i]=stored->key[i];
}
/* Learned hop increment, appended as words after the identity so no erase is needed; last one wins.
 * A full log falls back to RAM only. */
#define HOP_LOG       (IDENTITY_PAGE+0x100u)
#define HOP_LOG_WORDS 64u
#define HOP_LOG_TAG   0x484f5000u /* "\0PH" over the increment byte */
static uint8_t hop_ram;
static uint8_t hop_stored(void)
{
    unsigned i; uint8_t inc=hop_ram;
    if(inc) return inc;
    for(i=0;i<HOP_LOG_WORDS;i++) {
        uint32_t w=REG(HOP_LOG+4u*i);
        if(w==0xffffffffu) break;
        if((w&0xffffff00u)==HOP_LOG_TAG && (w&0xffu)>0u && (w&0xffu)<37u) inc=(uint8_t)w;
    }
    return inc;
}
/* About 41 us of stalled CPU: call it where no reply can arrive. */
static void hop_store(uint8_t inc)
{
    unsigned i;
    hop_ram=inc;
    for(i=0;i<HOP_LOG_WORDS;i++) if(REG(HOP_LOG+4u*i)==0xffffffffu) break;
    if(i==HOP_LOG_WORDS) return;
    NVMC_CONFIG=1; nvm_wait();
    REG(HOP_LOG+4u*i)=HOP_LOG_TAG|inc; nvm_wait();
    NVMC_CONFIG=0; nvm_wait();
}
static void disable_radio(void)
{
    uint32_t end=now()+1000;
    RADIO_SHORTS=0;
    if(RADIO_STATE==0) return;
    RADIO_EVENTS_DISABLED=0; RADIO_TASKS_DISABLE=1;
    while(!RADIO_EVENTS_DISABLED && before(end)) {}
    if(!RADIO_EVENTS_DISABLED) fault=1;
}
static void start_rx(void)
{
    RADIO_PACKETPTR=(uint32_t)rx;
    RADIO_EVENTS_END=0; RADIO_EVENTS_ADDRESS=0;
    RADIO_SHORTS=0x113; RADIO_TASKS_RXEN=1;
}
/* Reply end times per session, to find where the idle gap in a slot really is. */
static uint32_t rx_end_us;
static uint32_t reply_min[8], reply_max[8], reply_sum[8], reply_n[8];
static int receive_until(uint32_t end)
{
    while(before(end)) {
        if(RADIO_EVENTS_END) {
            __asm volatile("" ::: "memory");
            rx_end_us=now();
            crc=RADIO_CRCSTATUS; ap=RADIO_RXMATCH; rssi=RADIO_RSSISAMPLE;
            disable_radio(); return 1;
        }
    }
    disable_radio(); return 0;
}
static int transmit(void)
{
    uint32_t end;
    disable_radio(); if(fault) return 0;
    /* Polls keep the default ramp: the sensor times exposures off them, and the fast ramp's 100 us shift broke frame matching. */
    RADIO_MODECNF0 &= ~1u;
    RADIO_PACKETPTR=(uint32_t)tx; RADIO_EVENTS_END=0; RADIO_EVENTS_DISABLED=0;
    RADIO_SHORTS=0x13; /* READY_START, END_DISABLE, ADDRESS_RSSISTART */
    RADIO_TASKS_TXEN=1; end=now()+1000;
    while(!RADIO_EVENTS_END && before(end)) {}
    if(!RADIO_EVENTS_END) { fault=2; disable_radio(); return 0; }
    tx_end=now();
    /* Disable fully before RX: dropping DISABLED_RXEN at END could cancel auto-RX before it fired. */
    disable_radio();
    if(fault) return 0;
    start_rx();
    return 1;
}
static void radio_config(uint32_t peer, int link)
{
    disable_radio();
    RADIO_POWER=1; RADIO_MODECNF0 &= ~1u;
    /* +8 dBm max: at 0 dBm the sensor lost exposure sync at ~1 m through the chip antenna. */
    RADIO_MODE=RADIO_MODE_NRF_2MBIT; RADIO_TXPOWER=8;
    RADIO_PCNF0=8; RADIO_PCNF1=link?0x01040030u:0x01040020u;
    RADIO_CRCCNF=3; RADIO_CRCPOLY=0x108421; RADIO_CRCINIT=link?0xffffff:0;
    if(link) {
        uint32_t base=((uint32_t)address[0]<<24)|((uint32_t)address[1]<<16)|
                      ((uint32_t)address[2]<<8)|address[3];
        RADIO_BASE0=base; RADIO_BASE1=base;
        RADIO_PREFIX0=(uint32_t)address[4]|0x02010800u;
        RADIO_PREFIX1=3; RADIO_RXADDRESSES=0x1f; RADIO_TXADDRESS=0;
        channel=LINK_CHANNEL;
    } else {
        RADIO_BASE0=0xdeadbeef; RADIO_BASE1=peer;
        RADIO_PREFIX0=0x04030201; RADIO_PREFIX1=0x08070605;
        RADIO_TXADDRESS=1; RADIO_RXADDRESSES=3; channel=peer?26:2;
    }
    RADIO_FREQUENCY=channel; RADIO_INTENCLR=0xffffffffu;
    fast_rx=link;
}
static int transaction(uint8_t opcode, uint8_t seq)
{
    uint32_t next=now(), limit=next+10000000u;
    while(before(limit) && !fault) {
        uint32_t end;
        while(before(next)) {}
        next+=SLOT_US;
        if(!transmit()) break;
        end=tx_end+1174; /* hmd_rf's receive deadline */
        if((int32_t)(end-next)>-100) end=next-100;
        while(before(end)) {
            if(!receive_until(end)) break;
            if(opcode==0 && crc && ap==1 && rx[0]==0) return 1;
            /* the controller's gate checks CRC and sequence, not opcode; also require a bounded frame */
            if((opcode==0 && crc && ap==1 && rx[0]>=1 && rx[0]<=48 &&
                (rx[1]&1)==(seq&1)) ||
               (opcode!=0 && dm_ack(rx,opcode,seq,crc,ap)))
                return 1;
            start_rx();
        }
        if(!before(next)) { fault=3; break; } /* never silently drift */
    }
    return 0;
}
static uint8_t hop_channel(uint32_t index);
static void build_poll(uint32_t timestamp, uint32_t index, uint8_t sid, int hop);
static int search_link(void)
{
    uint32_t sid;
    radio_config(0,1);
    for(sid=0;sid<256u && !fault;sid++) {
        uint32_t index;
        for(index=0;index<37u;index++) {
            uint32_t ts,end;
            channel=hop_channel(index); RADIO_FREQUENCY=channel;
            ts=now(); build_poll(ts,index,(uint8_t)sid,0);
            if(!transmit()) break;
            end=tx_end+1500u;
            if(receive_until(end) && crc && ap!=0 && rx[0]==33) {
                link_sid=(uint8_t)sid;
                controller_sid=rx[2];
                return (int)sid+1;
            }
            usb_service();
            if(pair_requested) return 0;
        }
    }
    return 0;
}
static uint8_t hop_channel(uint32_t index)
{
    uint32_t c=4u+2u*(index%37u); if(c>=26u) c+=2u; return (uint8_t)c;
}
static void build_poll(uint32_t timestamp, uint32_t index, uint8_t sid, int hop)
{
    /* Parked: a single map bit, which every index remaps onto. */
    uint64_t map=hop?channel_map<<3:(uint64_t)1<<(index+3); unsigned i;
    dm_clear(tx); tx[0]=(poll_mode&1u)?20:44; tx[1]=1; tx[2]=(poll_mode&4u)?0:sid;
    tx[5]=0x5c; tx[6]=0xcf; /* beacon header */
    for(i=0;i<4;i++) tx[7+i]=(uint8_t)(timestamp>>(8*i));
    tx[11]=(poll_mode&2u)?0:1; tx[13]=(uint8_t)exposure_period_us; tx[14]=(uint8_t)(exposure_period_us>>8);
    for(i=0;i<5;i++) tx[15+i]=(uint8_t)(map>>(8*i));
    tx[20]=hop?(uint8_t)index:24u;
}
/* Commands ride on a poll: opcode at byte 3, LE32 argument at 21. Opcodes match Rift.dll's NormalTouch report numbering. */
struct link_command { uint8_t opcode; uint32_t arg; };
static const struct link_command link_commands[] = {
    { 0x04, 0u },    /* IrLedConfig, pulse width from ir_width_us */
    /* No DisableSleep (0x0f): it did not stop the doze, and with no undo but a reboot it drained batteries overnight. */
};
#define NCOMMANDS (sizeof link_commands/sizeof link_commands[0])
#define COMMAND_EVERY 500u /* polls; resent in case it rebooted */
/* FRActiveTimeout (RAM only, reset by reboot): an hour, resent every 30 min and on rejoin so a still Touch never dozes.
 * Sent as a burst since single commands get lost on the air. */
#define OP_ACTIVE_TIMEOUT 0x18u
#define ACTIVE_TIMEOUT_S 3600u
#define ACTIVE_TIMEOUT_EVERY_POLLS 900000u /* 30 min */
#define ACTIVE_TIMEOUT_BURST 20u           /* polls, alternating the two sessions */
#define ACTIVE_TIMEOUT_REJOIN_POLLS 1000u  /* 2 s without a reply in the slot */
/* After the host is gone this long, PowerState(3) reboots the controllers to restore the default timeout, then PowerState(0) sleeps them.
 * Not shorter: a powered-off Touch needs a button and 5-10 s to light its LEDs. */
#define HOST_GRACE_POLLS 30000u /* 60 s */
#define REBOOT_POLLS 1000u        /* 2 s of reboot requests before the sleeps */
#define OP_POWER_STATE 0x17u

/* Hop learning: wait for a reply (the anchor), step by the candidate, and count replies over PROBE_POLLS.
 * Switching without an anchor fails, since both then step alike from different indexes and never meet. */
#define PROBE_POLLS       8u
#define PROBE_GOOD        3u
#define ANCHOR_POLLS      1000u  /* 2 s without any reply ends the sweep */
#define LEARN_RETRY_POLLS 15000u /* 30 s after a sweep found nothing */
/* Wrong increment: the best slot got 1 in 80 to 1 in 16 of 2500 polls. Fewer is a dozing controller, which relearning would not help. */
#define WRONG_HOP_MIN (2500u/80u)
#define WRONG_HOP_MAX (2500u/16u)
/* Status 8 b: map bits 32..36, increment (bits 8..13), auto (bit 14), sweep running (bit 15). */
static uint32_t hop_status(void)
{
    return ((uint32_t)(channel_map>>32)&0x1fu)|((uint32_t)hop_inc<<8)|
           ((hop_mode==HOP_AUTO)?1u<<14:0u)|(hop_learning?1u<<15:0u);
}
/* Step by 1 until the first anchor: a parked controller only takes a hopping map from a poll it hears, once in 37. */
static void hop_learn_start(uint8_t *prev, uint8_t *cand)
{
    *prev=hop_inc; *cand=1; hop_learning=1; hop_inc=1;
}
static void read_link(void)
{
    uint8_t packet[64];
    uint32_t learn_n=0, learn_got=0, learn_after=0, low_windows=0;
    uint8_t learn_prev=0, learn_cand=0, probing=0, hop_save=0;
    if(!channel_map_ready) set_channel_map(ALL_CHANNELS);
    uint32_t next=now(), polls=0, replies=0, overruns=0;
    uint32_t index=TABLE_INDEX, sweep=next+300000u, host_gone=0; int locked=0;
    uint32_t acks[NCOMMANDS]={0},sent_opcode=0,read_watch=0,read_sent=0,read_toggle=0,read_retries=0;
    uint32_t timeout_burst=ACTIVE_TIMEOUT_BURST, timeout_due=ACTIVE_TIMEOUT_EVERY_POLLS, slot_heard[8]={0};
    while(!fault && !pair_requested) {
        uint32_t ts,end;
        if(!locked && !before(sweep)) {
            index=(index+1u)%37u; channel=hop_channel(index);
            RADIO_FREQUENCY=channel; sweep=now()+300000u;
        }
        if(hop_mode==HOP_AUTO && locked && !hop_learning && !hop_inc && (int32_t)(polls-learn_after)>=0) {
            hop_learn_start(&learn_prev,&learn_cand); learn_n=learn_got=0; probing=0;
        }
        int hop=locked && hop_inc;
        unsigned ci=37u, poll_got=0; /* this poll's channel slot, for adapt_channels */
        if(hop) { index=(index+hop_inc)%37u; ci=channel_remap[index]; channel=hop_channel(ci); RADIO_FREQUENCY=channel; }
        if(exposure_phase_us<exposure_phase_target_us) { exposure_phase_us++; next++; }
        else if(exposure_phase_us>exposure_phase_target_us) { exposure_phase_us--; next--; }
        while(before(next)) {}
        ts=next; next+=SLOT_US;
        build_poll(ts,index,link_sid,hop); polls++;
        if(host_attached()) host_gone=0; else if(host_gone<0xffffffffu) host_gone++;
        /* the Touch buzzes until told amplitude 0: a host that crashed mid-buzz never sends it */
        if(host_gone==1u) {
            unsigned k;
            for(k=0;k<2u;k++) if(haptic_amp[k]) { haptic_amp[k]=0; haptic_repeat[k]=3; }
        }
        if(polls>=timeout_due || keepalive_kick) { keepalive_kick=0; timeout_burst=ACTIVE_TIMEOUT_BURST; timeout_due=polls+ACTIVE_TIMEOUT_EVERY_POLLS; }
        if(!keepalive_on || sleep_seq) timeout_burst=0;
        sent_opcode=0;
        if(locked && (read_pending || read_retries)) {
            unsigned i;
            if(read_pending){read_toggle^=0x04u;read_retries=1;}
            read_pending=0;read_retries--;
            /* Headset framing: 44 bytes, flags 0x12|toggle. Byte 11 belongs to the poll tick, so only a 'Y' override touches it. */
            tx[0]=0x2cu;tx[3]=read_op;
            tx[2]=read_sid==0?controller_sid:read_sid==0xffu?link_sid:read_sid;
            /* one transmission per request; the host retries */
            tx[4]=(uint8_t)(0x12u|read_toggle);   /* 0x10 read, 0x02 seen from the headset */
            if(read_b11!=0xffu) tx[11]=read_b11;
            for(i=0;i<4;i++) tx[21+i]=(uint8_t)(read_arg>>(8*i));
            for(i=25;i<45;i++) tx[i]=0;
            sent_opcode=read_op; read_sent=1;
            read_watch=50u;   /* polls */
        } else if(locked && (haptic_repeat[0] || haptic_repeat[1])) {
            unsigned k=haptic_repeat[polls&1u] ? (polls&1u) : !(polls&1u), i;
            haptic_repeat[k]--;
            tx[0]=44;tx[2]=haptic_sid[k];tx[3]=0x03u;tx[4]=0;
            for(i=0;i<4;i++) {
                tx[21+i]=(uint8_t)((uint32_t)haptic_freq[k]>>(8*i));
                tx[25+i]=(uint8_t)((uint32_t)haptic_amp[k]>>(8*i));
            }
            sent_opcode=0x03u;
        } else if(locked && !read_watch &&
                  (polls%COMMAND_EVERY<2u*NCOMMANDS || ir_dirty || sleep_seq ||
                   (timeout_burst && host_gone<=HOST_GRACE_POLLS))) {
            /* Address the reported device id, alternating both sessions so each hand gets width and wake.
             * A width change goes out at once since host auto exposure closes its loop through it. */
            const struct link_command *c=ir_dirty?&link_commands[0]:&link_commands[((polls%COMMAND_EVERY)/2u)%NCOMMANDS];
            struct link_command power;
            static const struct link_command active_timeout={OP_ACTIVE_TIMEOUT,ACTIVE_TIMEOUT_S};
            if(timeout_burst && !ir_dirty && host_gone<=HOST_GRACE_POLLS) { c=&active_timeout; timeout_burst--; }
            if(host_gone>HOST_GRACE_POLLS && !ir_dirty) {
                power.opcode=OP_POWER_STATE;
                power.arg=host_gone<HOST_GRACE_POLLS+REBOOT_POLLS?3u:0u;
                c=&power;
            } else if(sleep_seq && !ir_dirty) {
                power.opcode=OP_POWER_STATE;
                power.arg=0u;
                c=&power;
                sleep_seq--;
            }
            uint8_t command_sid=(uint8_t)(controller_sid ^ (ir_dirty ? (2u-ir_dirty) : (polls&1u)));
            if(ir_dirty) ir_dirty--;
            unsigned i;
            tx[0]=44;tx[2]=command_sid;tx[3]=c->opcode;tx[4]=0;
            uint32_t arg=c->opcode==0x04u?ir_width_us:c->arg;
            for(i=0;i<4;i++) tx[21+i]=(uint8_t)(arg>>(8*i));
            sent_opcode=c->opcode;
        }
        if(DM_SYNC_PIN) GPIO_SET=1u<<DM_SYNC_PIN;
        if(!transmit()) break;
        if(DM_SYNC_PIN) GPIO_CLR=1u<<DM_SYNC_PIN;
        /* flash write stalls the CPU, so do it before the earliest reply (~540 us) */
        if(hop_save) { hop_store(hop_save); hop_save=0; }
        while(before(ts+700u)) usb_service();
        if(read_watch && !read_sent) read_watch--;
        /* Listen until 60 us before the next poll, except in the status frame:
         * the records need the time, and a late poll cost both controllers ~270 ms of bad MICs. */
        end=polls%2500u==0?tx_end+1500:next-60;
        if((int32_t)(end-next)>-60) end=next-60;
        while(before(end)) {
            if(!receive_until(end)) break;
            /* copy out and rearm first, the next controller's reply lands in rx */
            for(unsigned k=0;k<sizeof packet;k++) packet[k]=rx[k];
            if(fast_rx) RADIO_MODECNF0 |= 1u;
            start_rx();
            /* uplink bit 2 is the controller's real transaction toggle, lost reads included */
            if(crc && packet[2]==(read_sid==0?controller_sid:read_sid==0xffu?link_sid:read_sid) && (packet[1]&15)==1)
                read_toggle=packet[4]&0x04u;
            if(read_watch && crc && (packet[0]!=33 || packet[3]!=0 || !(packet[4]&0x20)))
                stream_reply(packet,(unsigned)packet[0]+1u,reply_slot(rx_end_us-ts));
            if(sent_opcode && crc && packet[3]==sent_opcode) {
                unsigned k;
                for(k=0;k<NCOMMANDS;k++) if(link_commands[k].opcode==sent_opcode) acks[k]++;
            }
            if(crc && ap!=0 && packet[0]==33 && (packet[1]&15)==1 && (packet[4]&0x20)) {
                uint32_t off=rx_end_us-ts; unsigned k=reply_slot(off);
                replies++; poll_got++; locked=1;
                if(!slot_heard[k] || polls-slot_heard[k]>ACTIVE_TIMEOUT_REJOIN_POLLS) timeout_burst=ACTIVE_TIMEOUT_BURST;
                slot_heard[k]=polls;
                if(!reply_n[k] || off<reply_min[k]) reply_min[k]=off;
                if(off>reply_max[k]) reply_max[k]=off;
                reply_sum[k]+=off; reply_n[k]++;
                stream_input(packet,ts,k);
            }
        }
        disable_radio();
        if(hop_learning) {
            int done=0;
            learn_n++;
            if(!probing) {
                /* the current step is wrong, so an anchor comes within 37 */
                if(poll_got) { hop_inc=learn_cand; probing=1; learn_n=learn_got=0; }
                else if(learn_n>=ANCHOR_POLLS) done=-1;
            } else {
                if(poll_got) learn_got++;
                if(learn_n>=PROBE_POLLS) {
                    if(learn_got>=PROBE_GOOD) done=1;
                    else if(learn_cand>=36u) done=-1;
                    else { learn_cand++; probing=0; learn_n=0; }
                }
            }
            if(done) {
                unsigned k;
                hop_learning=0;
                if(done>0) hop_save=hop_inc;
                else { hop_inc=learn_prev; learn_after=polls+LEARN_RETRY_POLLS; }
                /* the sweep's misses say nothing about the channels */
                for(k=0;k<37;k++) ch_expected[k]=ch_got[k]=0;
                stream_status(8,(uint32_t)channel_map,hop_status());
            }
            ci=37u;
        }
        if(ci<37u) {
            unsigned active=0, k;
            for(k=0;k<8;k++) if(slot_heard[k] && polls-slot_heard[k]<500u) active++;
            if(active) { ch_expected[ci]+=active; ch_got[ci]+=poll_got; }
        }
        if(read_sent) stream_raw(5u,tx,34u,0u);
        read_sent=0;
        /* Status 7 keeps the host's exposure clock with no Touch connected (PS VR2 Sense LED sync runs on it).
         * Every 10 ms so each exposure period gets one. */
        if(polls%5u==0) stream_status(7,ts,exposure_period_us);
        if(polls%2500u==0) {
            if(hop && !hop_learning) adapt_channels(polls);
            stream_status(8,(uint32_t)channel_map,hop_status());
            if(hop_mode==HOP_AUTO && hop_inc && !hop_learning && (int32_t)(polls-learn_after)>=0) {
                uint32_t best=0;
                for(unsigned k=0;k<8;k++) if(reply_n[k]>best) best=reply_n[k];
                if(best>=WRONG_HOP_MIN && best<WRONG_HOP_MAX) {
                    if(++low_windows>=2) { low_windows=0; hop_learn_start(&learn_prev,&learn_cand); learn_n=learn_got=0; probing=0; }
                } else low_windows=0;
            }
            stream_status(2,polls,(replies&0xffffu)|(overruns<<16));
            stream_status(5,DM_FW_REV|((uint32_t)(exposure_period_us/100u)<<16),FICR_DEVICEID0);
            if(!identity_due) identity_due=1u;
            /* status 3: opcode|count<<8 */
            stream_status(3,link_commands[0].opcode|(acks[0]<<8),
                          link_commands[NCOMMANDS-1].opcode|(acks[NCOMMANDS-1]<<8));
            /* Status 4 reply end offsets: a = slot<<24 | count, b = min | max<<12 | avg<<24 (16 us units). */
            for(unsigned k=0;k<8;k++) if(reply_n[k]) {
                uint32_t avg=reply_sum[k]/reply_n[k];
                stream_status(4,(k<<24)|(reply_n[k]&0xffffffu),
                    (reply_min[k]&0xfffu)|((reply_max[k]&0xfffu)<<12)|(((avg>>4)&0xffu)<<24));
                reply_n[k]=reply_sum[k]=reply_max[k]=0;
            }
        }
        if(!before(next)) {
            /* Stay on the grid: the controllers keep the slot time, and a shifted grid broke their MICs until they caught up. */
            uint32_t skipped=(now()-next)/SLOT_US+1u;
            overruns++; next+=skipped*SLOT_US;
            if(locked && hop_inc) index=(index+hop_inc*(skipped%37u))%37u;
        }
        usb_service();
    }
}

static int pair_controller(void)
{
    uint32_t deadline,peer=0; uint8_t device_type=0;
    radio_config(0,0); deadline=now()+60000000u;
    while(before(deadline) && !fault) {
        uint32_t end=now()+20000u; if((int32_t)(end-deadline)>0) end=deadline;
        start_rx(); if(!receive_until(end)) { usb_service(); continue; }
        if(crc && ap==0 && rx[0]==25 && (rx[1]>>1)==1 && rx[2]==1 &&
           (rx[5]==2 || rx[5]==3)) {
            device_type=rx[5];
            peer=(uint32_t)rx[6]|((uint32_t)rx[7]<<8)|((uint32_t)rx[8]<<16)|((uint32_t)rx[9]<<24);
            break;
        }
        usb_service();
    }
    if(!device_type) return fault ? 3 : 4; /* failed or timed out */
    radio_config(peer,0);
    dm_connect(tx);
    if(!transaction(0,0)) return 3;
    {
        uint16_t slot_us=(uint16_t)(pair_us?pair_us:pair_slot?SLOT_OFFSET_US(pair_slot):device_type==2?1000u:1250u);
        pair_us=0;
        dm_config(tx,peer,address,key,slot_us,1);
    }
    if(!transaction(0x13,1)) return 3;
    dm_finish(tx,0);
    if(!transaction(0x21,0)) return 3;
    if(!search_link() || fault) return 3;
    return 2;
}
int main(void)
{
    uint32_t deadline,i;
    NVIC_ICER0=0xffffffff; NVIC_ICPR0=0xffffffff; GPIO_DIR=LED|(DM_SYNC_PIN?1u<<DM_SYNC_PIN:0u);
    TIMER0_MODE=0; TIMER0_BITMODE=3; TIMER0_PRESCALER=4;
    TIMER0_TASKS_CLEAR=1; TIMER0_TASKS_START=1;
    CLOCK_EVENTS_HFCLKSTARTED=0; CLOCK_TASKS_HFCLKSTART=1;
    deadline=now()+1000000;
    while(!CLOCK_EVENTS_HFCLKSTARTED && before(deadline)) {}
    if(!CLOCK_EVENTS_HFCLKSTARTED) { fault=4; goto done; }
    load_identity();
    hop_inc=hop_stored();
    tud_init(0);
    /* No BSP to deliver VBUS events, and USB is always powered while this runs. */
    tusb_hal_nrf_power_event(0); /* USB_EVT_DETECTED */
    tusb_hal_nrf_power_event(2); /* USB_EVT_READY */
    for(i=0;i<10000u;i++) usb_service();
    stream_status(1,(uint32_t)search_link(),fault);
    for(;;) {
        if(fault) {
            /* The radio stopped answering. Power-cycling the peripheral resets every register, and
             * radio_config sets them again: better than a dongle that stays dead until a replug, or
             * a reboot, which re-enumerates USB under the host. Status 1 says it happened. */
            uint32_t resume=now()+100000u;
            stream_status(1,0u,fault);
            RADIO_POWER=0; RADIO_POWER=1; fault=0;
            while(before(resume)) usb_service();
        }
        radio_config(0,1); read_link();
        if(fault) continue;
        if(pair_requested) {
            pair_requested=0u; fault=0;
            stream_status(6,1u,0u);
            {
                int state=pair_controller();
                stream_status(6,(uint32_t)state,0u);
                fault=0;
            }
            continue;
        }
        break;
    }
done:
    disable_radio();
    for(;;) usb_service();
    return 0;
}