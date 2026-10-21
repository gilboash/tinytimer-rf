/* GPL-3.0-or-later. */
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gate.h"
#include "gate_io.h"
#include "link.h"
#include "lora_link.h"

static volatile unsigned stream_hz;

void gate_io_set_stream_hz(unsigned hz) { stream_hz = hz > 50 ? 50 : hz; }
unsigned gate_io_stream_hz(void) { return stream_hz; }

static void send_levels(void) {
    gate_view_t v;
    gate_view(&v);
    char b[200];
    int n = snprintf(b, sizeof(b), "LEVELS {\"t\":%u,\"c\":%u,\"e\":%u,\"l\":[", (unsigned)(esp_timer_get_time() / 1000),
                     v.crossing, v.enabled);
    for (unsigned j = 0; j < GATE_SEATS && n < (int)sizeof(b) - 8; j++)
        n += snprintf(b + n, sizeof(b) - n, "%s%u", j ? "," : "", v.level[j]);
    snprintf(b + n, sizeof(b) - n, "]}\n");
    link_text(b);
}

static void io_task(void *arg) {
    (void)arg;
    int64_t next_stream = 0;
    for (;;) {
        unsigned hz = stream_hz;
        gate_pass_t p;
        if (gate_next_pass(&p, hz ? 1000 / hz : 200)) {
            link_printf("LAP {\"p\":%u,\"n\":%u,\"t\":%u,\"lap_ms\":%u,\"peak\":%u}\n", p.pilot, p.count,
                        (unsigned)p.pass_ms, (unsigned)p.lap_ms, p.peak);
            lora_link_queue_pass(&p);
        }
        int64_t now = esp_timer_get_time();
        if (hz && now >= next_stream) {
            next_stream = now + 1000000 / hz;
            send_levels();
        }
    }
}

void gate_io_start(void) { xTaskCreate(io_task, "gate_io", 3072, NULL, 5, NULL); }
