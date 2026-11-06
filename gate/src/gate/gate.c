/* GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "gate.h"
#include "lap.h"
#include "radio.h"
#include "frames.h"

/* Before the dashboard sets thresholds. Level codes are 0.25 dB: in SNR mode (the
 * default) 64 / 32 are 16 dB / 8 dB above the channel's noise floor, whose
 * peaks are ~1-2.5 dB; calibrate on the dashboard for the real setup. */
#define DEFAULT_ENTER 64
#define DEFAULT_EXIT 32
#define DEFAULT_MIN_LAP_MS 3000
#define NVS_NAMESPACE "gate"
#define NVS_KEY_CFG "cfg"
#define SAVED_VERSION 3 /* 3 adds names and race options; 2 and 1 (RSSI-scale thresholds, dropped) still load */

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; /* seats: scanner vs command task */
/* On the heap: the median windows (~5 KB) don't fit in the C5's static RAM
 * below the RF dump bank. */
static struct {
    uint16_t freq_mhz;   /* 0: disabled */
    bool mapped;         /* the profile can tune it */
    int64_t last_us;
    uint8_t count;       /* passes since gate_reset_counts */
    uint8_t last_lap;    /* lap.lap when we last looked */
    uint8_t hold;        /* peak of the filtered level since the last gate_view */
    uint8_t rp;          /* recent peak: the filtered level with a ~100 ms decay, for cross-seat comparison */
    uint8_t cal_lo, cal_hi; /* calibration window */
    uint32_t cal_sum, cal_n;
    bool have_prev;
    uint32_t prev_pass_ms;
    lap_seat_t lap;
} *seat;
static uint16_t min_lap_ms = DEFAULT_MIN_LAP_MS;
static uint8_t id = 1;
static QueueHandle_t passes;
static char names[GATE_SEATS][LL_NAME_LEN + 1];
static gate_race_t race_opts = {20, LL_SET_HOLESHOT, 0};
static uint8_t meta_rev;
static volatile bool cal_on;
static volatile uint32_t dirty_ms; /* time of the last unsaved change, 0 = saved */
/* Raceband R1..R8: the channel plan until a pilot is set otherwise, so a gate that has
 * never been configured still scans all eight pilots when it is powered on its own. */
static const uint16_t RACEBAND[GATE_SEATS] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};

typedef struct {
    uint8_t version, gate_id;
    uint16_t min_lap_ms;
    ll_pilot_cfg_t pilot[GATE_SEATS];
    char name[GATE_SEATS][LL_NAME_LEN + 1];
    uint8_t laps, flags, countdown;
} saved_t;
typedef struct {   /* versions 1 and 2 */
    uint8_t version, gate_id;
    uint16_t min_lap_ms;
    ll_pilot_cfg_t pilot[GATE_SEATS];
} saved_v2_t;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t gate_init(void) {
    seat = heap_caps_calloc(GATE_SEATS, sizeof(*seat), MALLOC_CAP_INTERNAL);
    passes = xQueueCreate(32, sizeof(gate_pass_t));
    if (!seat || !passes) return ESP_ERR_NO_MEM;
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        lap_init(&seat[j].lap, DEFAULT_ENTER, DEFAULT_EXIT);
        seat[j].freq_mhz = RACEBAND[j];
        if (j == 0) strcpy(names[j], "Ron"); else snprintf(names[j], sizeof(names[j]), "Pilot %u", j + 1);
        /* A quad crosses a gate in ~100 ms; the 255 ms median filter RotorHazard
         * uses removes pulses shorter than ~130 ms (tests/lap_filter_test.c). */
        lap_set_filter(&seat[j].lap, LAP_FILTER_EMA);
    }
    gate_load();
    return ESP_OK;
}

bool gate_metric_is_snr(void) {
    scan_config_t c;
    scanner_get_config(&c);
    return c.metrics_mode == METRICS_SNR;
}

void gate_apply_plan(scan_config_t *c) {
    const profile_t *p = c->profile;
    c->nch = GATE_SEATS;
    c->enabled = 0;
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        snprintf(c->ch[j].name, sizeof(c->ch[j].name), "P%u", j + 1);
        uint32_t khz;
        if (seat[j].freq_mhz && profile_from_equiv_khz(p, seat[j].freq_mhz * 1000u, &khz)) {
            seat[j].mapped = true;
            c->ch[j].khz = khz;
            c->enabled |= 1u << j;
        } else {
            seat[j].mapped = false;
            c->ch[j].khz = p->ch[j < p->nch ? j : 0].khz; /* placeholder, not scanned */
        }
    }
    c->running = c->enabled != 0;
}

