// Lap-timer base station.
//
// LoRa side: listens for PASS / STATUS from gates, ACKs them, and can send a
// per-pilot CFG back. USB side: one JSON object per line out, one text command
// per line in. All business logic (names, laps, call-outs) is on the server.
//
//   out  {"ev":"pass","gate":1,"seq":7,"ms":123456,"toa_ms":46,"sent_ms":99000,
//         "rssi":-71,"snr":9.5,"p":[{"n":1,"c":3,"t":98500,"lap":21300,"pk":182}]}
//   out  {"ev":"status","gate":1,"ms":..,"rssi":..,"snr":..,"minlap":3000,"dropped":0,
//         "pilots":[{"n":1,"f":5658,"e":125,"x":105},...]}
//   out  {"ev":"hello",...} at boot, {"ev":"hb","ms":..} every second
//   in   CFG <gate|*> <pilot> <freq_mhz|0> <enter> <exit> [save]
//        NAME <pilot> <text>   SET <laps> <holeshot 0|1> <countdown> <min_lap_ms>   SYNC
//        LIVE <secs>                  (0 = stop; live levels come back as {"ev":"live",...})
//        CAL <noise|peak> [secs]      (result comes back as {"ev":"cal",...})
//        INFO
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "lap_link.h"
#include "app.h"

// XIAO ESP32S3 + B2B Wio-SX1262 (pins confirmed on this hardware in the
// power-meter project).
#define LORA_SCK   7
#define LORA_MISO  8
#define LORA_MOSI  9
#define LORA_CS    41
#define LORA_DIO1  39
#define LORA_BUSY  40
#define LORA_RST   42
#define LORA_RXEN  38
#define USER_LED   21  // active low

// Must match the gate (gate/src/gate/lora_link.cpp).
#define LORA_FREQ_MHZ 868.1f
#define LORA_BW       250.0f
#define LORA_SF       7
#define LORA_CR       5
#define LORA_SYNC     0x12
#define LORA_TX_DBM   14

static SPIClass loraSpi(FSPI);
static SPISettings loraSpiSettings(2000000, MSBFIRST, SPI_MODE0);
static SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, loraSpi, loraSpiSettings);

static uint16_t cfgSeq = 0;
static uint32_t lastHb = 0;
static uint32_t ledOffAt = 0;

// RadioLib's setPacketReceivedAction() registers a GPIO ISR through the ipc1
// task, whose stack is too small (see power-meter notes). Poll DIO1 instead.
static bool packetReady() {
    if (digitalRead(LORA_DIO1) != HIGH) return false;
    return (radio.getIrqFlags() & (1UL << RADIOLIB_IRQ_RX_DONE)) != 0;
}

static void blink() {
    digitalWrite(USER_LED, LOW);
    ledOffAt = millis() + 40;
}

static void sendAck(uint8_t gate, uint16_t seq, float rssi, float snr) {
    ll_ack_pkt_t a = {};
    a.magic = LL_MAGIC_ACK;
    a.version = LL_VERSION;
    a.gate_id = gate;
    a.seq = seq;
    a.rssi_dbm = (int16_t)rssi;
    a.snr_db = (int8_t)snr;
    delay(12);  // the gate needs a moment to switch from TX to RX
    radio.transmit((uint8_t *)&a, sizeof(a));
    radio.startReceive();
}

