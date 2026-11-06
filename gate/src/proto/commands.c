/* Host protocol: ASCII command lines over USB Serial/JTAG. Two modes:
 *   GATE    (boot default) the lap timer: per-pilot seats, thresholds, passes,
 *           LAP / LEVELS lines, LoRa uplink (gate/). Only the commands in
 *           gate_allowed plus the gate commands in handle_gate work.
 *   MONITOR the Widscan58 scanner: every command, binary frames and spectra
 *           for the debug view. The LoRa task keeps running.
 * CONFIG SAVE keeps GATE mode's gain, FFT size, level metric, LO plan and
 * level filter in NVS; every switch to GATE mode (and boot) applies them. A
 * switch to MONITOR applies METRICS FULL. GPL-3.0-or-later. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "commands.h"
#include "frames.h"
#include "gpio_out.h"
#include "link.h"
#include "metrics.h"
#include "radio.h"
#include "scanner.h"
#include "psd.h"
#include "gate.h"
#include "gate_io.h"
#include "lora_link.h"

/* Multi-part replies are assembled here and queued as one item: the scanner
 * task queues frames concurrently, and a frame landing between the pieces of
 * a line cuts it in two on the host. Only the command task uses it. */
#define REPLY_BYTES 3072
static char *reply; /* heap: static SRAM goes to hot code in IRAM on the C5 */

static size_t reply_len;
static bool reply_overflow;

static void reply_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply_add(const char *fmt, ...) {
    if (!reply) reply_overflow = true;
    if (reply_overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(reply + reply_len, REPLY_BYTES - reply_len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= REPLY_BYTES - reply_len) reply_overflow = true;
    else reply_len += (size_t)n;
}

static void reply_send(void) {
    link_text(reply_overflow ? "ERR reply_length\n" : reply);
    reply_len = 0;
    reply_overflow = false;
    if (reply) reply[0] = 0;
}

static const char *metrics_mode_name(unsigned m) {
    return m == METRICS_RSSI ? "RSSI" : m == METRICS_SNR ? "SNR" : "FULL";
}

static const char *level_filter_name(lap_filter_t f) { return f == LAP_FILTER_EMA ? "EMA" : "MEDIAN"; }

static void send_profile(const scan_config_t *c) {
    const profile_t *p = c->profile;
    reply_add("PROFILE {\"name\":\"%s\",\"description\":\"%s\",\"target\":\"%s\","
              "\"band_khz\":[%" PRIu32 ",%" PRIu32 "],\"base_khz\":%" PRIu32 ",\"equiv_base_khz\":%" PRIu32
              ",\"scale\":%.6f,\"rate_hz\":%" PRIu32 ",\"usable_hz\":%" PRIu32 ",\"passband_hz\":%" PRIu32
              ",\"rssi_hz\":%" PRIu32 ",\"search_hz\":%" PRIu32 ",\"channels\":[",
              p->name, p->description, radio_caps()->target, p->band_lo_khz, p->band_hi_khz,
              p->base_khz, p->equiv_base_khz, (double)p->scale, radio_caps()->rates_hz[c->rate_code],
              p->usable_hz, p->passband_hz, c->rssi_hz, c->search_hz);
    for (unsigned j = 0; j < c->nch; j++)
        reply_add("%s{\"id\":%u,\"name\":\"%s\",\"khz\":%" PRIu32 "}", j ? "," : "", j, c->ch[j].name, c->ch[j].khz);
    reply_add("]}\n");
    reply_send();
}

typedef enum { PROTO_GATE, PROTO_MONITOR } proto_t;
static proto_t proto = PROTO_GATE;
/* Scan configuration of the protocol not in use, restored on a switch. */
static scan_config_t other_cfg;

/* In GATE mode the capture is always four FFT segments. */
#define GATE_SEGMENTS 4
static void gate_set_fft(scan_config_t *c, unsigned nfft) {
    c->nfft = (uint16_t)nfft;
    c->capture_samples = (uint16_t)(GATE_SEGMENTS * nfft);
    c->segments = GATE_SEGMENTS;
}

/* GATE mode defaults: per-channel levels only, small FFT, no spectrum,
 * manual gain 45 (set here so a monitor-mode gain never carries over; PLAN.md,
 * gain sweep) and lap.c's 255 ms median filter. */
static void gate_defaults(scan_config_t *c) {
    c->gate = true;
    c->gain_mode = RADIO_GAIN_MANUAL;
    c->gain = 45;
    c->metrics_mode = METRICS_SNR; /* level above each channel's own noise floor: one threshold fits every pilot */
    gate_set_fft(c, 64);
    c->spec_every = 0;
    c->mode = PLAN_GROUP;
    gate_apply_plan(c);
}

/* GATE mode settings saved by CONFIG SAVE (NVS blob, layout by version).
 * The capture length follows the FFT size (gate_set_fft). Versions 1 and 2
 * start with the same 8 bytes as this one; 1 then stored the capture
 * length. Both are read with the median filter (they predate LEVELFILTER). */
#define GATE_SAVED_VERSION 3
typedef struct {
    uint8_t version;
    uint8_t gain_mode, gain, metrics_mode, mode;
    uint16_t nfft;
    uint8_t filter; /* lap_filter_t */
} gate_saved_t;
#define GATE_SAVED_V2_LEN 8
#define GATE_SAVED_V1_LEN 10
_Static_assert(offsetof(gate_saved_t, filter) == GATE_SAVED_V2_LEN, "versions 1 and 2 are a prefix");
static gate_saved_t gate_saved; /* applied on every switch to GATE mode */
static gate_saved_t gate_builtin; /* the defaults without a save (boot) */
#define NVS_NAMESPACE "widscan58"
#define NVS_KEY_GATE "gate"

static void gate_saved_from(gate_saved_t *s, const scan_config_t *c) {
    *s = (gate_saved_t){.version = GATE_SAVED_VERSION, .gain_mode = (uint8_t)c->gain_mode, .gain = c->gain,
                      .metrics_mode = c->metrics_mode, .mode = (uint8_t)c->mode, .nfft = c->nfft,
                      .filter = (uint8_t)gate_get_filter()};
}

static void gate_saved_apply(scan_config_t *c, const gate_saved_t *s) {
    c->gain_mode = (radio_gain_mode_t)s->gain_mode;
    c->gain = s->gain;
    c->metrics_mode = s->metrics_mode;
    c->mode = (plan_mode_t)s->mode;
    gate_set_fft(c, s->nfft);
    gate_set_filter((lap_filter_t)s->filter);
}

/* The same limits as the commands that set these. */
static bool gate_saved_valid(const gate_saved_t *s) {
    return
           (s->gain_mode == RADIO_GAIN_HARDWARE || (s->gain_mode == RADIO_GAIN_MANUAL && s->gain <= radio_caps()->gain_max)) &&
           (s->metrics_mode == METRICS_RSSI || s->metrics_mode == METRICS_SNR) &&
           (s->mode == PLAN_GROUP || s->mode == PLAN_SINGLE) && s->nfft >= 64 && s->nfft <= PSD_MAX_FFT &&
           !(s->nfft & (s->nfft - 1)) && (s->filter == LAP_FILTER_MEDIAN || s->filter == LAP_FILTER_EMA);
}

/* gate_saved from NVS; left alone (the defaults) when there is none. */
static void gate_saved_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t b[16] = {0};
    size_t len = sizeof(b);
    gate_saved_t s = {0};
    if (nvs_get_blob(h, NVS_KEY_GATE, b, &len) == ESP_OK) {
        bool ok = (b[0] == GATE_SAVED_VERSION && len == sizeof(s)) || (b[0] == 2 && len == GATE_SAVED_V2_LEN) ||
                  (b[0] == 1 && len == GATE_SAVED_V1_LEN);
        memcpy(&s, b, b[0] == GATE_SAVED_VERSION ? sizeof(s) : GATE_SAVED_V2_LEN);
        if (b[0] != GATE_SAVED_VERSION) s.filter = LAP_FILTER_MEDIAN;
        if (ok && gate_saved_valid(&s)) {
            gate_saved = s;
            gate_saved.version = GATE_SAVED_VERSION;
        }
    }
    nvs_close(h);
}

