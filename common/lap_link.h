/* LoRa wire format between a gate node (ESP32-C5) and the base station.
 * Shared by gate/ (C/C++, ESP-IDF) and base/ (C++, Arduino). Plain C, packed,
 * little-endian (both ends are little-endian MCUs).
 *
 * Division of labour: the gate only knows pilot NUMBERS (1..8, one per scanned
 * channel), per-pilot thresholds and pass timestamps from its own clock. Names,
 * lap aggregation, best laps and call-outs live on the server.
 *
 * Timing: a pass is stamped with the gate's millisecond clock (`pass_ms`). Every
 * PASS packet also carries the gate clock at the moment it was handed to the
 * radio (`sent_ms`), so the receiver can place a pass on its own timeline as
 *     pass_time = rx_time - time_on_air - (sent_ms - pass_ms)
 * without a separate clock-sync exchange, and a retransmission still resolves
 * to the same instant. `lap_ms` is computed on the gate from two of its own
 * timestamps, so it carries no radio jitter at all.
 */
#pragma once
#include <stdint.h>

#define LL_VERSION       1
#define LL_MAX_PILOTS    8

#define LL_MAGIC_PASS    'L' /* gate -> base: one or more pilot passes   */
#define LL_MAGIC_ACK     'K' /* base -> gate: acknowledges a PASS        */
#define LL_MAGIC_STATUS  'H' /* gate -> base: periodic config + health   */
#define LL_MAGIC_CFG     'C' /* base -> gate: set one pilot's settings   */

#define LL_PASSES_PER_PACKET 3

#pragma pack(push, 1)

typedef struct {
    uint8_t  pilot;     /* 1..8                                          */
    uint8_t  count;     /* passes by this pilot since the gate was armed */
    uint32_t pass_ms;   /* gate clock at the middle of the crossing's peak */
    uint32_t lap_ms;    /* pass_ms minus the pilot's previous pass; 0 for the first */
    uint8_t  peak;      /* peak level code of the crossing (0.25 dB steps) */
} ll_pass_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_PASS */
    uint8_t  version;
    uint8_t  gate_id;
    uint16_t seq;       /* per-packet counter; a retransmission gets a new one */
    uint32_t sent_ms;   /* gate clock when this packet was queued to the radio */
    uint8_t  n;         /* number of entries in p[] (1..LL_PASSES_PER_PACKET) */
    ll_pass_t p[LL_PASSES_PER_PACKET];
} ll_pass_pkt_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_ACK */
    uint8_t  version;
    uint8_t  gate_id;
    uint16_t seq;       /* echoes the PASS or STATUS seq being acknowledged */
    int16_t  rssi_dbm;  /* uplink RSSI as the base saw it */
    int8_t   snr_db;
} ll_ack_pkt_t;

typedef struct {
    uint16_t freq_mhz;  /* 0 = pilot disabled (not scanned)  */
    uint8_t  enter_at;  /* level code, crossing starts here  */
    uint8_t  exit_at;   /* level code, crossing ends here    */
} ll_pilot_cfg_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_STATUS */
    uint8_t  version;
    uint8_t  gate_id;
    uint16_t seq;
    uint32_t sent_ms;
    uint16_t min_lap_ms_div10; /* minimum lap time, in 10 ms units */
    uint16_t dropped;   /* pass entries given up on after retries */
    ll_pilot_cfg_t pilot[LL_MAX_PILOTS];
} ll_status_pkt_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_CFG */
    uint8_t  version;
    uint8_t  gate_id;   /* 0xFF = every gate */
    uint16_t seq;       /* base-side counter, a repeat is ignored        */
    uint8_t  pilot;     /* 1..8 */
    ll_pilot_cfg_t cfg;
    uint8_t  flags;     /* LL_CFG_SAVE: also persist to the gate's NVS   */
} ll_cfg_pkt_t;

#pragma pack(pop)

#define LL_CFG_SAVE 0x01

_Static_assert(sizeof(ll_pass_t) == 11, "ll_pass_t layout");
_Static_assert(sizeof(ll_pass_pkt_t) == 43, "ll_pass_pkt_t layout");
_Static_assert(sizeof(ll_ack_pkt_t) == 8, "ll_ack_pkt_t layout");
_Static_assert(sizeof(ll_status_pkt_t) == 45, "ll_status_pkt_t layout");
_Static_assert(sizeof(ll_cfg_pkt_t) == 11, "ll_cfg_pkt_t layout");
