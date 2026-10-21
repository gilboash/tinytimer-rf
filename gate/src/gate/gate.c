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

/* Before the dashboard sets thresholds: RSSI codes (0.25 dB), -32.5 / -37.5 dBFS. */
#define DEFAULT_ENTER 125
#define DEFAULT_EXIT 105
#define DEFAULT_MIN_LAP_MS 3000
#define NVS_NAMESPACE "gate"
#define NVS_KEY_CFG "cfg"
#define SAVED_VERSION 1

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; /* seats: scanner vs command task */
/* On the heap: the median windows (~5 KB) don't fit in the C5's static RAM
 * below the RF dump bank. */
static struct {
    uint16_t freq_mhz;   /* 0: disabled */
    bool mapped;         /* the profile can tune it */
    int64_t last_us;
    uint8_t count;       /* passes since gate_reset_counts */
    uint8_t last_lap;    /* lap.lap when we last looked */
    bool have_prev;
    uint32_t prev_pass_ms;
    lap_seat_t lap;
} *seat;
static uint16_t min_lap_ms = DEFAULT_MIN_LAP_MS;
static uint8_t id = 1;
static QueueHandle_t passes;

typedef struct {
    uint8_t version, gate_id;
    uint16_t min_lap_ms;
    ll_pilot_cfg_t pilot[GATE_SEATS];
} saved_t;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t gate_init(void) {
    seat = heap_caps_calloc(GATE_SEATS, sizeof(*seat), MALLOC_CAP_INTERNAL);
    passes = xQueueCreate(32, sizeof(gate_pass_t));
    if (!seat || !passes) return ESP_ERR_NO_MEM;
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        lap_init(&seat[j].lap, DEFAULT_ENTER, DEFAULT_EXIT);
        /* A quad crosses a gate in ~100 ms; the 255 ms median filter RotorHazard
         * uses removes pulses shorter than ~130 ms (tests/lap_filter_test.c). */
        lap_set_filter(&seat[j].lap, LAP_FILTER_EMA);
    }
    gate_load();
    return ESP_OK;
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
void gate_set_min_lap_ms(uint16_t ms) { min_lap_ms = ms; }
uint8_t gate_id(void) { return id; }
void gate_set_id(uint8_t v) { id = v ? v : 1; }

void gate_view(gate_view_t *v) {
    memset(v, 0, sizeof(*v));
    portENTER_CRITICAL(&mux);
    for (unsigned j = 0; j < GATE_SEATS; j++) {
        v->level[j] = seat[j].lap.valid ? seat[j].lap.level : 0;
        v->enter[j] = seat[j].lap.enter_at;
        v->exit[j] = seat[j].lap.exit_at;
        v->count[j] = seat[j].count;
        if (seat[j].lap.crossing) v->crossing |= 1u << j;
        if (seat[j].mapped) v->enabled |= 1u << j;
    }
    portEXIT_CRITICAL(&mux);
}

esp_err_t gate_save(void) {
    saved_t s = {.version = SAVED_VERSION, .gate_id = id, .min_lap_ms = min_lap_ms};
    for (unsigned j = 0; j < GATE_SEATS; j++) gate_get_pilot(j + 1, &s.pilot[j]);
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
    size_t len = sizeof(s);
    if (nvs_get_blob(h, NVS_KEY_CFG, &s, &len) == ESP_OK && len == sizeof(s) && s.version == SAVED_VERSION) {
        id = s.gate_id ? s.gate_id : 1;
        min_lap_ms = s.min_lap_ms;
        for (unsigned j = 0; j < GATE_SEATS; j++) {
            const ll_pilot_cfg_t *c = &s.pilot[j];
            if (c->enter_at < 1 || c->enter_at > 254 || c->exit_at < 1 || c->exit_at >= c->enter_at) continue;
            seat[j].freq_mhz = c->freq_mhz;
            seat[j].lap.enter_at = c->enter_at;
            seat[j].lap.exit_at = c->exit_at;
        }
    }
    nvs_close(h);
}