static void onPass(const uint8_t *b, size_t len, uint32_t rxMs, float rssi, float snr) {
    ll_pass_pkt_t pk = {};
    if (len < 10 || len > sizeof(pk)) return;
    memcpy(&pk, b, len);
    if (pk.n == 0 || pk.n > LL_PASSES_PER_PACKET ||
        len != sizeof(pk) - (LL_PASSES_PER_PACKET - pk.n) * sizeof(ll_pass_t))
        return;
    uint32_t toaMs = (radio.getTimeOnAir(len) + 999) / 1000;
    sendAck(pk.gate_id, pk.seq, rssi, snr);
    Serial.printf("{\"ev\":\"pass\",\"gate\":%u,\"seq\":%u,\"ms\":%lu,\"toa_ms\":%lu,\"sent_ms\":%lu,"
                  "\"rssi\":%.0f,\"snr\":%.1f,\"p\":[",
                  pk.gate_id, pk.seq, (unsigned long)rxMs, (unsigned long)toaMs, (unsigned long)pk.sent_ms,
                  rssi, snr);
    for (uint8_t i = 0; i < pk.n; i++)
        Serial.printf("%s{\"n\":%u,\"c\":%u,\"t\":%lu,\"lap\":%lu,\"pk\":%u,\"xp\":%u,\"xl\":%u}", i ? "," : "", pk.p[i].pilot,
                      pk.p[i].count, (unsigned long)pk.p[i].pass_ms, (unsigned long)pk.p[i].lap_ms, pk.p[i].peak,
                      pk.p[i].xp, pk.p[i].xl);
    Serial.print("]}\n");
    PassEntry e[LL_PASSES_PER_PACKET];
    for (uint8_t i = 0; i < pk.n; i++)
        e[i] = {pk.p[i].pilot, pk.p[i].count, pk.p[i].pass_ms, pk.p[i].lap_ms, pk.p[i].peak, pk.p[i].xp, pk.p[i].xl};
    app_rx_pass(pk.gate_id, toaMs, pk.sent_ms, rssi, snr, e, pk.n);
}

static void onStatus(const uint8_t *b, size_t len, uint32_t rxMs, float rssi, float snr) {
    ll_status_pkt_t s;
    if (len != sizeof(s)) return;
    memcpy(&s, b, len);
    sendAck(s.gate_id, s.seq, rssi, snr);
    Serial.printf("{\"ev\":\"status\",\"gate\":%u,\"ms\":%lu,\"sent_ms\":%lu,\"rssi\":%.0f,\"snr\":%.1f,"
                  "\"minlap\":%u,\"dropped\":%u,\"metric\":\"%s\",\"rev\":%u,\"pilots\":[",
                  s.gate_id, (unsigned long)rxMs, (unsigned long)s.sent_ms, rssi, snr, s.min_lap_ms_div10 * 10,
                  s.dropped, (s.flags & LL_STATUS_SNR) ? "snr" : "rssi", s.meta_rev);
    for (int i = 0; i < LL_MAX_PILOTS; i++)
        Serial.printf("%s{\"n\":%d,\"f\":%u,\"e\":%u,\"x\":%u}", i ? "," : "", i + 1, s.pilot[i].freq_mhz,
                      s.pilot[i].enter_at, s.pilot[i].exit_at);
    Serial.print("]}\n");
    app_rx_status(s.gate_id, s.sent_ms, rssi, snr, s.min_lap_ms_div10 * 10, s.dropped, s.pilot, (s.flags & LL_STATUS_SNR) != 0, s.meta_rev);
}

static void onCfgAck(uint16_t seq);

