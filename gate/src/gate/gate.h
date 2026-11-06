/* Lap-timer gate: one "seat" per pilot (1..8), each a scanned channel with its
 * own crossing detector (lap.c). Passes are queued for the LoRa task and
 * printed on USB. GPL-3.0-or-later (derived from Widscan58's rh.c). */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "lap.h"
#include "scanner.h"
#include "lap_link.h"

#define GATE_SEATS PROFILE_MAX_CHANNELS

typedef struct {
    uint8_t pilot;      /* 1..8 */
    uint8_t count;      /* passes since gate_reset */
    uint32_t pass_ms;
    uint32_t lap_ms;    /* 0 for the first pass */
    uint8_t peak;
    uint8_t lv[GATE_SEATS]; /* every seat's filtered level at the moment of the pass (debug) */
    uint8_t xp, xl;         /* strongest other seat (1..8, 0 none) and its recent peak around the pass */
} gate_pass_t;

esp_err_t gate_init(void);
/* Called often from the I/O task: stores the settings in flash a couple of seconds after the
 * last change (from USB or LoRa), so a gate powered on its own comes back as it was left. */
void gate_autosave_tick(void);
/* True when the seats' level codes are SNR (dB above the channel's noise floor, x4)
 * rather than absolute RSSI; thresholds are in the same unit. */
bool gate_metric_is_snr(void);
/* Scan setup from the pilot frequencies; scanning only once one is enabled. */
void gate_apply_plan(scan_config_t *c);
/* Forget every seat's levels and crossing (lap_reset), e.g. when the level
 * metric changes. */
void gate_reset_levels(void);
void gate_set_filter(lap_filter_t f);
lap_filter_t gate_get_filter(void);
/* One level update for a seat (scanner task). */
void gate_feed(unsigned seat, int64_t t_us, uint8_t level);

/* Pilot settings (pilot is 1..8). freq_mhz 0 disables the pilot. */
bool gate_get_pilot(unsigned pilot, ll_pilot_cfg_t *out);
bool gate_set_pilot(unsigned pilot, const ll_pilot_cfg_t *cfg);
/* Zero the pass counters and lap references ("race start"). */
void gate_reset_counts(void);
uint16_t gate_min_lap_ms(void);
void gate_set_min_lap_ms(uint16_t ms);
uint8_t gate_id(void);
void gate_set_id(uint8_t id);
/* Pilot names and the race options live here too, so every UI (base, phone, Mac) shows the same
 * thing. meta_rev changes whenever either changes. Names are short, UTF-8 and never contain a quote,
 * a backslash or a control character. */
typedef struct { uint8_t laps, flags, countdown; } gate_race_t;   /* flags: LL_SET_HOLESHOT */
void gate_get_name(unsigned pilot, char out[LL_NAME_LEN + 1]);
bool gate_set_name(unsigned pilot, const char *name);
void gate_get_race(gate_race_t *out);
void gate_set_race(const gate_race_t *in);
uint8_t gate_meta_rev(void);
/* Persist / restore pilots, gate id, min lap time, names, race options (NVS). */
esp_err_t gate_save(void);
void gate_load(void);

/* Live view for the debug dashboard. */
typedef struct {
    uint8_t level[GATE_SEATS];
    uint8_t hold[GATE_SEATS]; /* highest filtered level since the previous gate_view */
    uint8_t enter[GATE_SEATS], exit[GATE_SEATS];
    uint8_t count[GATE_SEATS];
    uint8_t crossing;  /* bit per seat */
    uint8_t enabled;   /* bit per seat */
} gate_view_t;
void gate_view(gate_view_t *v);

/* Calibration: levels of every scanned seat over a window. Start, wait, finish.
 * lo/avg/hi are filtered level codes (0 for a seat that was not scanned). */
typedef struct { uint8_t lo[GATE_SEATS], avg[GATE_SEATS], hi[GATE_SEATS]; } gate_cal_t;
void gate_cal_start(void);
void gate_cal_finish(gate_cal_t *out);

/* Debug: inject a pass for a pilot as if it had been detected now. */
void gate_sim_pass(unsigned pilot);

/* Passes waiting for the LoRa/USB task. Returns false after timeout. */
bool gate_next_pass(gate_pass_t *p, uint32_t timeout_ms);
