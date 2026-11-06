// Race engine + state, a port of the App class in server/lapserver.py so the
// same web pages work unchanged against this device. GPL-3.0-or-later.
#include "app.h"
#include <Preferences.h>
#include <algorithm>
#include <vector>
#include "esp_timer.h"

// ---------------------------------------------------------------- state ----
static const uint16_t RACEBAND[8] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
static const char *COLORS[8] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"};

struct PilotCfg {
    char name[24];
    char color[8];
    uint16_t freq;
    bool enabled;
    uint8_t enter, exit;
};
struct Settings {
    uint8_t laps = 20;       // the page keeps its own per-device target; kept for symmetry with the Mac server
    bool holeshot = true;    // first crossing is only the hole shot, not a counted lap
    uint8_t countdown = 0;
    uint16_t min_lap_ms = 3000;
};
// The base keeps the raw laps of the current round; the page works out standings,
// finishes and call-outs and keeps the history (as the Chorus32 race page does).
struct PilotRun {
    std::vector<uint32_t> laps;  // [0] = hole shot (start -> first crossing), then each lap
    uint32_t ref_ms = 0;
    uint8_t ref_gate = 0;
    double last_t = 0;
};
enum RaceState { IDLE, STAGING, RACING, STOPPED };
struct Race {
    RaceState state = IDLE;
    uint32_t num = 0;
    double go_t = 0, start_t = 0, stop_t = 0;
    bool has_go = false, has_start = false, has_stop = false;
    PilotRun p[8];
};
struct PassEvt {
    double t;
    uint8_t gate, pilot, count, peak, xp, xl;
    uint32_t gate_ms, lap_ms;
    float rssi, snr, age_s;
};
struct LinkPt {
    double t;
    int16_t rssi;
    float snr;
    bool pass;
};
struct GateStatus {
    bool seen = false;
    uint8_t gate = 0;
    uint32_t ms = 0;
    float rssi = 0, snr = 0;
    uint16_t minlap = 0, dropped = 0;
    bool snr_metric = false;
    ll_pilot_cfg_t pilots[LL_MAX_PILOTS];
    double srv_t = 0;
};