static void pollRadio() {
    if (!packetReady()) return;
    uint32_t rxMs = millis();
    uint8_t buf[160];
    size_t len = radio.getPacketLength();
    if (len == 0 || len > sizeof(buf)) {
        radio.startReceive();
        return;
    }
    int16_t st = radio.readData(buf, len);
    float rssi = radio.getRSSI(), snr = radio.getSNR();
    if (st == RADIOLIB_ERR_NONE && buf[1] == LL_VERSION) {
        blink();
        if (buf[0] == LL_MAGIC_PASS) onPass(buf, len, rxMs, rssi, snr);
        else if (buf[0] == LL_MAGIC_STATUS) onStatus(buf, len, rxMs, rssi, snr);
        else if (buf[0] == LL_MAGIC_ACK && len == sizeof(ll_ack_pkt_t)) onCfgAck(((ll_ack_pkt_t *)buf)->seq);
        else if (buf[0] == LL_MAGIC_SYNC && len == sizeof(ll_sync_pkt_t)) {
            ll_sync_pkt_t y;
            memcpy(&y, buf, sizeof(y));
            app_rx_sync(y.gate_id, y.meta_rev, y.laps, y.flags, y.countdown, y.min_lap_ms, y.name);
            Serial.printf("{\"ev\":\"sync\",\"gate\":%u,\"rev\":%u,\"laps\":%u,\"holeshot\":%u,\"countdown\":%u,\"min_lap_ms\":%u,\"names\":[",
                          y.gate_id, y.meta_rev, y.laps, (y.flags & LL_SET_HOLESHOT) ? 1 : 0, y.countdown, y.min_lap_ms);
            for (int i = 0; i < LL_MAX_PILOTS; i++) {
                char nm[LL_NAME_LEN + 1];
                memcpy(nm, y.name[i], LL_NAME_LEN);
                nm[LL_NAME_LEN] = 0;
                Serial.printf("%s\"%s\"", i ? "," : "", nm);
            }
            Serial.print("]}\n");
        }
        else if (buf[0] == LL_MAGIC_LIVE && len == sizeof(ll_live_pkt_t)) {
            ll_live_pkt_t lv;
            memcpy(&lv, buf, sizeof(lv));
            app_rx_live(lv.gate_id, lv.level, lv.hold, lv.crossing);
            Serial.printf("{\"ev\":\"live\",\"gate\":%u,\"c\":%u,\"l\":[", lv.gate_id, lv.crossing);
            for (int i = 0; i < LL_MAX_PILOTS; i++) Serial.printf("%s%u", i ? "," : "", lv.level[i]);
            Serial.print("],\"h\":[");
            for (int i = 0; i < LL_MAX_PILOTS; i++) Serial.printf("%s%u", i ? "," : "", lv.hold[i]);
            Serial.print("]}\n");
        }
        else if (buf[0] == LL_MAGIC_CALRES && len == sizeof(ll_cal_res_t)) {
            ll_cal_res_t r;
            memcpy(&r, buf, sizeof(r));
            app_rx_cal(r.gate_id, r.kind, r.lo, r.avg, r.hi);
            Serial.printf("{\"ev\":\"cal\",\"gate\":%u,\"kind\":%u", r.gate_id, r.kind);
            const uint8_t *arr[3] = {r.lo, r.avg, r.hi};
            const char *nm[3] = {"lo", "avg", "hi"};
            for (int k = 0; k < 3; k++) {
                Serial.printf(",\"%s\":[", nm[k]);
                for (int i = 0; i < LL_MAX_PILOTS; i++) Serial.printf("%s%u", i ? "," : "", arr[k][i]);
                Serial.print("]");
            }
            Serial.print("}\n");
        }
    } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
        Serial.printf("{\"ev\":\"bad\",\"ms\":%lu,\"why\":\"crc\"}\n", (unsigned long)rxMs);
        app_rx_bad();
    }
    radio.startReceive();
}

// Config goes out as a queue of pending CFG packets, each retransmitted (same
// seq) until the gate acknowledges it, so a lost packet or a gate that was busy
// transmitting no longer drops a setting silently.
struct PendCfg {
    bool used;
    uint8_t buf[20];   // the packet as sent (CFG, NAME or SET)
    uint8_t len;
    uint16_t seq;
    uint8_t gate, pilot;
    uint8_t tries;
    uint32_t next;
};
static PendCfg pcfg[16];
static uint32_t lastTx = 0;
static struct { bool used; ll_cal_req_t req; uint8_t tries; uint32_t next; } pcal;
static struct { bool used; ll_live_req_t req; uint8_t tries; uint32_t next; } plive;
static struct { bool used; ll_syncreq_pkt_t req; uint32_t next; } psync;

static void onCfgAck(uint16_t seq) {
    for (PendCfg &p : pcfg)
        if (p.used && p.seq == seq) {
            p.used = false;
            app_cfg_tx_result(true);
            Serial.printf("{\"ev\":\"cfg_tx\",\"gate\":%u,\"pilot\":%u,\"seq\":%u,\"ok\":1,\"tries\":%u}\n", p.gate, p.pilot,
                          p.seq, p.tries);
        }
}

static PendCfg *freeSlot() {
    for (PendCfg &p : pcfg)
        if (!p.used) return &p;
    return &pcfg[0];  // queue full: the oldest loses
}

static void queuePkt(const void *pkt, size_t len, uint8_t gate, uint8_t pilot, uint16_t seq) {
    PendCfg *slot = freeSlot();
    *slot = {};
    slot->used = true;
    memcpy(slot->buf, pkt, len);
    slot->len = len;
    slot->seq = seq;
    slot->gate = gate;
    slot->pilot = pilot;
    slot->next = millis();
}