static esp_err_t gate_saved_erase(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_erase_key(h, NVS_KEY_GATE);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t gate_saved_store(const gate_saved_t *s) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, NVS_KEY_GATE, s, sizeof(*s));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* Switch protocols, or with p == proto just apply the mode's settings again. */
static void set_proto(proto_t p) {
    scan_config_t cur, next;
    scanner_get_config(&cur);
    if (p == proto) {
        next = cur;
    } else {
        next = other_cfg;
        other_cfg = cur;
    }
    if (p == PROTO_GATE) {
        uint8_t was = next.metrics_mode;
        gate_saved_apply(&next, &gate_saved);
        if (next.metrics_mode != was) gate_reset_levels(); /* RSSI and SNR codes don't compare */
        gate_apply_plan(&next); /* the seats may have changed meanwhile */
        link_set_streaming(false);
        link_flush();
    } else {
        next.metrics_mode = METRICS_FULL;
    }
    scanner_set_config(&next);
    if (p == PROTO_MONITOR) link_set_streaming(true);
    proto = p;
}

static bool gate_allowed(const char *line) {
    static const char *const exact[] = {"PROTOCOL?", "PROTOCOL MONITOR", "PROTOCOL GATE", "INFO", "CONFIG?",
                                        "METRICS RSSI", "METRICS SNR", "METRICS?", "GAIN?", "GAIN HARDWARE",
                                        "FILTER AUTO", "MODE GROUP", "MODE SINGLE", "CONFIG SAVE",
                                        "CONFIG RESET", "RECAL", "LEVELFILTER MEDIAN", "LEVELFILTER EMA",
                                        "LEVELFILTER?", "PHYIDLE?", "GPIO?", "GATE?", "GATE RESET", "GATE SAVE", "LORA?", "STREAM?"};
    static const char *const prefix[] = {"GAIN MANUAL ", "FFT ", "FILTER ", "IQCORR ", "PHYIDLE ", "THROTTLE ", "GPIO ", "PILOT ", "THRESH ", "NAME ", "RACESET ", "GATE ", "STREAM ", "SIMPASS ", "LORA "};
    for (unsigned j = 0; j < sizeof(exact) / sizeof(exact[0]); j++)
        if (!strcmp(line, exact[j])) return true;
    for (unsigned j = 0; j < sizeof(prefix) / sizeof(prefix[0]); j++)
        if (!strncmp(line, prefix[j], strlen(prefix[j]))) return true;
    return false;
}