void gate_reset_levels(void) {
    portENTER_CRITICAL(&mux);
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        lap_reset(&seat[j].lap);
        seat[j].last_us = 0;
    }
    portEXIT_CRITICAL(&mux);
}

void gate_set_filter(lap_filter_t f) {
    portENTER_CRITICAL(&mux);
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        if (seat[j].lap.filter == f) continue;
        lap_set_filter(&seat[j].lap, f);
        seat[j].last_us = 0;
    }
    portEXIT_CRITICAL(&mux);
}

lap_filter_t gate_get_filter(void) { return (lap_filter_t)seat[0].lap.filter; }

/* Called with a new pass already accepted: queue it. */
static void queue_pass(unsigned j, uint32_t pass_ms, uint8_t peak) {
    gate_pass_t p = {.pilot = (uint8_t)(j + 1), .pass_ms = pass_ms, .peak = peak};
    portENTER_CRITICAL(&mux);
    p.lap_ms = seat[j].have_prev ? pass_ms - seat[j].prev_pass_ms : 0;
    seat[j].have_prev = true;
    seat[j].prev_pass_ms = pass_ms;
    p.count = ++seat[j].count;
    for (unsigned k = 0; k < GATE_SEATS; k++) p.lv[k] = seat[k].lap.valid ? seat[k].lap.level : 0;
    for (unsigned k = 0; k < GATE_SEATS; k++)
        if (k != j && seat[k].rp > p.xl) { p.xl = seat[k].rp; p.xp = (uint8_t)(k + 1); }
    portEXIT_CRITICAL(&mux);
    xQueueSend(passes, &p, 0); /* full: the oldest stays, newest is lost; LoRa task drains fast */
}

IRAM_ATTR void gate_feed(unsigned j, int64_t t_us, uint8_t level) {
    if (j >= GATE_SEATS) return;
    if (level < 1) level = 1;
    if (level > 254) level = 254;
    bool pass = false;
    uint32_t pass_ms = 0;
    uint8_t peak = 0;
    portENTER_CRITICAL(&mux);
    int64_t prev = seat[j].last_us;
    seat[j].last_us = t_us;
    lap_feed(&seat[j].lap, (uint32_t)(t_us / 1000), level, prev ? (uint32_t)(t_us - prev) : 0);
    if (seat[j].lap.valid && seat[j].lap.level > seat[j].hold) seat[j].hold = seat[j].lap.level;
    {
        uint8_t lv = seat[j].lap.valid ? seat[j].lap.level : 0, decay = seat[j].rp >> 5;
        seat[j].rp = lv >= seat[j].rp ? lv : (uint8_t)(seat[j].rp - (decay ? decay : 1));
    }
    if (cal_on && seat[j].lap.valid) {
        uint8_t v = seat[j].lap.level;
        if (v < seat[j].cal_lo) seat[j].cal_lo = v;
        if (v > seat[j].cal_hi) seat[j].cal_hi = v;
        seat[j].cal_sum += v;
        seat[j].cal_n++;
    }
    if (seat[j].lap.lap != seat[j].last_lap) {
        seat[j].last_lap = seat[j].lap.lap;
        pass_ms = seat[j].lap.pass_ms;
        peak = seat[j].lap.pass_peak;
        /* Ignore a second pass inside the minimum lap time (antenna wobble, a
         * pilot circling back through the gate). The detector already moved on. */
        pass = !seat[j].have_prev || pass_ms - seat[j].prev_pass_ms >= min_lap_ms;
    }
    portEXIT_CRITICAL(&mux);
    if (pass) queue_pass(j, pass_ms, peak);
}

void gate_cal_start(void) {
    portENTER_CRITICAL(&mux);
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        seat[j].cal_lo = 255;
        seat[j].cal_hi = 0;
        seat[j].cal_sum = seat[j].cal_n = 0;
    }
    cal_on = true;
    portEXIT_CRITICAL(&mux);
}

void gate_cal_finish(gate_cal_t *out) {
    portENTER_CRITICAL(&mux);
    cal_on = false;
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        bool any = seat[j].cal_n > 0;
        out->lo[j] = any ? seat[j].cal_lo : 0;
        out->hi[j] = any ? seat[j].cal_hi : 0;
        out->avg[j] = any ? (uint8_t)(seat[j].cal_sum / seat[j].cal_n) : 0;
    }
    portEXIT_CRITICAL(&mux);
}