static void sendCfg(uint8_t gate, uint8_t pilot, uint16_t freq, uint8_t enter, uint8_t exit_, bool save) {
    ll_cfg_pkt_t c = {};
    c.magic = LL_MAGIC_CFG;
    c.version = LL_VERSION;
    c.gate_id = gate;
    c.seq = ++cfgSeq;
    c.pilot = pilot;
    c.cfg = {freq, enter, exit_};
    c.flags = save ? LL_CFG_SAVE : 0;
    queuePkt(&c, sizeof(c), gate, pilot, c.seq);
}

void radio_send_cfg(uint8_t gate, uint8_t pilot, uint16_t freq, uint8_t enter, uint8_t exit_, bool save) {
    sendCfg(gate, pilot, freq, enter, exit_, save);
}

void radio_send_name(uint8_t gate, uint8_t pilot, const char *name) {
    ll_name_pkt_t n = {};
    n.magic = LL_MAGIC_NAME;
    n.version = LL_VERSION;
    n.gate_id = gate;
    n.seq = ++cfgSeq;
    n.pilot = pilot;
    strncpy(n.name, name, LL_NAME_LEN);
    queuePkt(&n, sizeof(n), gate, pilot, n.seq);
}

void radio_send_set(uint8_t gate, uint8_t laps, uint8_t flags, uint8_t countdown, uint16_t min_lap_ms) {
    ll_set_pkt_t t = {};
    t.magic = LL_MAGIC_SET;
    t.version = LL_VERSION;
    t.gate_id = gate;
    t.seq = ++cfgSeq;
    t.laps = laps;
    t.flags = flags;
    t.countdown = countdown;
    t.min_lap_ms = min_lap_ms;
    queuePkt(&t, sizeof(t), gate, 0, t.seq);
}

void radio_request_sync(uint8_t gate) {
    psync.used = true;
    psync.req = {};
    psync.req.magic = LL_MAGIC_SYNCREQ;
    psync.req.version = LL_VERSION;
    psync.req.gate_id = gate;
    psync.req.seq = ++cfgSeq;
    psync.next = millis();
}

void radio_send_cal(uint8_t gate, uint8_t kind, uint8_t secs) {
    pcal.used = true;
    pcal.req = {};
    pcal.req.magic = LL_MAGIC_CAL;
    pcal.req.version = LL_VERSION;
    pcal.req.gate_id = gate;
    pcal.req.seq = ++cfgSeq;
    pcal.req.kind = kind;
    pcal.req.secs = secs;
    pcal.tries = 0;
    pcal.next = millis();
}

void radio_send_live(uint8_t gate, uint8_t secs) {
    plive.used = true;
    plive.req = {};
    plive.req.magic = LL_MAGIC_LIVEREQ;
    plive.req.version = LL_VERSION;
    plive.req.gate_id = gate;
    plive.req.seq = ++cfgSeq;
    plive.req.secs = secs;
    plive.tries = 0;
    plive.next = millis();
}

// One transmission per call, at least 150 ms apart (the gate needs time to
// re-arm its receiver after acknowledging).
static void serviceTx() {
    uint32_t now = millis();
    if (now - lastTx < 150) return;
    if (pcal.used && (int32_t)(now - pcal.next) >= 0) {
        radio.transmit((uint8_t *)&pcal.req, sizeof(pcal.req));
        radio.startReceive();
        lastTx = millis();
        pcal.next = lastTx + 600;
        if (++pcal.tries >= 2) pcal.used = false;  // the repeat is harmless if the gate already started
        return;
    }
    if (psync.used && (int32_t)(now - psync.next) >= 0) {
        radio.transmit((uint8_t *)&psync.req, sizeof(psync.req));
        radio.startReceive();
        lastTx = millis();
        psync.used = false;  // the reply is the confirmation; the app asks again if it never comes
        return;
    }
    if (plive.used && (int32_t)(now - plive.next) >= 0) {
        radio.transmit((uint8_t *)&plive.req, sizeof(plive.req));
        radio.startReceive();
        lastTx = millis();
        plive.next = lastTx + 500;
        if (++plive.tries >= 2) plive.used = false;  // sent twice: the repeat is harmless if the first arrived
        return;
    }
    for (PendCfg &p : pcfg) {
        if (!p.used || (int32_t)(now - p.next) < 0) continue;
        if (p.tries >= 6) {
            p.used = false;
            app_cfg_tx_result(false);
            Serial.printf("{\"ev\":\"cfg_tx\",\"gate\":%u,\"pilot\":%u,\"seq\":%u,\"ok\":0,\"tries\":%u}\n", p.gate, p.pilot,
                          p.seq, p.tries);
            continue;
        }
        radio.transmit(p.buf, p.len);
        radio.startReceive();
        lastTx = millis();
        p.tries++;
        p.next = lastTx + 600 + p.tries * 250;
        return;
    }
}