static SemaphoreHandle_t lk;
struct Lock {
    Lock() { xSemaphoreTakeRecursive(lk, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(lk); }
};

static PilotCfg pilots[8];
static Settings cfg;
static Race race;
static Preferences prefs;

static const int RECENT_CAP = 60, LINK_CAP = 300, TERM_CAP = 40, SEEN_CAP = 128;
static std::vector<PassEvt> recent;
static std::vector<LinkPt> link_pts;
static std::vector<String> term_lines;
static std::vector<double> term_t;
static std::vector<String> term_src;
struct SeenKey { uint8_t gate, pilot, count; uint32_t gate_ms; };
static SeenKey seen[SEEN_CAP];
static int seen_n = 0, seen_head = 0;

static GateStatus gstat;
static struct {
    float freq = 0, bw = 0;
    int sf = 0;
    bool hello = false;
    double last_hb = 0, last_rx = 0;
    uint32_t pass_pkts = 0, status_pkts = 0, passes_new = 0, passes_dup = 0, bad = 0, cfg_tx = 0, cfg_tx_fail = 0;
    int last_error = 0;
} base;

struct CfgReq { uint8_t gate, pilot; uint16_t freq; uint8_t enter, exit; bool save; };
static std::vector<CfgReq> cfg_q;
// Settings live in the gate's flash. What this base shows is adopted from what the gate reports; an
// edit made here is protected from being overwritten by a stale report for a few seconds.
static const uint32_t ADOPT_HOLDOFF_MS = 20000;
static uint32_t edit_ms[8], name_edit_ms[8], set_edit_ms;
static int synced_rev = -1;
static uint32_t sync_asked_ms;
static bool want_sync;
struct NameReq { uint8_t gate, pilot; char name[LL_NAME_LEN + 1]; };
static std::vector<NameReq> name_q;
struct SetReq { uint8_t gate, laps, flags, countdown; uint16_t min_lap_ms; };
static std::vector<SetReq> set_q;
static bool just_edited(uint32_t t, uint32_t holdoff = ADOPT_HOLDOFF_MS) { return t && (uint32_t)(millis() - t) < holdoff; }

// At most LL_NAME_LEN bytes, never ending in half a UTF-8 character.
static void clip_name(char *dst, const char *src) {
    size_t n = strlen(src);
    if (n > LL_NAME_LEN) {
        n = LL_NAME_LEN;
        size_t k = n;
        while (k > 0 && ((unsigned char)src[k] & 0xC0) == 0x80) k--;  // the cut falls inside a character: back up to its start
        n = k;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

struct LiveReq { uint8_t gate, secs; };
static std::vector<LiveReq> live_q;
struct CalReq { uint8_t gate, kind, secs; };
static std::vector<CalReq> cal_q;
struct CalRes { bool have = false; double t = 0; uint8_t gate = 0, lo[8], avg[8], hi[8]; };
static CalRes cal_res[2];
static void cal_to_json(JsonObject o, const CalRes &c, int kind);

static double mono() { return esp_timer_get_time() / 1e6; }
static bool g_snr = false;  // gate reports SNR codes (dB above noise) instead of RSSI
static float dbfs(int code) { return g_snr ? code / 4.0f : code / 4.0f - 63.75f; }

// ----------------------------------------------------------- persistence ----
static void defaults() {
    for (int i = 0; i < 8; i++) {
        if (i == 0) strlcpy(pilots[i].name, "Ron", sizeof(pilots[i].name));
        else snprintf(pilots[i].name, sizeof(pilots[i].name), "Pilot %d", i + 1);
        strcpy(pilots[i].color, COLORS[i]);
        pilots[i].freq = RACEBAND[i];
        pilots[i].enabled = false;
        pilots[i].enter = 64;
        pilots[i].exit = 32;
    }
    cfg = Settings();
}

static void cfg_to_json(JsonObject o) {
    o["v"] = 2;  // 2: thresholds are SNR codes (1 had RSSI-scale 125/105)
    JsonArray pa = o["pilots"].to<JsonArray>();
    for (int i = 0; i < 8; i++) {
        JsonObject p = pa.add<JsonObject>();
        p["n"] = i + 1;
        p["name"] = pilots[i].name;
        p["color"] = pilots[i].color;
        p["freq"] = pilots[i].freq;
        p["enabled"] = pilots[i].enabled;
        p["enter"] = pilots[i].enter;
        p["exit"] = pilots[i].exit;
    }
    JsonObject s = o["settings"].to<JsonObject>();
    s["laps"] = cfg.laps;
    s["holeshot"] = cfg.holeshot;
    s["countdown"] = cfg.countdown;
    s["min_lap_ms"] = cfg.min_lap_ms;
}

static void save_cfg() {
    JsonDocument d;
    cfg_to_json(d.to<JsonObject>());
    String s;
    serializeJson(d, s);
    prefs.putString("cfg", s);
}

static void load_cfg() {
    defaults();
    String s = prefs.getString("cfg", "");
    if (!s.length()) return;
    JsonDocument d;
    if (deserializeJson(d, s)) return;
    for (JsonObject p : d["pilots"].as<JsonArray>()) {
        int n = p["n"] | 0;
        if (n < 1 || n > 8) continue;
        PilotCfg &c = pilots[n - 1];
        if (p["name"].is<const char *>()) strlcpy(c.name, p["name"], sizeof(c.name));
        if (p["color"].is<const char *>()) strlcpy(c.color, p["color"], sizeof(c.color));
        c.freq = p["freq"] | c.freq;
        c.enabled = p["enabled"] | c.enabled;
        c.enter = p["enter"] | c.enter;
        c.exit = p["exit"] | c.exit;
    }
    if ((d["v"] | 1) < 2)
        for (PilotCfg &c : pilots) { c.enter = 64; c.exit = 32; }  // old thresholds were RSSI codes
    JsonObject s2 = d["settings"];
    cfg.laps = s2["laps"] | cfg.laps;
    cfg.holeshot = s2["holeshot"] | cfg.holeshot;
    cfg.countdown = s2["countdown"] | cfg.countdown;
    cfg.min_lap_ms = s2["min_lap_ms"] | cfg.min_lap_ms;
}

// ------------------------------------------------------------ publishing ----
static void publish(JsonDocument &d) {
    String s;
    serializeJson(d, s);
    web_publish(s);
}

static void race_to_json(JsonObject o) {
    static const char *names[] = {"idle", "staging", "racing", "stopped"};
    o["state"] = names[race.state];
    o["num"] = race.num;
    if (race.has_go) o["go_t"] = race.go_t; else o["go_t"] = nullptr;
    if (race.has_start) o["start_t"] = race.start_t; else o["start_t"] = nullptr;
    if (race.has_stop) o["stop_t"] = race.stop_t; else o["stop_t"] = nullptr;
    o["now"] = mono();
    JsonObject s = o["settings"].to<JsonObject>();
    s["laps"] = cfg.laps;
    s["holeshot"] = cfg.holeshot;
    s["countdown"] = cfg.countdown;
    s["min_lap_ms"] = cfg.min_lap_ms;
    JsonObject laps = o["laps"].to<JsonObject>();
    for (int i = 0; i < 8; i++) {
        JsonArray a = laps[String(i + 1)].to<JsonArray>();
        for (uint32_t l : race.p[i].laps) a.add(l);
    }
}

static void publish_race() {
    JsonDocument d;
    d["type"] = "race";
    race_to_json(d["race"].to<JsonObject>());
    publish(d);
}

static void publish_cfg() {
    JsonDocument d;
    d["type"] = "cfg";
    cfg_to_json(d["cfg"].to<JsonObject>());
    publish(d);
}

static void pass_to_json(JsonObject o, const PassEvt &p) {
    o["type"] = "pass";
    o["source"] = p.gate == 0 ? "sim" : "lora";
    o["gate"] = p.gate;
    o["pilot"] = p.pilot;
    o["count"] = p.count;
    o["t"] = p.t;
    o["gate_ms"] = p.gate_ms;
    o["lap_ms"] = p.lap_ms;
    o["peak"] = p.peak;
    o["peak_dbfs"] = dbfs(p.peak);
    o["xp"] = p.xp;
    o["xl"] = p.xl;
    if (p.gate) {
        o["rssi"] = p.rssi;
        o["snr"] = p.snr;
        o["age_s"] = p.age_s;
    }
}

static void gate_status_to_json(JsonObject o, const GateStatus &g) {
    o["gate"] = g.gate;
    o["ms"] = g.ms;
    o["rssi"] = g.rssi;
    o["snr"] = g.snr;
    o["minlap"] = g.minlap;
    o["dropped"] = g.dropped;
    o["metric"] = g.snr_metric ? "snr" : "rssi";
    JsonArray pa = o["pilots"].to<JsonArray>();
    for (int i = 0; i < LL_MAX_PILOTS; i++) {
        JsonObject p = pa.add<JsonObject>();
        p["n"] = i + 1;
        p["f"] = g.pilots[i].freq_mhz;
        p["e"] = g.pilots[i].enter_at;
        p["x"] = g.pilots[i].exit_at;
    }
    o["srv_t"] = g.srv_t;
}

static void base_to_json(JsonObject o) {
    if (base.hello) {
        JsonObject h = o["hello"].to<JsonObject>();
        h["role"] = "lapbase";
        h["freq"] = base.freq;
        h["bw"] = base.bw;
        h["sf"] = base.sf;
    } else {
        o["hello"] = nullptr;
    }
    o["last_hb"] = base.last_hb;
    if (base.last_rx > 0) o["last_rx"] = base.last_rx; else o["last_rx"] = nullptr;
    o["hb_ms"] = millis();
    o["pass_pkts"] = base.pass_pkts;
    o["status_pkts"] = base.status_pkts;
    o["passes_new"] = base.passes_new;
    o["passes_dup"] = base.passes_dup;
    o["bad"] = base.bad;
    o["cfg_tx"] = base.cfg_tx;
    o["cfg_tx_fail"] = base.cfg_tx_fail;
    JsonArray er = o["errors"].to<JsonArray>();
    if (base.last_error) {
        JsonObject e = er.add<JsonObject>();
        e["ev"] = "err";
        e["why"] = "radio_init";
        e["code"] = base.last_error;
    }
    o["port"] = "WiFi hotspot (this device)";
    o["srv_t"] = mono();
}

static void publish_base() {
    JsonDocument d;
    d["type"] = "base";
    base_to_json(d["d"].to<JsonObject>());
    publish(d);
}

static void publish_link(const LinkPt &l) {
    JsonDocument d;
    d["type"] = "link";
    JsonArray a = d["d"].to<JsonArray>();
    a.add(l.t);
    a.add(l.rssi);
    a.add(l.snr);
    a.add(l.pass ? "pass" : "status");
    publish(d);
}

String app_snapshot() {
    Lock l;
    JsonDocument d;
    d["type"] = "snapshot";
    cfg_to_json(d["cfg"].to<JsonObject>());
    race_to_json(d["race"].to<JsonObject>());
    JsonObject dev = d["devices"].to<JsonObject>();
    dev["gate"] = nullptr;
    dev["base"] = "local";
    dev["unknown"].to<JsonArray>();
    d["gate"].to<JsonObject>();
    JsonObject st = d["status"].to<JsonObject>();
    if (gstat.seen) gate_status_to_json(st[String(gstat.gate)].to<JsonObject>(), gstat);
    base_to_json(d["base"].to<JsonObject>());
    JsonArray lk_ = d["link"].to<JsonArray>();
    int from = link_pts.size() > 120 ? link_pts.size() - 120 : 0;
    for (size_t i = from; i < link_pts.size(); i++) {
        JsonArray a = lk_.add<JsonArray>();
        a.add(link_pts[i].t);
        a.add(link_pts[i].rssi);
        a.add(link_pts[i].snr);
        a.add(link_pts[i].pass ? "pass" : "status");
    }
    JsonArray rc = d["recent"].to<JsonArray>();
    from = recent.size() > 30 ? recent.size() - 30 : 0;
    for (size_t i = from; i < recent.size(); i++) pass_to_json(rc.add<JsonObject>(), recent[i]);
    JsonArray tm = d["term"].to<JsonArray>();
    for (size_t i = 0; i < term_lines.size(); i++) {
        JsonObject e = tm.add<JsonObject>();
        e["type"] = "term";
        e["src"] = term_src[i];
        e["line"] = term_lines[i];
        e["t"] = term_t[i];
    }
    JsonObject cl = d["cal"].to<JsonObject>();
    if (cal_res[0].have) cal_to_json(cl["noise"].to<JsonObject>(), cal_res[0], 0);
    if (cal_res[1].have) cal_to_json(cl["peak"].to<JsonObject>(), cal_res[1], 1);
    d["now"] = mono();
    d["sim"] = false;
    d["local"] = true;
    String out;
    serializeJson(d, out);
    return out;
}

void app_term(const char *src, const char *line) {
    Lock l;
    double t = mono();
    term_src.push_back(src);
    term_lines.push_back(line);
    term_t.push_back(t);
    if ((int)term_lines.size() > TERM_CAP) {
        term_src.erase(term_src.begin());
        term_lines.erase(term_lines.begin());
        term_t.erase(term_t.begin());
    }
    JsonDocument d;
    d["type"] = "term";
    d["src"] = src;
    d["line"] = line;
    d["t"] = t;
    publish(d);
}

// ------------------------------------------------------------- race logic ----
static const size_t MAX_LAPS = 200;

static void add_lap(int i, uint32_t lap) {
    PilotRun &r = race.p[i];
    if (r.laps.size() >= MAX_LAPS) return;
    r.laps.push_back(lap);
    JsonDocument d;
    d["type"] = "lap";
    d["n"] = i + 1;
    d["i"] = (int)r.laps.size() - 1;
    d["ms"] = lap;
    publish(d);
}

static void score(int pilot, uint8_t gate, double t, uint32_t gate_ms) {
    int i = pilot - 1;
    if (race.state != RACING || !pilots[i].enabled || t < race.start_t) return;
    PilotRun &r = race.p[i];
    if (r.laps.empty()) {  // hole shot: start -> first crossing, on the base clock
        r.ref_ms = gate_ms;
        r.ref_gate = gate;
        r.last_t = t;
        add_lap(i, (uint32_t)((t - race.start_t) * 1000));
        return;
    }
    uint32_t lap;
    if (r.ref_gate == gate && gate_ms >= r.ref_ms) lap = gate_ms - r.ref_ms;  // both stamps from the gate clock
    else lap = (uint32_t)((t - r.last_t) * 1000);                              // gate rebooted: base time
    if (lap < cfg.min_lap_ms) return;  // a bounce: keep measuring from the real previous crossing
    r.ref_ms = gate_ms;
    r.ref_gate = gate;
    r.last_t = t;
    add_lap(i, lap);
}

// Returns false for a repeat of a pass already seen (the gate resent it because
// our ACK was lost).
static bool on_pass(uint8_t gate, uint8_t pilot, uint8_t count, double t, uint32_t gate_ms, uint32_t lap_ms,
                    uint8_t peak, float rssi, float snr, float age, uint8_t xp = 0, uint8_t xl = 0) {
    for (int k = 0; k < seen_n; k++) {
        const SeenKey &s = seen[k];
        if (s.gate == gate && s.pilot == pilot && s.count == count && s.gate_ms == gate_ms) return false;
    }
    seen[seen_head] = {gate, pilot, count, gate_ms};
    seen_head = (seen_head + 1) % SEEN_CAP;
    if (seen_n < SEEN_CAP) seen_n++;
    PassEvt e = {t, gate, pilot, count, peak, xp, xl, gate_ms, lap_ms, rssi, snr, age};
    recent.push_back(e);
    if ((int)recent.size() > RECENT_CAP) recent.erase(recent.begin());
    JsonDocument d;
    pass_to_json(d.to<JsonObject>(), e);
    publish(d);
    if (pilot >= 1 && pilot <= 8) score(pilot, gate, t, gate_ms);
    return true;
}

static void race_start() {
    if (race.state == STAGING || race.state == RACING) return;
    uint32_t num = race.num + 1;
    race = Race();
    race.num = num;
    race.go_t = mono() + cfg.countdown;
    race.has_go = true;
    if (cfg.countdown > 0) {
        race.state = STAGING;
    } else {
        race.state = RACING;
        race.start_t = race.go_t;
        race.has_start = true;
    }
    publish_race();
}

static void race_stop() {
    if (race.state == STAGING || race.state == RACING) {
        race.state = STOPPED;
        race.stop_t = mono();
        race.has_stop = true;
    }
    publish_race();
}

static void race_reset() {
    uint32_t num = race.num;
    race = Race();
    race.num = num;
    publish_race();
}

// ------------------------------------------------------------ gate config ----
static uint8_t gate_id() { return gstat.seen ? gstat.gate : 1; }

static void push_pilot(int n) {
    const PilotCfg &p = pilots[n - 1];
    if (cfg_q.size() >= 16) return;
    cfg_q.push_back({gate_id(), (uint8_t)n, (uint16_t)(p.enabled ? p.freq : 0), p.enter, p.exit, false});
}

// ------------------------------------------------------------ radio events ----
void app_hello(float freq, float bw, int sf) {
    Lock l;
    base.freq = freq;
    base.bw = bw;
    base.sf = sf;
    base.hello = true;
}

void app_radio_error(int code) {
    Lock l;
    base.last_error = code;
}

static void add_link(double t, float rssi, float snr, bool pass) {
    LinkPt p = {t, (int16_t)rssi, snr, pass};
    link_pts.push_back(p);
    if ((int)link_pts.size() > LINK_CAP) link_pts.erase(link_pts.begin());
    publish_link(p);
}

void app_rx_pass(uint8_t gate, uint32_t toa_ms, uint32_t sent_ms, float rssi, float snr, const PassEntry *p, int n) {
    Lock l;
    double now = mono();
    base.pass_pkts++;
    base.last_rx = now;
    add_link(now, rssi, snr, true);
    char b[96];
    snprintf(b, sizeof(b), "pass pkt gate %u, %d pass(es), %.0f dBm %.1f dB", gate, n, rssi, snr);
    app_term("base", b);
    for (int i = 0; i < n; i++) {
        float age = (float)((uint32_t)(sent_ms - p[i].pass_ms)) / 1000.0f;
        double t = now - toa_ms / 1000.0 - age;
        if (on_pass(gate, p[i].pilot, p[i].count, t, p[i].pass_ms, p[i].lap_ms, p[i].peak, rssi, snr, age, p[i].xp, p[i].xl)) base.passes_new++;
        else base.passes_dup++;
    }
    publish_base();
}

void app_rx_status(uint8_t gate, uint32_t gate_ms, float rssi, float snr, uint16_t minlap, uint16_t dropped,
                   const ll_pilot_cfg_t pilots_in[LL_MAX_PILOTS], bool snr_metric, uint8_t meta_rev) {
    Lock l;
    double now = mono();
    base.status_pkts++;
    base.last_rx = now;
    add_link(now, rssi, snr, false);
    gstat.seen = true;
    gstat.gate = gate;
    gstat.ms = gate_ms;
    gstat.rssi = rssi;
    gstat.snr = snr;
    gstat.minlap = minlap;
    gstat.dropped = dropped;
    gstat.snr_metric = snr_metric;
    g_snr = snr_metric;
    memcpy(gstat.pilots, pilots_in, sizeof(gstat.pilots));
    gstat.srv_t = now;
    bool changed = false;
    for (int i = 0; i < 8; i++) {
        if (just_edited(edit_ms[i])) continue;
        const ll_pilot_cfg_t &g = pilots_in[i];
        PilotCfg &c = pilots[i];
        bool en = g.freq_mhz != 0;
        if (g.enter_at < 1 || g.enter_at > 254 || g.exit_at < 1 || g.exit_at >= g.enter_at) continue;
        if (c.enabled != en || (en && c.freq != g.freq_mhz) || c.enter != g.enter_at || c.exit != g.exit_at) {
            c.enabled = en;
            if (en) c.freq = g.freq_mhz;
            c.enter = g.enter_at;
            c.exit = g.exit_at;
            changed = true;
        }
    }
    if (!just_edited(set_edit_ms) && minlap && cfg.min_lap_ms != minlap) { cfg.min_lap_ms = minlap; changed = true; }
    if (changed) { save_cfg(); publish_cfg(); publish_race(); }
    if ((int)meta_rev != synced_rev && !just_edited(sync_asked_ms, 6000)) want_sync = true;  // names / race options changed on the gate
    JsonDocument d;
    d["type"] = "status";
    gate_status_to_json(d["d"].to<JsonObject>(), gstat);
    publish(d);
    char b[96];
    snprintf(b, sizeof(b), "status gate %u, %.0f dBm %.1f dB, dropped %u", gate, rssi, snr, dropped);
    app_term("base", b);
    publish_base();
}

void app_rx_sync(uint8_t gate, uint8_t rev, uint8_t laps, uint8_t flags, uint8_t countdown, uint16_t min_lap_ms,
                 const char names_in[8][LL_NAME_LEN]) {
    Lock l;
    bool changed = false;
    for (int i = 0; i < 8; i++) {
        if (just_edited(name_edit_ms[i])) continue;
        char nm[LL_NAME_LEN + 1];
        memcpy(nm, names_in[i], LL_NAME_LEN);
        nm[LL_NAME_LEN] = 0;
        if (nm[0] && strcmp(pilots[i].name, nm)) { strlcpy(pilots[i].name, nm, sizeof(pilots[i].name)); changed = true; }
    }
    if (!just_edited(set_edit_ms)) {
        bool hs = (flags & LL_SET_HOLESHOT) != 0;
        if (cfg.laps != laps || cfg.holeshot != hs || cfg.countdown != countdown || cfg.min_lap_ms != min_lap_ms) {
            cfg.laps = laps;
            cfg.holeshot = hs;
            cfg.countdown = countdown;
            cfg.min_lap_ms = min_lap_ms;
            changed = true;
        }
    }
    synced_rev = rev;
    (void)gate;
    if (changed) { save_cfg(); publish_cfg(); publish_race(); }
    app_term("base", "synced names and race options from the gate");
}

void app_rx_bad() {
    Lock l;
    base.bad++;
    app_term("base", "bad CRC");
    publish_base();
}

// Same message the Mac server makes from the gate's USB stream, so the race page's meters and
// the charts work unchanged over LoRa while live mode is on.
void app_rx_live(uint8_t gate, const uint8_t level[8], const uint8_t hold[8], uint8_t crossing) {
    Lock l;
    JsonDocument d;
    d["type"] = "levels";
    JsonObject o = d["d"].to<JsonObject>();
    uint8_t mask = 0;
    JsonArray lv = o["l"].to<JsonArray>(), h = o["h"].to<JsonArray>();
    for (int i = 0; i < 8; i++) {
        lv.add(level[i]);
        h.add(hold[i]);
        if (level[i] || hold[i]) mask |= 1 << i;
    }
    o["t"] = millis();
    o["c"] = crossing;
    o["e"] = mask;
    o["gate"] = gate;
    o["live"] = true;
    o["srv_t"] = mono();
    publish(d);
}

static void cal_to_json(JsonObject o, const CalRes &c, int kind) {
    o["kind"] = kind == LL_CAL_NOISE ? "noise" : "peak";
    o["gate"] = c.gate;
    o["t"] = c.t;
    JsonArray lo = o["lo"].to<JsonArray>(), avg = o["avg"].to<JsonArray>(), hi = o["hi"].to<JsonArray>();
    for (int i = 0; i < 8; i++) {
        lo.add(c.lo[i]);
        avg.add(c.avg[i]);
        hi.add(c.hi[i]);
    }
}

void app_rx_cal(uint8_t gate, uint8_t kind, const uint8_t lo[8], const uint8_t avg[8], const uint8_t hi[8]) {
    Lock l;
    if (kind > 1) return;
    CalRes &c = cal_res[kind];
    c.have = true;
    c.t = mono();
    c.gate = gate;
    memcpy(c.lo, lo, 8);
    memcpy(c.avg, avg, 8);
    memcpy(c.hi, hi, 8);
    JsonDocument d;
    d["type"] = "cal";
    cal_to_json(d["d"].to<JsonObject>(), c, kind);
    publish(d);
    app_term("base", kind == LL_CAL_NOISE ? "calibration: noise levels received" : "calibration: peak levels received");
}

void app_cfg_tx_result(bool ok) {
    Lock l;
    if (ok) base.cfg_tx++; else base.cfg_tx_fail++;
    publish_base();
}

// ----------------------------------------------------------------- loop ----
void app_loop() {
    static double last_sec = 0;
    CfgReq req;
    CalReq creq;
    bool have = false, have_cal = false;
    {
        Lock l;
        double now = mono();
        if (race.state == STAGING && now >= race.go_t) {
            race.state = RACING;
            race.start_t = race.go_t;
            race.has_start = true;
            publish_race();
        }
        if (now - last_sec >= 1.0) {
            last_sec = now;
            base.last_hb = now;
            publish_base();
        }
        if (!cfg_q.empty()) {
            req = cfg_q.front();
            cfg_q.erase(cfg_q.begin());
            have = true;
        }
        if (!cal_q.empty()) {
            creq = cal_q.front();
            cal_q.erase(cal_q.begin());
            have_cal = true;
        }
    }
    NameReq nreq;
    SetReq sreq;
    bool have_name = false, have_set = false, do_sync = false;
    {
        Lock l;
        if (!name_q.empty()) { nreq = name_q.front(); name_q.erase(name_q.begin()); have_name = true; }
        if (!set_q.empty()) { sreq = set_q.front(); set_q.erase(set_q.begin()); have_set = true; }
        if (want_sync) { want_sync = false; do_sync = true; sync_asked_ms = millis() | 1; }
    }
    if (have_name) radio_send_name(nreq.gate, nreq.pilot, nreq.name);
    if (have_set) radio_send_set(sreq.gate, sreq.laps, sreq.flags, sreq.countdown, sreq.min_lap_ms);
    if (do_sync) radio_request_sync(gate_id());
    LiveReq lreq;
    bool have_live = false;
    {
        Lock l;
        if (!live_q.empty()) { lreq = live_q.front(); live_q.erase(live_q.begin()); have_live = true; }
    }
    if (have_live) radio_send_live(lreq.gate, lreq.secs);
    if (have_cal) radio_send_cal(creq.gate, creq.kind, creq.secs);
    if (have) radio_send_cfg(req.gate, req.pilot, req.freq, req.enter, req.exit, req.save);  // outside the lock: slow
}

void app_init() {
    lk = xSemaphoreCreateRecursiveMutex();
    prefs.begin("lap", false);
    load_cfg();
    race = Race();
}

// ------------------------------------------------------------------ REST ----
static bool handle_cfg_line(const String &line, String &resp, int &code) {
    String l = line;
    l.trim();
    if (l == "INFO") {
        app_term("tx:base", "INFO");
        resp = "{\"ok\":true}";
        return true;
    }
    char gate[8], tail[8] = "";
    unsigned pilot, freq, enter, exit_;
    int n = sscanf(l.c_str(), "CFG %7s %u %u %u %u %7s", gate, &pilot, &freq, &enter, &exit_, tail);
    if (n >= 5 && pilot >= 1 && pilot <= 8 && enter <= 254 && exit_ <= 254) {
        uint8_t g = gate[0] == '*' ? 0xFF : (uint8_t)atoi(gate);
        if (cfg_q.size() < 16) cfg_q.push_back({g, (uint8_t)pilot, (uint16_t)freq, (uint8_t)enter, (uint8_t)exit_, !strcmp(tail, "save")});
        app_term("tx:base", l.c_str());
        resp = "{\"ok\":true}";
        return true;
    }
    code = 400;
    resp = "{\"error\":\"unknown command\"}";
    return false;
}

int app_post(const String &path, JsonDocument &body, String &resp) {
    Lock l;
    resp = "{\"ok\":true}";
    if (path == "/api/race/start") {
        race_start();
    } else if (path == "/api/race/stop") {
        race_stop();
    } else if (path == "/api/race/reset") {
        race_reset();
    } else if (path == "/api/pilot") {
        int n = body["n"] | 0;
        if (n < 1 || n > 8) { resp = "{\"error\":\"pilot 1-8\"}"; return 400; }
        PilotCfg p = pilots[n - 1];
        bool name_changed = false, cfg_changed = false;
        if (body["name"].is<const char *>()) {
            char nm[LL_NAME_LEN + 1];
            clip_name(nm, body["name"]);
            if (nm[0] && strcmp(nm, p.name)) { strlcpy(p.name, nm, sizeof(p.name)); name_changed = true; }
        }
        if (body["color"].is<const char *>()) strlcpy(p.color, body["color"], sizeof(p.color));
        if (!body["freq"].isNull() && p.freq != body["freq"].as<int>()) { p.freq = body["freq"].as<int>(); cfg_changed = true; }
        if (!body["enabled"].isNull() && p.enabled != body["enabled"].as<bool>()) { p.enabled = body["enabled"].as<bool>(); cfg_changed = true; }
        if (!body["enter"].isNull() && p.enter != body["enter"].as<int>()) { p.enter = body["enter"].as<int>(); cfg_changed = true; }
        if (!body["exit"].isNull() && p.exit != body["exit"].as<int>()) { p.exit = body["exit"].as<int>(); cfg_changed = true; }
        if (!(p.exit >= 1 && p.exit < p.enter && p.enter <= 254)) {
            resp = "{\"error\":\"need 1 <= exit < enter <= 254\"}";
            return 400;
        }
        pilots[n - 1] = p;
        save_cfg();
        bool push = body["push"] | true;
        if (cfg_changed) edit_ms[n - 1] = millis() | 1;
        if (name_changed) {
            name_edit_ms[n - 1] = millis() | 1;
            if (name_q.size() < 8) {
                NameReq r = {gate_id(), (uint8_t)n, {}};
                strlcpy(r.name, p.name, sizeof(r.name));
                // a newer edit of the same pilot's name replaces one still waiting
                for (size_t k = 0; k < name_q.size(); k++)
                    if (name_q[k].pilot == n) { name_q.erase(name_q.begin() + k); break; }
                name_q.push_back(r);
            }
        }
        if (push && cfg_changed) push_pilot(n);
        publish_cfg();
        publish_race();
        resp = (push && cfg_changed) ? "{\"ok\":true,\"pushed_via\":\"lora\"}" : "{\"ok\":true,\"pushed_via\":null}";
    } else if (path == "/api/settings") {
        bool changed = false;
        auto upd = [&](auto &field, int v) { if ((int)field != v) { field = v; changed = true; } };
        if (!body["laps"].isNull()) upd(cfg.laps, constrain(body["laps"].as<int>(), 1, 99));
        if (!body["countdown"].isNull()) upd(cfg.countdown, constrain(body["countdown"].as<int>(), 0, 30));
        if (!body["holeshot"].isNull()) upd(cfg.holeshot, body["holeshot"].as<bool>() ? 1 : 0);
        if (!body["min_lap_ms"].isNull()) upd(cfg.min_lap_ms, constrain(body["min_lap_ms"].as<int>(), 1000, 120000));
        save_cfg();
        if (changed) {
            set_edit_ms = millis() | 1;
            if (set_q.size() < 2) set_q.push_back({gate_id(), cfg.laps, (uint8_t)(cfg.holeshot ? LL_SET_HOLESHOT : 0), cfg.countdown, cfg.min_lap_ms});
        }
        publish_cfg();
        publish_race();
    } else if (path == "/api/base/cmd") {
        int code = 200;
        String line = body["line"] | "";
        if (!handle_cfg_line(line, resp, code)) return code;
    } else if (path == "/api/gate/cmd") {
        resp = "{\"error\":\"no gate on USB (this base reaches the gate over LoRa)\"}";
        return 409;
    } else if (path == "/api/gate/cal") {
        String kind = body["kind"] | "noise";
        int secs = constrain(body["secs"] | 4, 1, 10);
        if (cal_q.size() < 4) cal_q.push_back({gate_id(), (uint8_t)(kind == "peak" ? LL_CAL_PEAK : LL_CAL_NOISE), (uint8_t)secs});
        app_term("tx:base", kind == "peak" ? "calibrate: measure peak" : "calibrate: measure noise");
        resp = "{\"ok\":true,\"secs\":" + String(secs) + "}";
    } else if (path == "/api/gate/live") {
        int secs = constrain(body["secs"] | 60, 0, 60);
        if (live_q.size() < 4) live_q.push_back({gate_id(), (uint8_t)secs});
    } else if (path == "/api/gate/save") {
        // The gate saves a pilot's settings when asked to over LoRa.
        for (int n = 1; n <= 8; n++)
            if (cfg_q.size() < 16)
                cfg_q.push_back({gate_id(), (uint8_t)n, (uint16_t)(pilots[n - 1].enabled ? pilots[n - 1].freq : 0),
                                 pilots[n - 1].enter, pilots[n - 1].exit, true});
    } else if (path == "/api/sim/pass") {
        int n = body["pilot"] | 1;
        if (n < 1 || n > 8) { resp = "{\"error\":\"pilot 1-8\"}"; return 400; }
        static uint8_t sim_count[9];
        static uint32_t sim_prev[9];
        uint32_t ms = millis();
        sim_count[n]++;
        on_pass(0, n, sim_count[n], mono(), ms, sim_prev[n] ? ms - sim_prev[n] : 0, 200, 0, 0, 0);
        sim_prev[n] = ms;
    } else {
        resp = "{\"error\":\"unknown\"}";
        return 404;
    }
    return 200;
}