static void send_status(const char *keyword) {
    scan_config_t c;
    plan_t plan;
    scanner_get_config(&c);
    scanner_get_plan(&plan);
    reply_add("%s {\"protocol\":\"%s\",\"profile\":\"%s\",\"running\":%d,\"enabled\":%" PRIu32 ",\"mode\":\"%s\","
              "\"capture_samples\":%u,\"nfft\":%u,\"segments\":%u,\"rate_code\":%u,\"filter\":%d,"
              "\"conjugate\":%d,\"iq_corr\":%d,\"settle_us\":%" PRIu32 ",\"tskip\":%u,\"gain_mode\":\"%s\",\"gain\":%u,\"gain_max\":%u,\"rawiq_max\":%u,"
              "\"recal\":\"%s\",\"metrics\":\"%s\",\"rssi_hz\":%" PRIu32 ",\"search_hz\":%" PRIu32 ",\"dc_guard\":%u,"
              "\"ema\":%.3f,\"clear_ema\":%.3f,\"signal_db\":%.1f,\"avg_shift\":%u,\"spec_every\":%u,\"spec_bins\":%u,"
              "\"spec_span_hz\":%" PRIu32 ",\"spec_detector\":%u,\"fft\":\"%s\",\"level_filter\":\"%s\",\"throttle_hz\":%" PRIu32 ","
              "\"phy_idle\":%u,\"phy_idle_min_us\":%" PRIu32 ",\"plan\":[",
              keyword, proto == PROTO_GATE ? "GATE" : "MONITOR", c.profile->name, c.running, c.enabled, c.mode == PLAN_GROUP ? "GROUP" : "SINGLE",
              c.capture_samples, c.nfft, c.segments, c.rate_code, c.filter, c.conjugate, c.iq_corr, c.settle_us, radio_get_tune_skip(),
              c.gain_mode == RADIO_GAIN_MANUAL ? "MANUAL" : "HARDWARE", c.gain, radio_caps()->gain_max, (unsigned)radio_caps()->max_samples,
              c.recal == RADIO_RECAL_ONCE ? "ONCE" : c.recal == RADIO_RECAL_CACHE ? "CACHE" : "EVERY",
              metrics_mode_name(c.metrics_mode), c.rssi_hz, c.search_hz, c.dc_guard,
              (double)c.ema_alpha, (double)c.clear_alpha, (double)c.signal_snr_db, c.avg_shift, c.spec_every,
              c.spec_bins, c.spec_span_hz, c.spec_detector, psd_fft_name((psd_fft_t)c.fft_type),
              level_filter_name(gate_get_filter()), c.max_rotation_hz, c.phy_idle, c.phy_idle_min_us);
    for (unsigned j = 0; j < plan.nhops; j++) {
        const plan_hop_t *h = &plan.hop[j];
        reply_add("%s{\"lo_khz\":%" PRIu32 ",\"ch\":[", j ? "," : "", h->lo_khz);
        for (unsigned k = 0; k < h->nch; k++) reply_add("%s%u", k ? "," : "", h->ch[k]);
        reply_add("],\"offset_hz\":[");
        for (unsigned k = 0; k < h->nch; k++) reply_add("%s%" PRId32, k ? "," : "", h->offset_hz[k]);
        reply_add("]}");
    }
    reply_add("]}\n");
    reply_send();
}

/* GPIO?: per GPIO number, '-' unused, 'L' low, 'H' high, 'x' not available
 * (flash, USB); "active" is what is set now, "saved" what boot applies. */
static void send_gpio(void) {
    char s[2][65];
    unsigned n = gpio_out_count();
    if (n > 64) n = 64;
    for (unsigned k = 0; k < 2; k++) {
        for (unsigned j = 0; j < n; j++)
            s[k][j] = !gpio_out_allowed(j) ? 'x' : "-LH"[gpio_out_get(j, k)];
        s[k][n] = 0;
    }
    reply_add("GPIO {\"count\":%u,\"active\":\"%s\",\"saved\":\"%s\"}\n", n, s[0], s[1]);
    reply_send();
}