void gate_sim_pass(unsigned pilot) {
    if (pilot < 1 || pilot > GATE_SEATS) return;
    queue_pass(pilot - 1, now_ms(), 200);
}

bool gate_next_pass(gate_pass_t *p, uint32_t timeout_ms) {
    return xQueueReceive(passes, p, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool gate_get_pilot(unsigned pilot, ll_pilot_cfg_t *out) {
    if (pilot < 1 || pilot > GATE_SEATS) return false;
    portENTER_CRITICAL(&mux);
    out->freq_mhz = seat[pilot - 1].freq_mhz;
    out->enter_at = seat[pilot - 1].lap.enter_at;
    out->exit_at = seat[pilot - 1].lap.exit_at;
    portEXIT_CRITICAL(&mux);
    return true;
}

bool gate_set_pilot(unsigned pilot, const ll_pilot_cfg_t *cfg) {
    if (pilot < 1 || pilot > GATE_SEATS) return false;
    if (cfg->enter_at < 1 || cfg->enter_at > 254 || cfg->exit_at < 1 || cfg->exit_at >= cfg->enter_at) return false;
    unsigned j = pilot - 1;
    scan_config_t c;
    scanner_get_config(&c);
    uint32_t khz;
    if (cfg->freq_mhz && (cfg->freq_mhz < 100 || !profile_from_equiv_khz(c.profile, cfg->freq_mhz * 1000u, &khz)))
        return false;
    portENTER_CRITICAL(&mux);
    bool changed = seat[j].freq_mhz != cfg->freq_mhz;
    seat[j].freq_mhz = cfg->freq_mhz;
    seat[j].lap.enter_at = cfg->enter_at;
    seat[j].lap.exit_at = cfg->exit_at;
    dirty_ms = now_ms() | 1;
    if (changed) {
        lap_reset(&seat[j].lap);
        seat[j].last_us = 0;
    }
    portEXIT_CRITICAL(&mux);
    if (changed) {
        gate_apply_plan(&c);
        scanner_set_config(&c);
    }
    return true;
}

void gate_reset_counts(void) {
    portENTER_CRITICAL(&mux);
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        seat[j].count = 0;
        seat[j].have_prev = false;
        seat[j].last_lap = seat[j].lap.lap;
    }
    portEXIT_CRITICAL(&mux);
    gate_pass_t drop;
    while (xQueueReceive(passes, &drop, 0) == pdTRUE) {}
}

uint16_t gate_min_lap_ms(void) { return min_lap_ms; }
void gate_set_min_lap_ms(uint16_t ms) { min_lap_ms = ms; dirty_ms = now_ms() | 1; }
uint8_t gate_id(void) { return id; }
void gate_set_id(uint8_t v) { id = v ? v : 1; dirty_ms = now_ms() | 1; }

void gate_view(gate_view_t *v) {
    memset(v, 0, sizeof(*v));
    portENTER_CRITICAL(&mux);
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        v->level[j] = seat[j].lap.valid ? seat[j].lap.level : 0;
        v->hold[j] = seat[j].hold > v->level[j] ? seat[j].hold : v->level[j];
        seat[j].hold = 0;
        v->enter[j] = seat[j].lap.enter_at;
        v->exit[j] = seat[j].lap.exit_at;
        v->count[j] = seat[j].count;
        if (seat[j].lap.crossing) v->crossing |= 1u << j;
        if (seat[j].mapped) v->enabled |= 1u << j;
    }
    portEXIT_CRITICAL(&mux);
}

/* Copy at most LL_NAME_LEN bytes without cutting a UTF-8 character, dropping what would break the
 * JSON lines or a terminal. */
static void copy_name(char *dst, const char *src) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && n < LL_NAME_LEN; p++) {
        if (*p < 0x20 || *p == 0x7F || *p == '"' || *p == '\\') continue;
        dst[n++] = (char)*p;
    }
    /* A truncation can leave the start of a multi-byte character at the end: drop the fragment. */
    size_t k = n;
    while (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) k--;
    if (k > 0) {
        unsigned char lead = (unsigned char)dst[k - 1];
        if (lead >= 0xC0) {
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (n - (k - 1) < need) n = k - 1;
        }
    }
    dst[n] = 0;
}

