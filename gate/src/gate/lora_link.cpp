/* GPL-3.0-or-later. */
#include <string.h>
#include <RadioLib.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "board_pins.h"
extern "C" {
#include "gate.h"
#include "link.h"
}
#include "lora_link.h"

/* ---- RF profile (868 MHz SRD, EU limits as the power-meter project) ---- */
#ifndef LORA_FREQ_MHZ
#define LORA_FREQ_MHZ 868.1f
#endif
#ifndef LORA_BW
#define LORA_BW 250.0f
#endif
#ifndef LORA_SF
#define LORA_SF 7
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#define LORA_TX_DBM 14
#define LORA_SYNC_WORD 0x12
#define LORA_PREAMBLE 8
#define LORA_TCXO_V 1.8f

#define STATUS_PERIOD_MS 15000 /* 1% duty cycle: keep chatter well under it */
#define STATUS_MIN_GAP_MS 2000
#define RETRY_BASE_MS 350
#define RETRY_STEP_MS 150
#define MAX_ATTEMPTS 12
#define PENDING_MAX 24

/* ---- RadioLib HAL on the IDF SPI driver. No interrupts: attachInterrupt from
 * a task overflows the ipc1 stack, and polling DIO1 is equivalent here. ---- */
class IdfHal : public RadioLibHal {
  public:
    IdfHal() : RadioLibHal(0x01, 0x03, 0x0, 0x1, 0x01, 0x02) {}
    void init() override { spiBegin(); }
    void term() override { spiEnd(); }
    void pinMode(uint32_t pin, uint32_t mode) override {
        if (pin == RADIOLIB_NC) return;
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << pin;
        c.mode = mode == 0x03 ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
        gpio_config(&c);
    }
    void digitalWrite(uint32_t pin, uint32_t v) override {
        if (pin != RADIOLIB_NC) gpio_set_level((gpio_num_t)pin, v);
    }
    uint32_t digitalRead(uint32_t pin) override {
        return pin == RADIOLIB_NC ? 0 : (uint32_t)gpio_get_level((gpio_num_t)pin);
    }
    void attachInterrupt(uint32_t, void (*)(void), uint32_t) override {}
    void detachInterrupt(uint32_t) override {}
    void delay(unsigned long ms) override { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
    void delayMicroseconds(unsigned long us) override {
        int64_t end = esp_timer_get_time() + (int64_t)us;
        while (esp_timer_get_time() < end) {}
    }
    unsigned long millis() override { return (unsigned long)(esp_timer_get_time() / 1000); }
    unsigned long micros() override { return (unsigned long)esp_timer_get_time(); }
    long pulseIn(uint32_t, uint32_t, unsigned long) override { return 0; }
    void spiBegin() {
        if (dev) return;
        spi_bus_config_t bus = {};
        bus.mosi_io_num = LORA_MOSI;
        bus.miso_io_num = LORA_MISO;
        bus.sclk_io_num = LORA_SCK;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 64;
        spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
        spi_device_interface_config_t d = {};
        d.clock_speed_hz = 2000000;
        d.mode = 0;
        d.spics_io_num = -1; /* RadioLib drives CS itself */
        d.queue_size = 1;
        spi_bus_add_device(SPI2_HOST, &d, &dev);
    }
    void spiBeginTransaction() {}
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) {
        size_t off = 0;
        while (off < len) {
            size_t n = len - off > 64 ? 64 : len - off;
            spi_transaction_t t = {};
            t.length = n * 8;
            t.tx_buffer = out + off;
            t.rx_buffer = in + off;
            spi_device_polling_transmit(dev, &t);
            off += n;
        }
    }
    void spiEndTransaction() {}
    void spiEnd() {}

  private:
    spi_device_handle_t dev = nullptr;
};

/* ---- State ---- */
typedef struct {
    gate_pass_t p;
    bool used;
    uint8_t attempts;
    uint32_t next_ms;
    uint16_t sent_seq;
} pend_t;

