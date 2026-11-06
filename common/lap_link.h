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
#define LL_MAGIC_CAL     'M' /* base -> gate: measure noise or peak levels */
#define LL_MAGIC_CALRES  'R' /* gate -> base: the measurement             */
#define LL_MAGIC_LIVEREQ 'V' /* base -> gate: start/stop streaming live levels */
#define LL_MAGIC_LIVE    'S' /* gate -> base: one live level sample          */
#define LL_MAGIC_NAME    'N' /* base -> gate: set a pilot's name (ACKed like CFG) */
#define LL_MAGIC_SET     'T' /* base -> gate: set the race options (ACKed like CFG) */
#define LL_MAGIC_SYNCREQ 'Q' /* base -> gate: send me your names and race options  */
#define LL_MAGIC_SYNC    'Y' /* gate -> base: names and race options             */

#define LL_PASSES_PER_PACKET 3

#pragma pack(push, 1)

typedef struct {
    uint8_t  pilot;     /* 1..8                                          */
    uint8_t  count;     /* passes by this pilot since the gate was armed */
    uint32_t pass_ms;   /* gate clock at the middle of the crossing's peak */
    uint32_t lap_ms;    /* pass_ms minus the pilot's previous pass; 0 for the first */
    uint8_t  peak;      /* peak level code of the crossing (0.25 dB steps) */
    uint8_t  xp;        /* strongest OTHER pilot around the crossing (1..8, 0 = none) ... */
    uint8_t  xl;        /* ... and its recent peak level code: tells leakage from a real pass */
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
    uint8_t  flags;     /* LL_STATUS_SNR: level codes are dB above the noise floor, not RSSI */
    uint8_t  meta_rev;  /* changes whenever the names or race options change: ask for a SYNC */
} ll_status_pkt_t;
#define LL_STATUS_SNR 0x01

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_CFG */
    uint8_t  version;
    uint8_t  gate_id;   /* 0xFF = every gate */
    uint16_t seq;       /* base-side counter, a repeat is ignored        */
    uint8_t  pilot;     /* 1..8 */
    ll_pilot_cfg_t cfg;
    uint8_t  flags;     /* LL_CFG_SAVE: also persist to the gate's NVS   */
} ll_cfg_pkt_t;

/* The gate answers a CFG with an ll_ack_pkt_t carrying the CFG's seq (the base
 * retransmits the CFG until it hears it), and a CAL with an ll_cal_res_t after
 * `secs` seconds of measuring: per pilot the lowest, average and highest filtered
 * level code seen. Thresholds are then worked out on the base side. */
typedef struct {
    uint8_t  magic;     /* LL_MAGIC_CAL */
    uint8_t  version;
    uint8_t  gate_id;   /* 0xFF = every gate */
    uint16_t seq;
    uint8_t  kind;      /* LL_CAL_NOISE or LL_CAL_PEAK (echoed in the result) */
    uint8_t  secs;      /* 1..10 */
} ll_cal_req_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_CALRES */
    uint8_t  version;
    uint8_t  gate_id;
    uint16_t seq;       /* the request's seq */
    uint8_t  kind;
    uint8_t  lo[LL_MAX_PILOTS], avg[LL_MAX_PILOTS], hi[LL_MAX_PILOTS]; /* 0 = pilot not scanned */
} ll_cal_res_t;

/* Live levels over LoRa, for tuning thresholds and watching the signal without a USB
 * cable. The base asks for `secs` seconds (0 = stop, refreshed by asking again; the gate
 * caps it at 60) and the gate sends a sample every ~400 ms: the current filtered level of
 * every pilot plus the highest level since the previous sample (short spikes show up). It
 * uses a few percent of the 1 % duty-cycle budget while on, so it is a tuning tool, not
 * something to leave running. */
typedef struct {
    uint8_t  magic;     /* LL_MAGIC_LIVEREQ */
    uint8_t  version;
    uint8_t  gate_id;   /* 0xFF = every gate */
    uint16_t seq;
    uint8_t  secs;
} ll_live_req_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_LIVE */
    uint8_t  version;
    uint8_t  gate_id;
    uint16_t seq;
    uint8_t  crossing;  /* bit per pilot: currently inside a crossing */
    uint8_t  level[LL_MAX_PILOTS], hold[LL_MAX_PILOTS]; /* 0 = pilot not scanned */
} ll_live_pkt_t;

/* Everything a UI needs besides pilot channels and thresholds lives in the gate's flash, so
 * the base, a phone and the Mac all show the same thing. Names are short (UTF-8, NUL padded);
 * the race options are the ones the page used to keep locally. */
#define LL_NAME_LEN 12
#define LL_SET_HOLESHOT 0x01 /* the first crossing is only the hole shot, not a counted lap */

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_NAME */
    uint8_t  version;
    uint8_t  gate_id;   /* 0xFF = every gate */
    uint16_t seq;
    uint8_t  pilot;     /* 1..8 */
    char     name[LL_NAME_LEN];
} ll_name_pkt_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_SET */
    uint8_t  version;
    uint8_t  gate_id;
    uint16_t seq;
    uint8_t  laps;      /* target laps for the finish flag */
    uint8_t  flags;     /* LL_SET_HOLESHOT */
    uint8_t  countdown; /* seconds */
    uint16_t min_lap_ms;
} ll_set_pkt_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_SYNCREQ */
    uint8_t  version;
    uint8_t  gate_id;
    uint16_t seq;
} ll_syncreq_pkt_t;

typedef struct {
    uint8_t  magic;     /* LL_MAGIC_SYNC */
    uint8_t  version;
    uint8_t  gate_id;
    uint16_t seq;       /* the request's seq */
    uint8_t  meta_rev;
    uint8_t  laps, flags, countdown;
    uint16_t min_lap_ms;
    char     name[LL_MAX_PILOTS][LL_NAME_LEN];
} ll_sync_pkt_t;

#pragma pack(pop)

#define LL_CAL_NOISE 0
#define LL_CAL_PEAK  1

#define LL_CFG_SAVE 0x01

_Static_assert(sizeof(ll_pass_t) == 13, "ll_pass_t layout");
_Static_assert(sizeof(ll_pass_pkt_t) == 49, "ll_pass_pkt_t layout");
_Static_assert(sizeof(ll_ack_pkt_t) == 8, "ll_ack_pkt_t layout");
_Static_assert(sizeof(ll_status_pkt_t) == 47, "ll_status_pkt_t layout");
_Static_assert(sizeof(ll_cfg_pkt_t) == 11, "ll_cfg_pkt_t layout");
_Static_assert(sizeof(ll_cal_req_t) == 7, "ll_cal_req_t layout");
_Static_assert(sizeof(ll_cal_res_t) == 30, "ll_cal_res_t layout");
_Static_assert(sizeof(ll_live_req_t) == 6, "ll_live_req_t layout");
_Static_assert(sizeof(ll_live_pkt_t) == 22, "ll_live_pkt_t layout");
_Static_assert(sizeof(ll_name_pkt_t) == 18, "ll_name_pkt_t layout");
_Static_assert(sizeof(ll_set_pkt_t) == 10, "ll_set_pkt_t layout");
_Static_assert(sizeof(ll_syncreq_pkt_t) == 5, "ll_syncreq_pkt_t layout");
_Static_assert(sizeof(ll_sync_pkt_t) == 107, "ll_sync_pkt_t layout");
