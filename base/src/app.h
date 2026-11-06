// Base-station application layer: the race engine and the web API that used to
// live in server/lapserver.py, running on the S3 itself so a phone on the base's
// WiFi hotspot is all that is needed. GPL-3.0-or-later.
//
//   radio (main.cpp) --events--> app.cpp (config, passes, race, call-outs)
//                                   |--> web.cpp (AP, pages, REST, SSE) --> phones
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "lap_link.h"

struct PassEntry {
    uint8_t pilot, count;
    uint32_t pass_ms, lap_ms;
    uint8_t peak, xp, xl;
};

void app_init();
// Main loop task: race countdown, CFG queue, once-a-second housekeeping.
void app_loop();

// Radio events, called from the main loop task.
void app_hello(float freq_mhz, float bw_khz, int sf);
void app_radio_error(int code);
void app_rx_pass(uint8_t gate, uint32_t toa_ms, uint32_t sent_ms, float rssi, float snr, const PassEntry *p, int n);
void app_rx_status(uint8_t gate, uint32_t gate_ms, float rssi, float snr, uint16_t minlap_ms, uint16_t dropped,
                   const ll_pilot_cfg_t pilots[LL_MAX_PILOTS], bool snr_metric, uint8_t meta_rev);
void app_rx_sync(uint8_t gate, uint8_t rev, uint8_t laps, uint8_t flags, uint8_t countdown, uint16_t min_lap_ms,
                 const char names[LL_MAX_PILOTS][LL_NAME_LEN]);
void app_rx_bad();
void app_rx_live(uint8_t gate, const uint8_t level[LL_MAX_PILOTS], const uint8_t hold[LL_MAX_PILOTS], uint8_t crossing);
void app_rx_cal(uint8_t gate, uint8_t kind, const uint8_t lo[LL_MAX_PILOTS], const uint8_t avg[LL_MAX_PILOTS],
                const uint8_t hi[LL_MAX_PILOTS]);
void app_cfg_tx_result(bool ok);
// A line of base chatter for the terminal on the base page.
void app_term(const char *src, const char *line);

// Provided by main.cpp; only ever called from the main loop task.
void radio_send_name(uint8_t gate, uint8_t pilot, const char *name);
void radio_send_set(uint8_t gate, uint8_t laps, uint8_t flags, uint8_t countdown, uint16_t min_lap_ms);
void radio_request_sync(uint8_t gate);
void radio_send_live(uint8_t gate, uint8_t secs);
void radio_send_cal(uint8_t gate, uint8_t kind, uint8_t secs);
void radio_send_cfg(uint8_t gate, uint8_t pilot, uint16_t freq, uint8_t enter, uint8_t exit_, bool save);

// Called from web handlers (any task). Both take the engine lock.
String app_snapshot();
// POST /api/...; fills `resp` with a JSON body and returns the HTTP status.
int app_post(const String &path, JsonDocument &body, String &resp);

// web.cpp
void web_start();
void web_publish(const String &json);  // one SSE message to every connected phone