static IdfHal *hal;
static SX1262 *radio;
static QueueHandle_t inq;
static pend_t *pend;
static lora_stats_t st;
static uint16_t seq;
static uint16_t last_cfg_seq = 0xFFFF;
static bool have_cfg_seq;
static uint32_t last_status_ms;
static volatile bool status_now;
static volatile bool cfg_ack_pending;
static uint16_t cfg_ack_seq;
static bool cal_active;
static uint32_t cal_end_ms;
static uint16_t cal_seq;
static uint8_t cal_kind;
static uint32_t live_until_ms, last_live_ms;  // live level streaming window (0 = off)
static uint16_t live_seq;
static volatile bool sync_pending;
static uint16_t sync_seq;
static portMUX_TYPE smux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool packet_ready() {
    /* DIO1 alone is not enough: the SX126x shares one buffer between TX and RX,
     * so a leftover TxDone makes readData() return the packet just sent. */
    if (gpio_get_level((gpio_num_t)LORA_DIO1) != 1) return false;
    return (radio->getIrqFlags() & (1UL << RADIOLIB_IRQ_RX_DONE)) != 0;
}

/* Defensive: the module's RST is not reachable from the C5 header, so the chip
 * keeps whatever state it was last left in. A stale "command timeout" in its
 * status byte makes RadioLib return SPI_CMD_TIMEOUT (-705) from the next command
 * and startReceive() give up; a throwaway standby clears it. (If RX ever stays
 * dead with err -705, power-cycle the module: that is the only reset.) */
static int16_t start_rx() {
    radio->standby();
    int16_t r = radio->startReceive();
    if (r != RADIOLIB_ERR_NONE) {
        radio->standby();
        r = radio->startReceive();
    }
    return r;
}

static void transmit(const void *buf, size_t len) {
    /* See start_rx(): clear any stale status before the first command. */
    radio->standby();
    int16_t r = radio->transmit((const uint8_t *)buf, len);
    st.tx++;
    if (r != RADIOLIB_ERR_NONE) st.err = r;
    int16_t r2 = start_rx();
    if (r != RADIOLIB_ERR_NONE || r2 != RADIOLIB_ERR_NONE)
        link_printf("LORADBG tx=%d startRx=%d busy=%d dio1=%d\n", r, r2, gpio_get_level((gpio_num_t)LORA_BUSY),
                    gpio_get_level((gpio_num_t)LORA_DIO1));
}

static void send_pass_packet(uint32_t now) {
    ll_pass_pkt_t pk = {};
    pk.magic = LL_MAGIC_PASS;
    pk.version = LL_VERSION;
    pk.gate_id = gate_id();
    pk.seq = ++seq;
    pk.sent_ms = now;
    for (int i = 0; i < PENDING_MAX && pk.n < LL_PASSES_PER_PACKET; i++) {
        pend_t &e = pend[i];
        if (!e.used || (int32_t)(now - e.next_ms) < 0) continue;
        if (e.attempts >= MAX_ATTEMPTS) {
            e.used = false;
            st.dropped++;
            continue;
        }
        if (e.attempts) st.retries++;
        e.attempts++;
        e.next_ms = now + RETRY_BASE_MS + (uint32_t)e.attempts * RETRY_STEP_MS;
        e.sent_seq = pk.seq;
        pk.p[pk.n++] = (ll_pass_t){e.p.pilot, e.p.count, e.p.pass_ms, e.p.lap_ms, e.p.peak, e.p.xp, e.p.xl};
    }
    if (!pk.n) return;
    pk.sent_ms = now_ms(); /* as late as possible: it is the age reference */
    transmit(&pk, sizeof(pk) - (LL_PASSES_PER_PACKET - pk.n) * sizeof(ll_pass_t));
}

static void send_status() {
    ll_status_pkt_t s = {};
    s.magic = LL_MAGIC_STATUS;
    s.version = LL_VERSION;
    s.gate_id = gate_id();
    s.seq = ++seq;
    s.min_lap_ms_div10 = (uint16_t)(gate_min_lap_ms() / 10);
    s.dropped = (uint16_t)st.dropped;
    for (unsigned n = 1; n <= LL_MAX_PILOTS; n++) gate_get_pilot(n, &s.pilot[n - 1]);
    s.flags = gate_metric_is_snr() ? LL_STATUS_SNR : 0;
    s.meta_rev = gate_meta_rev();
    s.sent_ms = now_ms();
    last_status_ms = s.sent_ms;
    transmit(&s, sizeof(s));
}