static void printHello() {
    Serial.printf("{\"ev\":\"hello\",\"role\":\"lapbase\",\"ver\":%d,\"freq\":%.1f,\"bw\":%.0f,\"sf\":%d}\n",
                  LL_VERSION, LORA_FREQ_MHZ, LORA_BW, LORA_SF);
    app_hello(LORA_FREQ_MHZ, LORA_BW, LORA_SF);
}

static void handleLine(String &l) {
    l.trim();
    if (l == "INFO") {
        printHello();
        return;
    }
    if (l == "SYNC") {
        radio_request_sync(0xFF);
        return;
    }
    {
        unsigned np;
        int off = 0;
        if (sscanf(l.c_str(), "NAME %u %n", &np, &off) >= 1 && np >= 1 && np <= LL_MAX_PILOTS && off > 0) {
            radio_send_name(0xFF, np, l.c_str() + off);
            return;
        }
        unsigned laps, hs, cd, ml;
        if (sscanf(l.c_str(), "SET %u %u %u %u", &laps, &hs, &cd, &ml) == 4) {
            radio_send_set(0xFF, laps, hs ? LL_SET_HOLESHOT : 0, cd, ml);
            return;
        }
    }
    unsigned live_secs;
    if (sscanf(l.c_str(), "LIVE %u", &live_secs) == 1) {
        radio_send_live(0xFF, live_secs > 60 ? 60 : live_secs);
        return;
    }
    char kindw[8];
    unsigned secs = 4;
    if (sscanf(l.c_str(), "CAL %7s %u", kindw, &secs) >= 1 && (!strcmp(kindw, "noise") || !strcmp(kindw, "peak"))) {
        radio_send_cal(0xFF, !strcmp(kindw, "peak") ? LL_CAL_PEAK : LL_CAL_NOISE, secs < 1 ? 1 : secs > 10 ? 10 : secs);
        return;
    }
    char gate[8];
    unsigned pilot, freq, enter, exit_;
    char tail[8] = "";
    int n = sscanf(l.c_str(), "CFG %7s %u %u %u %u %7s", gate, &pilot, &freq, &enter, &exit_, tail);
    if (n >= 5 && pilot >= 1 && pilot <= LL_MAX_PILOTS && enter <= 254 && exit_ <= 254) {
        uint8_t g = gate[0] == '*' ? 0xFF : (uint8_t)atoi(gate);
        sendCfg(g, pilot, freq, enter, exit_, !strcmp(tail, "save"));
    } else if (l.length()) {
        Serial.print("{\"ev\":\"err\",\"why\":\"command\"}\n");
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(USER_LED, OUTPUT);
    digitalWrite(USER_LED, HIGH);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) delay(10);
    app_init();
    web_start();  // WiFi hotspot + pages; works even if the radio fails below

    loraSpi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    int16_t st = radio.begin(LORA_FREQ_MHZ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_TX_DBM, 8, 1.8f, false);
    if (st == RADIOLIB_ERR_NONE) st = radio.setDio2AsRfSwitch(true);
    if (st == RADIOLIB_ERR_NONE) {
        radio.setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);
        st = radio.setCRC(2);
    }
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("{\"ev\":\"err\",\"why\":\"radio_init\",\"code\":%d}\n", st);
        app_radio_error(st);
        return;  // loop() still answers INFO
    }
    radio.startReceive();
    printHello();
}

void loop() {
    static String line;
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\n') {
            handleLine(line);
            line = "";
        } else if (line.length() < 80) {
            line += ch;
        }
    }
    pollRadio();
    serviceTx();
    app_loop();
    uint32_t now = millis();
    if (ledOffAt && (int32_t)(now - ledOffAt) >= 0) {
        digitalWrite(USER_LED, HIGH);
        ledOffAt = 0;
    }
    if (now - lastHb >= 1000) {
        lastHb = now;
        Serial.printf("{\"ev\":\"hb\",\"ms\":%lu}\n", (unsigned long)now);
    }
    delay(1);
}