static bool parse_u32(const char *s, uint32_t *v) {
    char *end;
    unsigned long x = strtoul(s, &end, 0);
    if (end == s || *end) return false;
    *v = (uint32_t)x;
    return true;
}

/* CHANNELS <name>=<MHz> ...: up to PROFILE_MAX_CHANNELS channels, each given
 * by its 5.8 GHz frequency; a scaled profile maps it into its own band. */
static bool parse_channels(char *args, const profile_t *p, scan_config_t *c) {
    unsigned n = 0;
    char *save;
    for (char *tok = strtok_r(args, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        uint32_t mhz;
        if (n == PROFILE_MAX_CHANNELS || !eq || eq == tok || eq - tok >= (int)sizeof(c->ch[0].name)) return false;
        *eq = 0;
        for (const char *q = tok; *q; q++)
            if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9'))) return false;
        if (!parse_u32(eq + 1, &mhz) || mhz > 100000 || !profile_from_equiv_khz(p, mhz * 1000, &c->ch[n].khz))
            return false;
        strcpy(c->ch[n].name, tok);
        n++;
    }
    if (!n) return false;
    c->nch = (uint8_t)n;
    c->enabled = (1u << n) - 1;
    return true;
}


/* ---- Lap-timer commands (GATE mode) ---- */

static void send_gate(void) {
    gate_race_t rc;
    gate_get_race(&rc);
    reply_add("GATE {\"id\":%u,\"min_lap_ms\":%u,\"metric\":\"%s\",\"rev\":%u,\"race\":{\"laps\":%u,\"holeshot\":%u,\"countdown\":%u},\"pilots\":[",
              gate_id(), (unsigned)gate_min_lap_ms(), gate_metric_is_snr() ? "snr" : "rssi", gate_meta_rev(), rc.laps,
              (rc.flags & LL_SET_HOLESHOT) ? 1u : 0u, rc.countdown);
    gate_view_t v;
    gate_view(&v);
    for (unsigned n = 1; n <= GATE_SEATS; n++) {
        ll_pilot_cfg_t c = {0};
        gate_get_pilot(n, &c);
        char nm[LL_NAME_LEN + 1];
        gate_get_name(n, nm);
        reply_add("%s{\"n\":%u,\"name\":\"%s\",\"freq\":%u,\"enter\":%u,\"exit\":%u,\"count\":%u}", n > 1 ? "," : "", n,
                  nm, c.freq_mhz, c.enter_at, c.exit_at, v.count[n - 1]);
    }
    reply_add("]}\n");
    reply_send();
}

/* Returns true if the line was a lap-timer command. */
static bool handle_gate(char *line) {
    uint32_t n, a, b;
    char extra;
    if (!strcmp(line, "GATE?")) {
        send_gate();
    } else if (sscanf(line, "PILOT %" SCNu32 " %" SCNu32 " %c", &n, &a, &extra) == 2 && a <= 9999) {
        /* PILOT <1-8> <MHz | 0 = off>: frequency only, thresholds kept. */
        ll_pilot_cfg_t c;
        if (!gate_get_pilot(n, &c)) { link_text("ERR pilot\n"); return true; }
        c.freq_mhz = (uint16_t)a;
        link_text(gate_set_pilot(n, &c) ? "OK\n" : "ERR freq\n");
    } else if (sscanf(line, "THRESH %" SCNu32 " %" SCNu32 " %" SCNu32 " %c", &n, &a, &b, &extra) == 3) {
        /* THRESH <1-8> <enter> <exit>: level codes 1..254, exit < enter. */
        ll_pilot_cfg_t c;
        if (!gate_get_pilot(n, &c) || a > 254 || b > 254) { link_text("ERR thresh\n"); return true; }
        c.enter_at = (uint8_t)a;
        c.exit_at = (uint8_t)b;
        link_text(gate_set_pilot(n, &c) ? "OK\n" : "ERR thresh\n");
    } else if (!strncmp(line, "NAME ", 5) && sscanf(line + 5, "%" SCNu32, &n) == 1 && n >= 1 && n <= GATE_SEATS) {
        /* NAME <1-8> <text>: the rest of the line, up to 12 bytes. */
        const char *t = line + 5;
        while (*t >= '0' && *t <= '9') t++;
        while (*t == ' ') t++;
        link_text(gate_set_name(n, t) ? "OK\n" : "ERR name\n");
    } else if (sscanf(line, "RACESET %" SCNu32 " %" SCNu32 " %" SCNu32 " %c", &a, &b, &n, &extra) == 3 && a >= 1 && a <= 99 && n <= 30) {
        /* RACESET <laps> <holeshot 0|1> <countdown s> */
        gate_race_t r = {(uint8_t)a, b ? LL_SET_HOLESHOT : 0, (uint8_t)n};
        gate_set_race(&r);
        link_text("OK\n");
    } else if (!strcmp(line, "GATE RESET")) {
        gate_reset_counts();
        link_text("OK\n");
    } else if (!strcmp(line, "GATE SAVE")) {
        esp_err_t e = gate_save();
        if (e != ESP_OK) link_printf("ERR save %s\n", esp_err_to_name(e));
        else link_text("OK\n");
    } else if (sscanf(line, "GATE ID %" SCNu32 " %c", &a, &extra) == 1 && a >= 1 && a <= 254) {
        gate_set_id((uint8_t)a);
        link_text("OK\n");
    } else if (sscanf(line, "GATE MINLAP %" SCNu32 " %c", &a, &extra) == 1 && a <= 60000) {
        gate_set_min_lap_ms((uint16_t)a);
        link_text("OK\n");
    } else if (sscanf(line, "STREAM %" SCNu32 " %c", &a, &extra) == 1 && a <= 50) {
        gate_io_set_stream_hz(a);
        link_text("OK\n");
    } else if (!strcmp(line, "STREAM?")) {
        link_printf("STREAM %u\n", gate_io_stream_hz());
    } else if (sscanf(line, "SIMPASS %" SCNu32 " %c", &n, &extra) == 1 && n >= 1 && n <= GATE_SEATS) {
        gate_sim_pass(n);
        link_text("OK\n");
    } else if (!strcmp(line, "LORA?")) {
        lora_stats_t st;
        lora_link_stats(&st);
        link_printf("LORA {\"up\":%d,\"err\":%d,\"freq\":%.1f,\"sf\":%d,\"tx\":%u,\"acked\":%u,\"retries\":%u,"
                    "\"dropped\":%u,\"pending\":%u,\"rx\":%u,\"cfg_rx\":%u,\"rssi\":%d,\"snr\":%d}\n",
                    st.up, st.err, (double)st.freq_mhz, st.sf, st.tx, st.acked, st.retries, st.dropped, st.pending,
                    st.rx, st.cfg_rx, st.last_rssi, st.last_snr);
    } else if (!strcmp(line, "LORA DIAG")) {
        lora_link_diag();
    } else if (!strcmp(line, "LORA PING")) {
        lora_link_request_status();
        link_text("OK\n");
    } else {
        return false;
    }
    return true;
}

static void handle(char *line) {
    if (proto == PROTO_GATE && !gate_allowed(line)) {
        link_text("ERR gate_mode\n");
        return;
    }
    if (proto == PROTO_GATE && handle_gate(line)) return;
    scan_config_t c;
    scanner_get_config(&c);
    char arg[32], arg2[32];
    uint32_t a, b, d;
    int32_t sa, sb, sd;
    char extra;

    if (!strcmp(line, "INFO")) {
        /* fw_ver is PROJECT_VER (git describe, see firmware/CMakeLists.txt); the
         * web UI compares it with the build it offers to flash. Then HEAD's
         * commit time and the PHY library's version. Fields are separated by
         * '|', which none of them contains (the PHY version has spaces). */
        link_printf("INFO lapgate|%s|%d|%s|%s|%s\n", radio_caps()->target, PROTO_VERSION,
                    esp_app_get_description()->version, FW_COMMIT_DATE, radio_phy_version());
    } else if (!strcmp(line, "PROFILE?")) {
        send_profile(&c);
    } else if (!strcmp(line, "CHANNELS DEFAULT")) {
        c.nch = c.profile->nch;
        memcpy(c.ch, c.profile->ch, sizeof(c.ch));
        c.enabled = (1u << c.nch) - 1;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strncmp(line, "CHANNELS ", 9)) {
        if (!parse_channels(line + 9, c.profile, &c)) { link_text("ERR channels\n"); return; }
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "STATUS?") || !strcmp(line, "CONFIG?")) {
        send_status(line[0] == 'S' ? "STATUS" : "CONFIG");
    } else if (!strcmp(line, "CONFIG SAVE")) {
        /* Starts with 'C', not 'S': GATE mode is the only one that can save. */
        if (proto != PROTO_GATE) { link_text("ERR gate_only\n"); return; }
        gate_saved_t s;
        gate_saved_from(&s, &c);
        esp_err_t e = gate_saved_store(&s);
        if (e != ESP_OK) { link_printf("ERR save %s\n", esp_err_to_name(e)); return; }
        gate_saved = s;
        link_text("OK\n");
    } else if (!strcmp(line, "CONFIG RESET")) {
        if (proto != PROTO_GATE) { link_text("ERR gate_only\n"); return; }
        esp_err_t e = gate_saved_erase();
        if (e != ESP_OK) { link_printf("ERR reset %s\n", esp_err_to_name(e)); return; }
        gate_saved = gate_builtin;
        set_proto(PROTO_GATE); /* apply them now */
        link_text("OK\n");
    } else if (!strcmp(line, "LEVELFILTER MEDIAN") || !strcmp(line, "LEVELFILTER EMA")) {
        /* GATE mode only: every switch to it applies the saved filter. */
        if (proto != PROTO_GATE) { link_text("ERR gate_only\n"); return; }
        gate_set_filter(line[12] == 'E' ? LAP_FILTER_EMA : LAP_FILTER_MEDIAN);
        link_text("OK\n");
    } else if (!strcmp(line, "LEVELFILTER?")) {
        link_printf("LEVELFILTER %s\n", level_filter_name(gate_get_filter()));
    } else if (!strcmp(line, "PROTOCOL?")) {
        link_printf("PROTOCOL %s\n", proto == PROTO_GATE ? "GATE" : "MONITOR");
    } else if (!strcmp(line, "PROTOCOL GATE") || !strcmp(line, "PROTOCOL MONITOR")) {
        set_proto(line[9] == 'G' ? PROTO_GATE : PROTO_MONITOR);
        link_text("OK\n");
    } else if (!strcmp(line, "ENABLE?")) {
        link_printf("ENABLE %" PRIu32 "\n", c.enabled);
    } else if (sscanf(line, "ENABLE %31s %c", arg, &extra) == 1 && parse_u32(arg, &a)) {
        c.enabled = a & ((1u << c.nch) - 1);
        scanner_set_config(&c);
        link_printf("OK ENABLE %" PRIu32 "\n", c.enabled);
    } else if (!strcmp(line, "MODE GROUP") || !strcmp(line, "MODE SINGLE")) {
        c.mode = line[5] == 'G' ? PLAN_GROUP : PLAN_SINGLE;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "SCAN START") || !strcmp(line, "SCAN STOP")) {
        c.running = line[5] == 'S' && line[6] == 'T' && line[7] == 'A';
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "GAIN HARDWARE")) {
        c.gain_mode = RADIO_GAIN_HARDWARE;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "GAIN MANUAL %" SCNu32 " %c", &a, &extra) == 1) {
        if (a > radio_caps()->gain_max) { link_text("ERR gain_range\n"); return; }
        c.gain_mode = RADIO_GAIN_MANUAL;
        c.gain = (uint8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "GAIN?")) {
        link_printf("GAIN %s %u 0 %u\n", c.gain_mode == RADIO_GAIN_MANUAL ? "MANUAL" : "HARDWARE", c.gain,
                    radio_caps()->gain_max);
    } else if (!strcmp(line, "FILTER AUTO")) {
        c.filter = -1;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "FILTER %" SCNu32 " %c", &a, &extra) == 1 && a <= radio_caps()->filter_code_max) {
        c.filter = (int8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "CAPN %" SCNu32 " %c", &a, &extra) == 1 && a >= 256 && a <= radio_caps()->max_samples) {
        c.capture_samples = (uint16_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "FFT %" SCNu32 " %c", &a, &extra) == 1 && a >= 64 && a <= PSD_MAX_FFT && !(a & (a - 1))) {
        if (proto == PROTO_GATE) gate_set_fft(&c, a);
        else c.nfft = (uint16_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SEGMENTS %" SCNu32 " %c", &a, &extra) == 1 && a >= 1 && a <= 64) {
        c.segments = (uint8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "RATE %" SCNu32 " %c", &a, &extra) == 1 && a < RADIO_MAX_RATES && radio_caps()->rates_hz[a]) {
        c.rate_code = (uint8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SETTLE %" SCNu32 " %c", &a, &extra) == 1 && a <= 20000) {
        c.settle_us = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "CONJ %" SCNu32 " %c", &a, &extra) == 1 && a <= 1) {
        c.conjugate = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "IQCORR %" SCNu32 " %c", &a, &extra) == 1 && a <= 1) {
        c.iq_corr = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "RECALMODE ONCE") || !strcmp(line, "RECALMODE EVERY") || !strcmp(line, "RECALMODE CACHE")) {
        c.recal = line[10] == 'O' ? RADIO_RECAL_ONCE : line[10] == 'C' ? RADIO_RECAL_CACHE : RADIO_RECAL_EVERY_LO;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "RECAL")) {
        scan_request_t r = {.type = SCAN_REQ_RECAL};
        if (!scanner_request(&r)) link_text("ERR busy\n");
    } else if (sscanf(line, "WIN %" SCNu32 " %" SCNu32 " %c", &a, &b, &extra) == 2 && a > 0 && b >= a && b <= 20000000) {
        c.rssi_hz = a;
        c.search_hz = b;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "METRICS FULL") || !strcmp(line, "METRICS RSSI") || !strcmp(line, "METRICS SNR")) {
        uint8_t m = line[8] == 'F' ? METRICS_FULL : line[8] == 'R' ? METRICS_RSSI : METRICS_SNR;
        if (proto == PROTO_GATE && m != c.metrics_mode) gate_reset_levels(); /* RSSI and SNR codes don't compare */
        c.metrics_mode = m;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "METRICS?")) {
        link_printf("METRICS %s\n", metrics_mode_name(c.metrics_mode));
    } else if (sscanf(line, "AVG %" SCNu32 " %c", &a, &extra) == 1 && a >= METRICS_AVG_SHIFT_MIN &&
               a <= METRICS_AVG_SHIFT_MAX) {
        c.avg_shift = (uint8_t)a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "EMA %" SCNu32 " %" SCNu32 " %c", &a, &b, &extra) == 2 && a >= 1 && a <= 1000 && b >= 1 && b <= 1000) {
        c.ema_alpha = a / 1000.0f;
        c.clear_alpha = b / 1000.0f;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SPEC %" SCNu32 " %" SCNu32 " %" SCNu32 " %31s %c", &a, &b, &d, arg2, &extra) == 4 &&
               a <= 255 && b >= 16 && b <= 1024 && (!strcmp(arg2, "MAX") || !strcmp(arg2, "MEAN"))) {
        /* SPEC <every_n_hops (0=off)> <bins> <span_hz one-sided> <MAX|MEAN> */
        c.spec_every = (uint8_t)a;
        c.spec_bins = (uint16_t)b;
        c.spec_span_hz = d;
        c.spec_detector = arg2[1] == 'A';
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "RAWIQ %" SCNu32 " %" SCNu32 " %" SCNu32 " %c", &a, &b, &d, &extra) >= 2) {
        /* RAWIQ <lo_khz> <samples> [rate_code] */
        int fields = sscanf(line, "RAWIQ %" SCNu32 " %" SCNu32 " %" SCNu32, &a, &b, &d);
        if (fields < 3) d = c.rate_code;
        if (b < 16 || b > radio_caps()->max_samples || d >= RADIO_MAX_RATES ||
            !radio_caps()->rates_hz[d] || a % radio_caps()->lo_step_khz) {
            link_text("ERR rawiq_args\n");
            return;
        }
        scan_request_t r = {.type = SCAN_REQ_RAWIQ, .lo_khz = a, .n = (uint16_t)b, .rate_code = (uint8_t)d};
        if (!scanner_request(&r)) link_text("ERR busy\n");
    } else if (sscanf(line, "SYNC %31s %c", arg, &extra) == 1) {
        link_printf("SYNC %s\n", arg);
    } else if (sscanf(line, "FFTTYPE %31s %c", arg, &extra) == 1 && strcmp(arg, "?")) {
        unsigned t = 0;
        while (t < PSD_FFT_COUNT && strcmp(arg, psd_fft_name((psd_fft_t)t))) t++;
        if (t == PSD_FFT_COUNT) { link_text("ERR ffttype\n"); return; }
        c.fft_type = (uint8_t)t;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "FFTTYPE?")) {
        link_printf("FFTTYPE %s\n", psd_fft_name((psd_fft_t)c.fft_type));
    } else if (sscanf(line, "FFTEVAL SYNTH %" SCNd32 " %" SCNd32 " %" SCNd32 " %c", &sa, &sb, &sd, &extra) == 3) {
        scan_request_t r = {.type = SCAN_REQ_FFTEVAL_SYNTH, .p1 = sa, .p2 = sb, .p3 = sd};
        if (!scanner_request(&r)) link_text("ERR busy\n");
    } else if (sscanf(line, "FFTEVAL LIVE %" SCNu32 " %" SCNd32 " %c", &a, &sd, &extra) == 2 &&
               !(a % radio_caps()->lo_step_khz)) {
        scan_request_t r = {.type = SCAN_REQ_FFTEVAL_LIVE, .lo_khz = a, .p3 = sd};
        if (!scanner_request(&r)) link_text("ERR busy\n");
    } else if (!strcmp(line, "TIMING?")) {
        scan_timing_t t;
        scanner_timing(&t);
        link_printf("TIMING psd %" PRIu32 " (prep %" PRIu32 " fft %" PRIu32 " power %" PRIu32 ") metrics %" PRIu32
                    " spectrum %" PRIu32 "\n", t.psd_us, t.prep_us, t.fft_us, t.power_us, t.metrics_us, t.spectrum_us);
    } else if (sscanf(line, "PROMISC %" SCNu32 " %c", &a, &extra) == 1 && a <= 1) {
        /* Debug: esp-sdr keeps Wi-Fi promiscuous RX on; measure its CPU cost. */
        esp_wifi_set_promiscuous(a);
        link_text("OK\n");
    } else if (sscanf(line, "THROTTLE %" SCNu32 " %c", &a, &extra) == 1 && (a == 0 || (a >= 10 && a <= 100000))) {
        c.max_rotation_hz = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if ((sscanf(line, "PHYIDLE %" SCNu32 " %" SCNu32 " %c", &a, &b, &extra) == 2 ||
                (sscanf(line, "PHYIDLE %" SCNu32 " %c", &a, &extra) == 1 && !(b = 0))) && a <= 4) {
        /* Debug: radio power-down in the rotation throttle's waits (radio_idle_enter). */
        c.phy_idle = (uint8_t)a;
        c.phy_idle_min_us = b;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "PHYIDLE?")) {
        scan_idle_stats_t s;
        scanner_idle_stats(&s);
        link_printf("PHYIDLE depth %u min %" PRIu32 " n %" PRIu32 " skipped %" PRIu32 " down %" PRIu32 " up %" PRIu32
                    " up_max %" PRIu32 " up_est %" PRIu32 "\n", c.phy_idle, c.phy_idle_min_us, s.n, s.skipped,
                    s.down_us_avg, s.up_us_avg, s.up_us_max, s.up_est_us);
    } else if (sscanf(line, "TUNESKIP %" SCNu32 " %c", &a, &extra) == 1 && a <= 7) {
        radio_set_tune_skip(a);
        link_text("OK\n");
    } else if (!strcmp(line, "SPURS?")) {
        reply_add("SPURS %u", c.nspurs);
        for (unsigned j = 0; j < c.nspurs; j++) reply_add(" %" PRIu32, c.spurs_khz[j]);
        reply_add(" guard %" PRIu32 "\n", c.spur_guard_hz);
        reply_send();
    } else if (!strcmp(line, "SPURS CLEAR")) {
        c.nspurs = 0;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SPURS ADD %" SCNu32 " %c", &a, &extra) == 1) {
        if (c.nspurs >= 16) { link_text("ERR spurs_full\n"); return; }
        c.spurs_khz[c.nspurs++] = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (sscanf(line, "SPURS GUARD %" SCNu32 " %c", &a, &extra) == 1 && a <= 1000000) {
        c.spur_guard_hz = a;
        scanner_set_config(&c);
        link_text("OK\n");
    } else if (!strcmp(line, "GPIO?")) {
        send_gpio();
    } else if (!strcmp(line, "GPIO SAVE")) {
        esp_err_t e = gpio_out_save();
        if (e != ESP_OK) { link_printf("ERR save %s\n", esp_err_to_name(e)); return; }
        link_text("OK\n");
    } else if (sscanf(line, "GPIO %" SCNu32 " %31s %c", &a, arg, &extra) == 2) {
        /* GPIO <n> NONE|LOW|HIGH: at once; GPIO SAVE keeps it for boot. */
        gpio_out_t s = !strcmp(arg, "LOW") ? GPIO_OUT_LOW : !strcmp(arg, "HIGH") ? GPIO_OUT_HIGH : GPIO_OUT_NONE;
        if (s == GPIO_OUT_NONE && strcmp(arg, "NONE")) { link_text("ERR gpio_state\n"); return; }
        if (!gpio_out_allowed(a)) { link_text("ERR gpio_pin\n"); return; }
        gpio_out_set(a, s);
        link_text("OK\n");
    } else if (!strcmp(line, "BOOTLOADER")) {
        link_text("OK BOOTLOADER\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        radio_reboot_to_bootloader();
    } else if (!strcmp(line, "REBOOT")) {
        link_text("OK REBOOT\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        link_text("ERR command\n");
    }
}

/* Line assembly for our commands. */
static char line[128];
static size_t used;
static bool overflow;
static void line_byte(uint8_t b) {
    if (b == '\r') return;
    if (b == '\n') {
        if (overflow) link_text("ERR command_length\n");
        else if (used) {
            line[used] = 0;
            handle(line);
        }
        used = 0;
        overflow = false;
        return;
    }
    if (used + 1 < sizeof(line)) line[used++] = (char)b;
    else overflow = true;
}

static void commands_task(void *arg) {
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        int n = link_read(buf, sizeof(buf), 100);
        for (int i = 0; i < n; i++) line_byte(buf[i]);
    }
}

void commands_task_start(void) {
    reply = malloc(REPLY_BYTES);
    if (reply) reply[0] = 0;
    /* Boot in GATE mode (gate_init has run); monitor mode starts from the defaults. */
    scan_config_t c;
    scanner_get_config(&c);
    other_cfg = c;
    gate_defaults(&c);
    gate_saved_from(&gate_builtin, &c);
    gate_saved = gate_builtin;
    gate_saved_load();
    gate_saved_apply(&c, &gate_saved);
    scanner_set_config(&c);
    link_set_streaming(false);
    xTaskCreatePinnedToCore(commands_task, "cmd", 6144, NULL, 6, NULL, portNUM_PROCESSORS - 1);
}