static void handle_rx() {
    uint8_t buf[64];
    size_t len = radio->getPacketLength();
    if (len == 0 || len > sizeof(buf)) {
        start_rx();
        return;
    }
    int16_t r = radio->readData(buf, len);
    if (r == RADIOLIB_ERR_NONE) {
        st.rx++;
        if (buf[0] == LL_MAGIC_ACK && len == sizeof(ll_ack_pkt_t) && buf[1] == LL_VERSION) {
            ll_ack_pkt_t a;
            memcpy(&a, buf, sizeof(a));
            if (a.gate_id == gate_id()) {
                st.last_rssi = a.rssi_dbm;
                st.last_snr = a.snr_db;
                for (int i = 0; i < PENDING_MAX; i++)
                    if (pend[i].used && pend[i].sent_seq == a.seq) {
                        pend[i].used = false;
                        st.acked++;
                    }
            }
        } else if (buf[0] == LL_MAGIC_CFG && len == sizeof(ll_cfg_pkt_t) && buf[1] == LL_VERSION) {
            ll_cfg_pkt_t c;
            memcpy(&c, buf, sizeof(c));
            if (c.gate_id == gate_id() || c.gate_id == 0xFF) {
                cfg_ack_seq = c.seq;  // acknowledge repeats too: our ACK may be what got lost
                cfg_ack_pending = true;
            }
            if ((c.gate_id == gate_id() || c.gate_id == 0xFF) && !(have_cfg_seq && c.seq == last_cfg_seq)) {
                have_cfg_seq = true;
                last_cfg_seq = c.seq;
                st.cfg_rx++;
                bool ok = gate_set_pilot(c.pilot, &c.cfg);
                if (ok && (c.flags & LL_CFG_SAVE)) gate_save();
                link_printf("CFG {\"p\":%u,\"freq\":%u,\"enter\":%u,\"exit\":%u,\"ok\":%d}\n", c.pilot, c.cfg.freq_mhz,
                            c.cfg.enter_at, c.cfg.exit_at, ok);
                status_now = true;
            }
        } else if (buf[0] == LL_MAGIC_NAME && len == sizeof(ll_name_pkt_t) && buf[1] == LL_VERSION) {
            ll_name_pkt_t q;
            memcpy(&q, buf, sizeof(q));
            if (q.gate_id == gate_id() || q.gate_id == 0xFF) {
                char nm[LL_NAME_LEN + 1];
                memcpy(nm, q.name, LL_NAME_LEN);
                nm[LL_NAME_LEN] = 0;
                gate_set_name(q.pilot, nm);
                cfg_ack_seq = q.seq;
                cfg_ack_pending = true;
                status_now = true;
            }
        } else if (buf[0] == LL_MAGIC_SET && len == sizeof(ll_set_pkt_t) && buf[1] == LL_VERSION) {
            ll_set_pkt_t q;
            memcpy(&q, buf, sizeof(q));
            if (q.gate_id == gate_id() || q.gate_id == 0xFF) {
                gate_race_t r = {q.laps, q.flags, q.countdown};
                gate_set_race(&r);
                gate_set_min_lap_ms(q.min_lap_ms);
                cfg_ack_seq = q.seq;
                cfg_ack_pending = true;
                status_now = true;
            }
        } else if (buf[0] == LL_MAGIC_SYNCREQ && len == sizeof(ll_syncreq_pkt_t) && buf[1] == LL_VERSION) {
            ll_syncreq_pkt_t q;
            memcpy(&q, buf, sizeof(q));
            if (q.gate_id == gate_id() || q.gate_id == 0xFF) {
                sync_seq = q.seq;
                sync_pending = true;
            }
        } else if (buf[0] == LL_MAGIC_LIVEREQ && len == sizeof(ll_live_req_t) && buf[1] == LL_VERSION) {
            ll_live_req_t q;
            memcpy(&q, buf, sizeof(q));
            if (q.gate_id == gate_id() || q.gate_id == 0xFF) {
                uint8_t secs = q.secs > 60 ? 60 : q.secs;  // a few % of the duty-cycle budget: keep it short
                live_until_ms = secs ? (now_ms() + secs * 1000u) | 1 : 0;
            }
        } else if (buf[0] == LL_MAGIC_CAL && len == sizeof(ll_cal_req_t) && buf[1] == LL_VERSION) {
            ll_cal_req_t q;
            memcpy(&q, buf, sizeof(q));
            if ((q.gate_id == gate_id() || q.gate_id == 0xFF) && !cal_active) {
                gate_cal_start();
                cal_active = true;
                cal_seq = q.seq;
                cal_kind = q.kind;
                cal_end_ms = now_ms() + (uint32_t)(q.secs < 1 ? 1 : q.secs > 10 ? 10 : q.secs) * 1000;
            }
        }
    }
    start_rx();
}

static void send_cfg_ack() {
    ll_ack_pkt_t a = {};
    a.magic = LL_MAGIC_ACK;
    a.version = LL_VERSION;
    a.gate_id = gate_id();
    a.seq = cfg_ack_seq;
    transmit(&a, sizeof(a));
}

