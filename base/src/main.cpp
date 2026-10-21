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
//        INFO
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "lap_link.h"

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
        Serial.printf("%s{\"n\":%u,\"c\":%u,\"t\":%lu,\"lap\":%lu,\"pk\":%u}", i ? "," : "", pk.p[i].pilot,
                      pk.p[i].count, (unsigned long)pk.p[i].pass_ms, (unsigned long)pk.p[i].lap_ms, pk.p[i].peak);
    Serial.print("]}\n");
}

static void onStatus(const uint8_t *b, size_t len, uint32_t rxMs, float rssi, float snr) {
    ll_status_pkt_t s;
    if (len != sizeof(s)) return;
    memcpy(&s, b, len);
    sendAck(s.gate_id, s.seq, rssi, snr);
    Serial.printf("{\"ev\":\"status\",\"gate\":%u,\"ms\":%lu,\"sent_ms\":%lu,\"rssi\":%.0f,\"snr\":%.1f,"
                  "\"minlap\":%u,\"dropped\":%u,\"pilots\":[",
                  s.gate_id, (unsigned long)rxMs, (unsigned long)s.sent_ms, rssi, snr, s.min_lap_ms_div10 * 10,
                  s.dropped);
    for (int i = 0; i < LL_MAX_PILOTS; i++)
        Serial.printf("%s{\"n\":%d,\"f\":%u,\"e\":%u,\"x\":%u}", i ? "," : "", i + 1, s.pilot[i].freq_mhz,
                      s.pilot[i].enter_at, s.pilot[i].exit_at);
    Serial.print("]}\n");
}

static void pollRadio() {
    if (!packetReady()) return;
    uint32_t rxMs = millis();
    uint8_t buf[64];
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
    } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
        Serial.printf("{\"ev\":\"bad\",\"ms\":%lu,\"why\":\"crc\"}\n", (unsigned long)rxMs);
    }
    radio.startReceive();
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
    int16_t st = radio.transmit((uint8_t *)&c, sizeof(c));
    radio.startReceive();
    Serial.printf("{\"ev\":\"cfg_tx\",\"gate\":%u,\"pilot\":%u,\"seq\":%u,\"ok\":%d}\n", gate, pilot, c.seq,
                  st == RADIOLIB_ERR_NONE);
}

static void printHello() {
    Serial.printf("{\"ev\":\"hello\",\"role\":\"lapbase\",\"ver\":%d,\"freq\":%.1f,\"bw\":%.0f,\"sf\":%d}\n",
                  LL_VERSION, LORA_FREQ_MHZ, LORA_BW, LORA_SF);
}

static void handleLine(String &l) {
    l.trim();
    if (l == "INFO") {
        printHello();
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

    loraSpi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    int16_t st = radio.begin(LORA_FREQ_MHZ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_TX_DBM, 8, 1.8f, false);
    if (st == RADIOLIB_ERR_NONE) st = radio.setDio2AsRfSwitch(true);
    if (st == RADIOLIB_ERR_NONE) {
        radio.setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);
        st = radio.setCRC(2);
    }
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("{\"ev\":\"err\",\"why\":\"radio_init\",\"code\":%d}\n", st);
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
