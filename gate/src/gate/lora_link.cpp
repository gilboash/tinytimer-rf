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
static portMUX_TYPE smux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool packet_ready() {
    /* DIO1 alone is not enough: the SX126x shares one buffer between TX and RX,
     * so a leftover TxDone makes readData() return the packet just sent. */
    if (gpio_get_level((gpio_num_t)LORA_DIO1) != 1) return false;
    return (radio->getIrqFlags() & (1UL << RADIOLIB_IRQ_RX_DONE)) != 0;
}

static void transmit(const void *buf, size_t len) {
    int16_t r = radio->transmit((const uint8_t *)buf, len);
    st.tx++;
    if (r != RADIOLIB_ERR_NONE) st.err = r;
    radio->startReceive();
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
        pk.p[pk.n++] = (ll_pass_t){e.p.pilot, e.p.count, e.p.pass_ms, e.p.lap_ms, e.p.peak};
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
    s.sent_ms = now_ms();
    last_status_ms = s.sent_ms;
    transmit(&s, sizeof(s));
}

static void handle_rx() {
    uint8_t buf[64];
    size_t len = radio->getPacketLength();
    if (len == 0 || len > sizeof(buf)) {
        radio->startReceive();
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
        }
    }
    radio->startReceive();
}

static void lora_task(void *) {
    radio->startReceive();
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

extern "C" void lora_link_queue_pass(const gate_pass_t *p) {
    if (st.up) xQueueSend(inq, p, 0);
}

extern "C" void lora_link_stats(lora_stats_t *s) { *s = st; }

extern "C" void lora_link_request_status(void) { status_now = true; }