static void send_sync() {
    ll_sync_pkt_t y = {};
    y.magic = LL_MAGIC_SYNC;
    y.version = LL_VERSION;
    y.gate_id = gate_id();
    y.seq = sync_seq;
    y.meta_rev = gate_meta_rev();
    gate_race_t r;
    gate_get_race(&r);
    y.laps = r.laps;
    y.flags = r.flags;
    y.countdown = r.countdown;
    y.min_lap_ms = gate_min_lap_ms();
    for (unsigned n = 1; n <= LL_MAX_PILOTS; n++) {
        char nm[LL_NAME_LEN + 1];
        gate_get_name(n, nm);
        memcpy(y.name[n - 1], nm, LL_NAME_LEN);
    }
    transmit(&y, sizeof(y));
}

static void send_live() {
    gate_view_t v;
    gate_view(&v);
    ll_live_pkt_t p = {};
    p.magic = LL_MAGIC_LIVE;
    p.version = LL_VERSION;
    p.gate_id = gate_id();
    p.seq = ++live_seq;
    p.crossing = v.crossing;
    for (int i = 0; i < LL_MAX_PILOTS; i++) {
        bool on = (v.enabled >> i) & 1;
        p.level[i] = on ? v.level[i] : 0;
        p.hold[i] = on ? v.hold[i] : 0;
    }
    transmit(&p, sizeof(p));
}

static void send_cal_result() {
    gate_cal_t c;
    gate_cal_finish(&c);
    ll_cal_res_t r = {};
    r.magic = LL_MAGIC_CALRES;
    r.version = LL_VERSION;
    r.gate_id = gate_id();
    r.seq = cal_seq;
    r.kind = cal_kind;
    memcpy(r.lo, c.lo, sizeof(r.lo));
    memcpy(r.avg, c.avg, sizeof(r.avg));
    memcpy(r.hi, c.hi, sizeof(r.hi));
    transmit(&r, sizeof(r));
}

