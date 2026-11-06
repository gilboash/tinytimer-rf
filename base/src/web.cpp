// WiFi hotspot + web server for the base station: the same pages and API as
// server/lapserver.py, served from the S3's flash. GPL-3.0-or-later.
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include "app.h"
#include "web_assets.h"

#ifndef AP_SSID
#define AP_SSID "tinytimer_breaking"
#endif
// Open network (no password) by design: a phone at the track just joins. Define AP_PASS
// (8+ characters) to turn WPA2 back on.

static AsyncWebServer server(80);
static AsyncEventSource events("/events");

/* ---- captive portal ----
 * Phones and laptops probe a known URL when they join a WiFi network and pop a "sign in" page if the
 * answer is not what the real internet would give. So: (1) a DNS server that answers every name with
 * this device, so the probe reaches us; (2) the probe URLs of Android, iOS/macOS, Windows and Firefox,
 * answered with a redirect to the app. A client that has actually opened the app (a live /events
 * connection in the last 10 minutes) gets the "internet is fine" answer instead: Android only keeps
 * using a WiFi network that passes its check, and a phone that falls back to mobile data can no longer
 * reach this page. */
static DNSServer dns;
static const uint32_t OK_FOR_MS = 10 * 60 * 1000;
static struct { uint32_t ip, at; } ok_clients[8];

static void mark_ok(IPAddress ip) {
    uint32_t a = (uint32_t)ip;
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (ok_clients[i].ip == a) { slot = i; break; }
        if (!ok_clients[i].at || (uint32_t)(millis() - ok_clients[i].at) > OK_FOR_MS) slot = i;
    }
    ok_clients[slot] = {a, millis() | 1};
}

static bool is_ok(IPAddress ip) {
    uint32_t a = (uint32_t)ip;
    for (auto &c : ok_clients)
        if (c.ip == a && c.at && (uint32_t)(millis() - c.at) < OK_FOR_MS) return true;
    return false;
}

static void portal_redirect(AsyncWebServerRequest *r) {
    AsyncWebServerResponse *resp = r->beginResponse(302, "text/plain", "tinytimer");
    resp->addHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
}

struct Probe { const char *path; int code; const char *type; const char *body; };
static const Probe PROBES[] = {
    {"/generate_204", 204, "text/plain", ""},                       // Android
    {"/gen_204", 204, "text/plain", ""},
    {"/hotspot-detect.html", 200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},  // iOS, macOS
    {"/library/test/success.html", 200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},
    {"/connecttest.txt", 200, "text/plain", "Microsoft Connect Test"},  // Windows
    {"/ncsi.txt", 200, "text/plain", "Microsoft NCSI"},
    {"/success.txt", 200, "text/plain", "success\n"},                 // Firefox
    {"/canonical.html", 200, "text/html", "<meta http-equiv=\"refresh\" content=\"0;url=https://support.mozilla.org/kb/captive-portal\"/>"},
    {"/redirect", 0, "", ""},
    {"/fwlink", 0, "", ""},
};

static void dns_task(void *) {
    for (;;) {
        dns.processNextRequest();
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void web_publish(const String &json) {
    if (events.count()) events.send(json.c_str(), nullptr, millis());
}

static void serve_asset(AsyncWebServerRequest *r, const WebAsset *a) {
    AsyncWebServerResponse *resp = r->beginResponse(200, a->mime, a->data, a->len);
    resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("Cache-Control", "no-cache");
    r->send(resp);
}

static void post_body(AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
    if (total > 2048) return;
    if (index == 0) r->_tempObject = calloc(total + 1, 1);  // freed by the library with the request
    if (r->_tempObject) memcpy((uint8_t *)r->_tempObject + index, data, len);
}

static void post_done(AsyncWebServerRequest *r) {
    JsonDocument body;
    if (r->_tempObject && deserializeJson(body, (const char *)r->_tempObject)) {
        r->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    String resp;
    int code = app_post(r->url(), body, resp);
    r->send(code, "application/json", resp);
}

void web_start() {
    WiFi.mode(WIFI_AP);
#ifdef AP_PASS
    WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 4);
#else
    WiFi.softAP(AP_SSID, nullptr, 6, 0, 4);
#endif
    IPAddress ip = WiFi.softAPIP();
    char b[96];
    snprintf(b, sizeof(b), "hotspot \"%s\" up, open http://%s", AP_SSID, ip.toString().c_str());
    Serial.printf("{\"ev\":\"wifi\",\"ssid\":\"%s\",\"ip\":\"%s\"}\n", AP_SSID, ip.toString().c_str());
    app_term("base", b);

    events.onConnect([](AsyncEventSourceClient *c) {
        mark_ok(c->client()->remoteIP());
        String s = app_snapshot();
        c->send(s.c_str(), nullptr, millis(), 2000);
    });
    server.addHandler(&events);

    for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
        const WebAsset *a = &WEB_ASSETS[i];
        server.on(a->path, HTTP_GET, [a](AsyncWebServerRequest *r) { serve_asset(r, a); });
    }
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *r) { r->send(200, "application/json", app_snapshot()); });
    static const char *posts[] = {"/api/race/start", "/api/race/stop", "/api/race/reset", "/api/pilot", "/api/settings",
                                  "/api/base/cmd", "/api/gate/cmd", "/api/gate/save", "/api/gate/cal", "/api/gate/live", "/api/sim/pass"};
    for (const char *p : posts) server.on(p, HTTP_POST, post_done, nullptr, post_body);
    for (const Probe &p : PROBES)
        server.on(p.path, HTTP_GET, [&p](AsyncWebServerRequest *r) {
            if (p.code && is_ok(r->client()->remoteIP())) {
                if (p.code == 204) r->send(204);
                else r->send(p.code, p.type, p.body);
            } else {
                portal_redirect(r);
            }
        });
    // Anything else a captive-portal check or a mistyped address asks for lands on the app.
    server.onNotFound([](AsyncWebServerRequest *r) {
        if (r->url().startsWith("/api/")) r->send(404, "application/json", "{\"error\":\"not found\"}");
        else portal_redirect(r);
    });
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", ip);
    xTaskCreate(dns_task, "dns", 3072, nullptr, 2, nullptr);
    server.begin();
}
