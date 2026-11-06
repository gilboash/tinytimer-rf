/* LoRa uplink of the gate: queues passes, retransmits until ACKed, sends a
 * periodic STATUS, applies CFG from the base. GPL-3.0-or-later. */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include "gate.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int up;             /* radio initialised */
    int err;            /* RadioLib status of the failed step, 0 if none */
    float freq_mhz;
    int sf;
    unsigned tx, acked, retries, dropped, pending, rx, cfg_rx;
    int last_rssi, last_snr; /* of the last ACK: the uplink as the base saw it */
} lora_stats_t;

/* Never fails the boot: a missing module just leaves `up` at 0. */
esp_err_t lora_link_init(void);
void lora_link_queue_pass(const gate_pass_t *p);
void lora_link_stats(lora_stats_t *s);
/* Print a raw reset/BUSY/SPI probe of the radio chip (LORA DIAG). */
void lora_link_diag(void);
/* Send a STATUS now (rate limited). */
void lora_link_request_status(void);

#ifdef __cplusplus
}
#endif