void gate_get_name(unsigned pilot, char out[LL_NAME_LEN + 1]) {
    out[0] = 0;
    if (pilot < 1 || pilot > GATE_SEATS) return;
    portENTER_CRITICAL(&mux);
    memcpy(out, names[pilot - 1], LL_NAME_LEN + 1);
    portEXIT_CRITICAL(&mux);
}

bool gate_set_name(unsigned pilot, const char *name) {
    if (pilot < 1 || pilot > GATE_SEATS) return false;
    char tmp[LL_NAME_LEN + 1];
    copy_name(tmp, name);
    if (!tmp[0]) snprintf(tmp, sizeof(tmp), "Pilot %u", pilot);
    portENTER_CRITICAL(&mux);
    bool changed = strcmp(names[pilot - 1], tmp) != 0;
    if (changed) {
        memcpy(names[pilot - 1], tmp, sizeof(tmp));
        meta_rev++;
    }
    portEXIT_CRITICAL(&mux);
    dirty_ms = now_ms() | 1;
    return true;
}

void gate_get_race(gate_race_t *out) { *out = race_opts; }

void gate_set_race(const gate_race_t *in) {
    gate_race_t r = *in;
    if (r.laps < 1) r.laps = 1;
    if (r.laps > 99) r.laps = 99;
    if (r.countdown > 30) r.countdown = 30;
    r.flags &= LL_SET_HOLESHOT;
    portENTER_CRITICAL(&mux);
    if (memcmp(&r, &race_opts, sizeof(r))) {
        race_opts = r;
        meta_rev++;
    }
    portEXIT_CRITICAL(&mux);
    dirty_ms = now_ms() | 1;
}

uint8_t gate_meta_rev(void) { return meta_rev; }

void gate_autosave_tick(void) {
    uint32_t d = dirty_ms;
    if (d && (uint32_t)(now_ms() - d) > 2000) {
        dirty_ms = 0;
        gate_save();
    }
}

esp_err_t gate_save(void) {
    saved_t s = {.version = SAVED_VERSION, .gate_id = id, .min_lap_ms = min_lap_ms};
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        gate_get_pilot(j + 1, &s.pilot[j]);
        gate_get_name(j + 1, s.name[j]);
    }
    s.laps = race_opts.laps;
    s.flags = race_opts.flags;
    s.countdown = race_opts.countdown;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, NVS_KEY_CFG, &s, sizeof(s));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* Restores into the seats directly (the scanner is not running yet at boot);
 * the caller applies the plan. */
void gate_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    saved_t s;
    memset(&s, 0, sizeof(s));
    size_t len = sizeof(s);
    uint8_t raw[sizeof(saved_t)];
    bool ok = false;
    if (nvs_get_blob(h, NVS_KEY_CFG, raw, &len) == ESP_OK) {
        if (len == sizeof(saved_t)) {
            memcpy(&s, raw, sizeof(s));
            ok = s.version == SAVED_VERSION;
        } else if (len == sizeof(saved_v2_t)) {  /* versions 1 and 2: no names or race options yet */
            memcpy(&s, raw, sizeof(saved_v2_t));
            ok = s.version == 1 || s.version == 2;
        }
    }
    if (ok) {
        id = s.gate_id ? s.gate_id : 1;
        min_lap_ms = s.min_lap_ms;
        for (unsigned j = 0; j < GATE_SEATS; j++) {
            const ll_pilot_cfg_t *c = &s.pilot[j];
            if (c->enter_at < 1 || c->enter_at > 254 || c->exit_at < 1 || c->exit_at >= c->enter_at) continue;
            seat[j].freq_mhz = c->freq_mhz;
            if (s.version == 1) continue; /* old RSSI-scale thresholds mean nothing in SNR mode */
            seat[j].lap.enter_at = c->enter_at;
            seat[j].lap.exit_at = c->exit_at;
        }
        if (s.version == SAVED_VERSION) {
            for (unsigned j = 0; j < GATE_SEATS; j++) {
                s.name[j][LL_NAME_LEN] = 0;
                if (s.name[j][0]) memcpy(names[j], s.name[j], LL_NAME_LEN + 1);
            }
            if (s.laps >= 1 && s.laps <= 99) race_opts.laps = s.laps;
            race_opts.flags = s.flags & LL_SET_HOLESHOT;
            if (s.countdown <= 30) race_opts.countdown = s.countdown;
        }
    }
    nvs_close(h);
}