static void lora_task(void *) {
    start_rx();
    for (;;) {
        gate_pass_t p;
        while (xQueueReceive(inq, &p, 0) == pdTRUE) {
            int slot = -1;
            uint32_t oldest = 0;
            int oldest_i = 0;
            uint32_t now = now_ms();
            for (int i = 0; i < PENDING_MAX; i++) {
                if (!pend[i].used) { slot = i; break; }
                uint32_t age = now - pend[i].p.pass_ms;
                if (age >= oldest) { oldest = age; oldest_i = i; }
            }
            if (slot < 0) { slot = oldest_i; st.dropped++; }
            pend[slot] = (pend_t){p, true, 0, now, 0};
        }
        if (packet_ready()) handle_rx();
        uint32_t now = now_ms();
        if (cfg_ack_pending) {
            cfg_ack_pending = false;
            send_cfg_ack();
        }
        if (sync_pending) {
            sync_pending = false;
            send_sync();
        }
        if (live_until_ms && (int32_t)(now - live_until_ms) >= 0) live_until_ms = 0;
        if (cal_active && (int32_t)(now - cal_end_ms) >= 0) {
            cal_active = false;
            send_cal_result();
        }
        unsigned npend = 0;
        bool ready = false;
        for (int i = 0; i < PENDING_MAX; i++)
            if (pend[i].used) {
                npend++;
                if ((int32_t)(now - pend[i].next_ms) >= 0) ready = true;
            }
        st.pending = npend;
        if (ready) send_pass_packet(now);
        else if ((status_now && now - last_status_ms >= STATUS_MIN_GAP_MS) || now - last_status_ms >= STATUS_PERIOD_MS) {
            status_now = false;
            send_status();
        } else if (live_until_ms && now - last_live_ms >= 400) {  // passes and status go first
            last_live_ms = now;
            send_live();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

extern "C" esp_err_t lora_link_init(void) {
    st.freq_mhz = LORA_FREQ_MHZ;
    st.sf = LORA_SF;
    /* Heap, not statics: the C5's static RAM below the RF capture bank is nearly full. */
    pend = (pend_t *)calloc(PENDING_MAX, sizeof(pend_t));
    inq = xQueueCreate(16, sizeof(gate_pass_t));
    hal = new IdfHal();
    Module *mod = new Module(hal, LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
    radio = new SX1262(mod);
    if (!pend || !inq) return ESP_ERR_NO_MEM;
    int16_t r = radio->begin(LORA_FREQ_MHZ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC_WORD, LORA_TX_DBM, LORA_PREAMBLE,
                             LORA_TCXO_V, false);
    if (r == RADIOLIB_ERR_NONE) r = radio->setDio2AsRfSwitch(true);
    if (r == RADIOLIB_ERR_NONE) {
        radio->setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);
        r = radio->setCRC(2);
    }
    if (r != RADIOLIB_ERR_NONE) {
        st.err = r;
        return ESP_OK; /* LORA? reports it; the gate still times and prints over USB */
    }
    st.up = 1;
    xTaskCreate(lora_task, "lora", 4096, NULL, 4, NULL);
    return ESP_OK;
}

/* Raw bring-up probe, independent of RadioLib: reset the chip, watch BUSY, ask
 * it for its status and its version string over the same SPI path. */
extern "C" void lora_link_diag(void) {
    if (!hal) { link_text("DIAG no hal\n"); return; }
    gpio_num_t cs = (gpio_num_t)LORA_CS, rst = (gpio_num_t)LORA_RST;
    link_printf("DIAG pins sck=%d miso=%d mosi=%d cs=%d dio1=%d rst=%d busy=%d rxen=%d\n", LORA_SCK, LORA_MISO,
                LORA_MOSI, LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, LORA_RXEN);
    link_printf("DIAG idle levels before reset: busy=%d dio1=%d miso=%d\n", gpio_get_level((gpio_num_t)LORA_BUSY),
                gpio_get_level((gpio_num_t)LORA_DIO1), gpio_get_level((gpio_num_t)LORA_MISO));
    /* Is BUSY driven by the chip, or just floating? Pull it each way and see. */
    gpio_set_pull_mode((gpio_num_t)LORA_BUSY, GPIO_PULLDOWN_ONLY);
    hal->delay(2);
    int dn = gpio_get_level((gpio_num_t)LORA_BUSY);
    gpio_set_pull_mode((gpio_num_t)LORA_BUSY, GPIO_PULLUP_ONLY);
    hal->delay(2);
    int up = gpio_get_level((gpio_num_t)LORA_BUSY);
    gpio_set_pull_mode((gpio_num_t)LORA_BUSY, GPIO_FLOATING);
    link_printf("DIAG BUSY with pulldown=%d, with pullup=%d (different = floating, nothing drives it)\n", dn, up);
    hal->pinMode(LORA_RST, 0x03);
    hal->digitalWrite(LORA_RST, 0);
    hal->delay(5);
    link_printf("DIAG busy while RST held low: %d\n", gpio_get_level((gpio_num_t)LORA_BUSY));
    hal->digitalWrite(LORA_RST, 1);
    int busy_ms = -1;
    for (int i = 0; i < 200; i++) {
        if (!gpio_get_level((gpio_num_t)LORA_BUSY)) { busy_ms = i; break; }
        hal->delay(1);
    }
    link_printf("DIAG busy low %d ms after reset release (-1 = never)\n", busy_ms);
    uint8_t tx1[2] = {0xC0, 0x00}, rx1[2] = {0, 0};
    gpio_set_level(cs, 0);
    hal->spiTransfer(tx1, 2, rx1);
    gpio_set_level(cs, 1);
    link_printf("DIAG GetStatus -> %02X %02X (a live chip answers xx 2x/3x/4x/5x, never FF FF or 00 00)\n", rx1[0], rx1[1]);
    hal->delay(2);
    for (int i = 0; i < 100 && gpio_get_level((gpio_num_t)LORA_BUSY); i++) hal->delay(1);
    uint8_t tx2[9] = {0x1D, 0x03, 0x20, 0, 0, 0, 0, 0, 0}, rx2[9] = {};
    gpio_set_level(cs, 0);
    hal->spiTransfer(tx2, 9, rx2);
    gpio_set_level(cs, 1);
    link_printf("DIAG ReadRegister 0x0320 -> %02X %02X %02X %02X %02X %02X %02X (expect 53 58 31 32 36 .. = \"SX126\")\n", rx2[2], rx2[3], rx2[4],
                rx2[5], rx2[6], rx2[7], rx2[8]);
}

extern "C" void lora_link_queue_pass(const gate_pass_t *p) {
    if (st.up) xQueueSend(inq, p, 0);
}

extern "C" void lora_link_stats(lora_stats_t *s) { *s = st; }

extern "C" void lora_link_request_status(void) { status_now = true; }
