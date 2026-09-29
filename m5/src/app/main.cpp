// The M5 remote: foc312 on the OSSM M5 Remote, driving a FOC-Stim (fork firmware) over Wi-Fi (notes/m5-remote.md).
// Everything with logic in it is remote/core (C, tested on the host against the Python engine); this file is the
// hardware glue: knobs and buttons, the screen, LittleFS, the USB loader, Wi-Fi and the box's TCP link.
//
// Tasks (PlaStim, 2026-09-28: Waves felt chunky - drawing the screen held up the engine for tens of ms at a time):
//   control  every 16 ms, high priority: knobs, buttons, engine, safety, the box link's reads and writes, traces.
//            Nothing slow ever runs here, so the values reach the box at a steady ~60 Hz (17 ms glide in between).
//   network  low priority: Wi-Fi and (re)connecting. It connects a spare socket and swaps it in under the lock, so a
//            slow connect never holds up control.
//   loop     Arduino's loop: the USB loader, saving settings, and the screen, drawn from a snapshot taken under the
//            lock in microseconds.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP32Encoder.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <errno.h>
#include <esp_netif.h>
#include <esp_netif_sta_list.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <lwip/sockets.h>

extern "C" {
#include "boxlink.h"
#include "ctrl.h"
#include "loader.h"
#include "pack.h"
}

// ---- the board (measured with the hwtest firmware, notes/m5-remote.md) ----------------------------------------
static const int ENC_PINS[4][2] = {{5, 9}, {18, 17}, {1, 2}, {7, 6}};   // knobs 1..4 as printed on the remote
// what each knob does on each screen is remote/core/ctrl.h's business (knob 1 master / scroll, 2 MA / change, 3-4 levels)
static const int ENC_DIR[4] = {+1, +1, +1, +1};                          // clockwise = up (checked by PlaStim)
static const int PIN_MX = 10, PIN_PUSH1 = 8, PIN_PUSH4 = 14;             // high = pressed
static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t CONTROL_PERIOD_MS = 16;
#define HIST_N 200                            // trace: ~10 s at one sample per 3 control ticks

// ---- state ----------------------------------------------------------------------------------------------------
struct Box { char name[25]; char host[64]; uint16_t port; uint8_t mac[6]; bool has_mac; };
struct Config {
    char ssid[33], pass[65];
    bool direct;                              // the remote runs its own network and the boxes join it
    int channel;
    Box boxes[8];
    int nboxes;
    float cap, slow, dead, ramp, mono;
    bool ok;
    char error[64];
} cfg;

static SemaphoreHandle_t mtx;                 // guards everything below that control touches
static uint8_t *pack_buf = nullptr;
static size_t pack_len = 0;
static pack_t pack;
static bool pack_ok = false;
static ctrl_t ctl;
static ctrl_settings_t saved_set;
static boxlink_t blink;
static loader_t ldr;
static WiFiClient tcp;
static ESP32Encoder enc[4];
static long enc_base[4];
static M5Canvas canvas(&M5.Display);

static char last_fault[120] = "";
static float hist[2][HIST_N];                 // pattern intensity per wire position, 0..1 (gate and pad included)
static float hist_rate[2][HIST_N];            // pulse rate per wire position, log-scaled 10..400 Hz -> 0..1
static float hist_width[2][HIST_N];           // pulse width per wire position as sent (40..400 us), 0..400 -> 0..1
static int hist_pos = 0, hist_div = 0;
static volatile bool reconnect_now = true;    // network: connect as soon as possible
static uint32_t tick_gap_max = 0, tick_gap_shown = 0, tick_window_ms = 0, rtt_max_shown = 0;

// what the screen draws from (copied under the lock)
static ctrl_t sc;
static boxlink_t sb;
static float shist[2][HIST_N], shist_rate[2][HIST_N], shist_width[2][HIST_N];
static int shist_pos;
static uint32_t stick_gap, srtt_max;
static volatile int box_rssi = 0;            // direct: the box's signal as the remote hears it (0 = not joined)
static volatile int n_joined = 0;            // direct: stations on the remote's network

static double now_s() { return esp_timer_get_time() / 1e6; }

struct Lock {
    Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mtx); }
};

// ---- files ----------------------------------------------------------------------------------------------------
static bool load_config() {
    memset(&cfg, 0, sizeof cfg);
    cfg.cap = 0.15f; cfg.slow = 4; cfg.dead = 2; cfg.ramp = 3; cfg.mono = 3;
    File f = LittleFS.open("/config.json", "r");
    if (!f) { snprintf(cfg.error, sizeof cfg.error, "no config.json (load from the PC)"); return false; }
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e || strcmp(doc["format"] | "", "stim-remote config v1") != 0) {
        snprintf(cfg.error, sizeof cfg.error, "config.json unreadable");
        return false;
    }
    strlcpy(cfg.ssid, doc["wifi"]["ssid"] | "", sizeof cfg.ssid);
    strlcpy(cfg.pass, doc["wifi"]["password"] | "", sizeof cfg.pass);
    cfg.direct = strcmp(doc["wifi"]["mode"] | "house", "direct") == 0;
    cfg.channel = doc["wifi"]["channel"] | 6;
    if (cfg.channel < 1 || cfg.channel > 11) cfg.channel = 6;
    for (JsonObject b : doc["boxes"].as<JsonArray>()) {
        if (cfg.nboxes >= 8) break;
        Box &x = cfg.boxes[cfg.nboxes++];
        strlcpy(x.name, b["name"] | "box", sizeof x.name);
        strlcpy(x.host, b["host"] | "", sizeof x.host);
        x.port = b["port"] | 55533;
        unsigned m[6];
        x.has_mac = sscanf(b["mac"] | "", "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6;
        for (int i = 0; i < 6; i++) x.mac[i] = (uint8_t)m[i];
    }
    float cap = doc["safety"]["amps_cap"] | 0.15f;
    cfg.cap = cap > 0.2f ? 0.2f : (cap < 0 ? 0 : cap);                       // the hard cap, whatever the file says
    cfg.slow = doc["safety"]["slow_start_s"] | 4.0f;
    cfg.dead = doc["safety"]["deadman_silence_s"] | 2.0f;
    cfg.ramp = doc["safety"]["deadman_ramp_down_s"] | 3.0f;
    cfg.mono = doc["et312"]["monophasic_asymmetry"] | 3.0f;
    if (!cfg.ssid[0] || !cfg.nboxes) { snprintf(cfg.error, sizeof cfg.error, "config.json: no Wi-Fi or no box"); return false; }
    cfg.ok = true;
    return true;
}

// House mode: join the house Wi-Fi like any client. Direct mode: the remote is the access point and the boxes join
// it (stimengine.remote pair-box): one hop through the air instead of two and no busy house AP in between (PlaStim's
// Living-room AP measured 78 % channel use and ~69 % retries to the remote, 2026-09-28).
static void wifi_start() {
    if (!cfg.ok) return;
    WiFi.disconnect();
    if (cfg.direct) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(cfg.ssid, cfg.pass, cfg.channel, 0, 4);
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);    // 20 MHz: less to collide with on a crowded band
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.begin(cfg.ssid, cfg.pass);
    }
}

// Direct mode: the chosen box's address on our network, found by its Wi-Fi MAC (or the only station, when the box
// has no MAC in the config). False while it has not joined or has no address yet.
static bool find_box(const Box &b, char *host, size_t cap) {
    wifi_sta_list_t wl;
    esp_netif_sta_list_t nl;
    box_rssi = 0;
    if (esp_wifi_ap_get_sta_list(&wl) != ESP_OK || esp_netif_get_sta_list(&wl, &nl) != ESP_OK) return false;
    n_joined = nl.num;
    for (int i = 0; i < nl.num; i++) {
        bool match = b.has_mac ? memcmp(nl.sta[i].mac, b.mac, 6) == 0 : nl.num == 1;
        if (!match || nl.sta[i].ip.addr == 0) continue;
        box_rssi = wl.sta[i].rssi;
        esp_ip4addr_ntoa(&nl.sta[i].ip, host, (int)cap);
        return true;
    }
    return false;
}

static void load_pack() {
    pack_ok = false;
    File f = LittleFS.open("/patterns.bin", "r");
    if (!f) return;
    size_t n = f.size();
    uint8_t *buf = (uint8_t *)(psramFound() ? ps_malloc(n) : malloc(n));
    if (!buf) { f.close(); return; }
    f.read(buf, n);
    f.close();
    if (pack_open(&pack, buf, n) == PACK_OK) {
        free(pack_buf);
        pack_buf = buf;
        pack_len = n;
        pack_ok = true;
    } else {
        free(buf);
    }
}

static bool load_settings(ctrl_settings_t *s) {
    File f = LittleFS.open("/settings.bin", "r");
    if (!f) return false;
    uint32_t tag = 0;
    bool ok = f.read((uint8_t *)&tag, 4) == 4 && tag == 0x31534352u /* "RCS1" */ &&
              f.read((uint8_t *)s, sizeof *s) == sizeof *s;
    f.close();
    return ok;
}

static void save_settings(const ctrl_settings_t &s) {
    File f = LittleFS.open("/settings.bin", "w");
    if (!f) return;
    uint32_t tag = 0x31534352u;
    f.write((const uint8_t *)&tag, 4);
    f.write((const uint8_t *)&s, sizeof s);
    f.close();
}

static void start_controller() {
    ctrl_settings_t s;
    bool have = load_settings(&s);
    ctrl_init(&ctl, pack_ok ? &pack : nullptr, have ? &s : nullptr, cfg.nboxes ? cfg.nboxes : 1, now_s(), cfg.slow,
              cfg.dead, cfg.ramp, cfg.cap, cfg.mono, esp_random());
    saved_set = ctl.set;
}

// ---- the box link's socket ------------------------------------------------------------------------------------
// The box link writes into a buffer; control sends it in one piece per tick (tcp_flush). Never blocks:
// WiFiClient::write retries a dead peer for up to ~10 s; one send per tick also keeps lwIP's ~16-segment queue from
// filling on the 17-message bursts (connect, the 1 s refresh), which a send per message did ("link write failed").
// Only a buffer the box hasn't taken for 1.5 s counts as a failure.
static uint8_t txbuf[8192];
static size_t txlen = 0;
static uint32_t tx_stuck_ms = 0;

static bool tcp_write(void *, const uint8_t *d, size_t n) {
    if (!tcp.connected() || txlen + n > sizeof txbuf) return false;
    memcpy(txbuf + txlen, d, n);
    txlen += n;
    return true;
}

static bool tcp_flush() {
    if (!txlen) { tx_stuck_ms = 0; return true; }
    int r = lwip_send(tcp.fd(), txbuf, txlen, MSG_DONTWAIT);
    if (r > 0) {
        memmove(txbuf, txbuf + r, txlen - (size_t)r);
        txlen -= (size_t)r;
        tx_stuck_ms = 0;
        return true;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!tx_stuck_ms) tx_stuck_ms = millis();
        return millis() - tx_stuck_ms < 1500;
    }
    return false;
}

static bool link_live() { return blink.state != BOXLINK_IDLE && blink.state != BOXLINK_FAULT; }

// ---- USB loader (patterns + settings from the PC; refused while armed). Runs in loop() under the lock. -----------
static File part;
static char part_path[48];

static void l_send(void *, const char *line) { Serial.print(line); }
static bool l_busy(void *) { return ctrl_armed(&ctl); }
static uint32_t l_free(void *) { return (uint32_t)(LittleFS.totalBytes() - LittleFS.usedBytes()); }
static bool l_open(void *, const char *name, uint32_t) {
    snprintf(part_path, sizeof part_path, "/.%s.part", name);
    part = LittleFS.open(part_path, "w");
    return (bool)part;
}
static bool l_write(void *, const uint8_t *d, size_t n) { return part && part.write(d, n) == n; }
static bool l_commit(void *, const char *name) {
    if (!part) return false;
    part.close();
    char path[48];
    snprintf(path, sizeof path, "/%s", name);
    if (LittleFS.exists(path)) LittleFS.remove(path);
    return LittleFS.rename(part_path, path);
}
static void l_abort(void *) { if (part) part.close(); if (LittleFS.exists(part_path)) LittleFS.remove(part_path); }
static bool l_remove(void *, const char *name) {
    char path[48];
    snprintf(path, sizeof path, "/%s", name);
    return LittleFS.exists(path) && LittleFS.remove(path);
}
static bool l_list(void *, int index, char *name, size_t cap, uint32_t *size) {
    File root = LittleFS.open("/");
    int i = 0;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (f.name()[0] == '.') continue;
        if (i++ == index) { strlcpy(name, f.name(), cap); *size = f.size(); return true; }
    }
    return false;
}
static bool l_reload(void *, char *reason, size_t cap) {
    bool ok = load_config();
    load_pack();
    if (!ok) { strlcpy(reason, cfg.error, cap); return false; }
    if (!pack_ok) { strlcpy(reason, "patterns.bin missing or damaged", cap); return false; }
    tcp.stop();
    txlen = 0;
    boxlink_io_t io = blink.io;
    boxlink_init(&blink, &io, cfg.cap);
    start_controller();
    wifi_start();
    reconnect_now = true;
    return true;
}

// ---- inputs (control task) ------------------------------------------------------------------------------------
struct Button { int pin; bool state; uint32_t changed; };
static Button btn_mx = {PIN_MX, false, 0}, btn_p1 = {PIN_PUSH1, false, 0}, btn_p4 = {PIN_PUSH4, false, 0};

static bool pressed(Button &b) {           // debounced rising edge
    bool v = digitalRead(b.pin) == HIGH;
    uint32_t ms = millis();
    if (v != b.state && ms - b.changed > DEBOUNCE_MS) {
        b.state = v;
        b.changed = ms;
        return v;
    }
    return false;
}

static void inputs(double t) {
    for (int i = 0; i < 4; i++) {
        long c = enc[i].getCount();
        long det = (c - enc_base[i]) / 2;     // 2 counts per detent; a bounce mid-detent adds or loses nothing
        if (det) {
            enc_base[i] += det * 2;
            ctrl_knob(&ctl, (ctrl_knob_t)i, (int)det * ENC_DIR[i], t);
        }
    }
    if (pressed(btn_mx)) ctrl_button(&ctl, t);
    if (pressed(btn_p1)) ctrl_push_knob1(&ctl, t);
    if (pressed(btn_p4)) ctrl_push_knob4(&ctl, t);
}

// ---- control task ---------------------------------------------------------------------------------------------
static void control_step() {
    Lock lock;
    uint32_t ms = millis();
    double t = now_s();
    int prev_box = ctl.set.box;
    inputs(t);
    if (ctl.set.box != prev_box) {            // the options switched boxes (ctrl already disarmed)
        tcp.stop();
        txlen = 0;
        boxlink_disconnected(&blink, "switching box");
        reconnect_now = true;
    }
    if (link_live()) {
        if (!tcp.connected()) {
            boxlink_disconnected(&blink, "the box closed the connection");
        } else {
            uint8_t buf[256];
            while (tcp.available()) {
                int n = tcp.read(buf, sizeof buf);
                if (n <= 0) break;
                boxlink_feed(&blink, buf, (size_t)n, ms);
            }
            boxlink_poll(&blink, ms);
        }
    }
    // the link's state into the controller: a new fault stops and zeroes the levels (once per fault)
    bool running = blink.state == BOXLINK_RUNNING;
    if (blink.state == BOXLINK_FAULT && strcmp(blink.fault, last_fault) != 0) {
        strlcpy(last_fault, blink.fault, sizeof last_fault);
        ctrl_link(&ctl, false, blink.fault, t);
    } else {
        if (blink.state != BOXLINK_FAULT) last_fault[0] = 0;
        ctrl_link(&ctl, running, nullptr, t);
    }
    safety_values_t v;
    ctrl_tick(&ctl, t, &v);
    ctrl_set_box_fork(&ctl, running ? blink.fork_version : 0, t);   // the shapes on offer follow the box's firmware
    if (running) {
        boxlink_set_playing(&blink, ctrl_armed(&ctl), ms);   // the box says Playing / Idle with the remote
        boxlink_send_values(&blink, &v, ms);
    }
    if (link_live() && !tcp_flush()) boxlink_disconnected(&blink, "the box stopped taking data");
    // trace: the pattern's own intensity per wire position (so its shape shows at any level), ~10 s
    if (++hist_div >= 3) {
        hist_div = 0;
        for (int p = 0; p < 2; p++) {
            int ch = ctl.set.swapped ? 1 - p : p;
            const et312_channel_t &c = ch ? ctl.foc.frame.b : ctl.foc.frame.a;
            hist[p][hist_pos] = (c.gate_on ? (float)c.intensity : 0.0f) * (float)ctl.foc.pad_gain[ch];
            float r = c.pulse_rate_hz < 10 ? 10 : (c.pulse_rate_hz > 400 ? 400 : (float)c.pulse_rate_hz);
            hist_rate[p][hist_pos] = logf(r / 10.0f) / logf(40.0f);
            float wd = c.pulse_width_us < FORK_WIDTH_MIN ? FORK_WIDTH_MIN
                     : (c.pulse_width_us > FORK_WIDTH_MAX ? FORK_WIDTH_MAX : (float)c.pulse_width_us);
            hist_width[p][hist_pos] = wd / (float)FORK_WIDTH_MAX;
        }
        hist_pos = (hist_pos + 1) % HIST_N;
    }
}

static void control_task(void *) {
    TickType_t wake = xTaskGetTickCount();
    uint32_t last = millis();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
        uint32_t ms = millis();
        uint32_t gap = ms - last;
        last = ms;
        if (gap > tick_gap_max) tick_gap_max = gap;
        if (ms - tick_window_ms >= 1000) {    // the longest gap between ticks over the last second, for the screen
            tick_gap_shown = tick_gap_max;
            tick_gap_max = 0;
            tick_window_ms = ms;
            Lock lock;
            rtt_max_shown = blink.rtt_max_ms;        // the worst round trip over the last second
            blink.rtt_max_ms = 0;
        }
        control_step();
    }
}

// ---- network task: Wi-Fi and (re)connecting -------------------------------------------------------------------
static void network_task(void *) {
    uint32_t wifi_try = 0, conn_try = 0;
    WiFiClient fresh;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!cfg.ok) continue;
        uint32_t ms = millis();
        if (!cfg.direct && WiFi.status() != WL_CONNECTED) {
            if (ms - wifi_try > 5000) { wifi_try = ms; WiFi.begin(cfg.ssid, cfg.pass); }
            Lock lock;
            if (link_live()) boxlink_disconnected(&blink, "Wi-Fi lost");
            continue;
        }
        bool live;
        char host[64];
        uint16_t port;
        Box box;
        {
            Lock lock;
            live = link_live();
            box = cfg.boxes[ctl.set.box];
        }
        strlcpy(host, box.host, sizeof host);
        port = box.port;
        if (cfg.direct && !find_box(box, host, sizeof host)) {
            Lock lock;
            if (link_live()) boxlink_disconnected(&blink, "the box left the remote's Wi-Fi");
            continue;
        }
        // not connected, or faulted (a trip latches the box): retry every 3 s until the power-cycled box answers
        if (live || (!reconnect_now && ms - conn_try < 3000)) continue;
        reconnect_now = false;
        conn_try = ms;
        fresh.stop();
        if (!fresh.connect(host, port, 1000)) continue;       // blocking, but not holding the lock
        fresh.setNoDelay(true);
        Lock lock;
        tcp.stop();
        tcp = fresh;
        txlen = 0;
        tx_stuck_ms = 0;
        boxlink_connected(&blink, millis());
    }
}

// ---- screen (loop, from the snapshot) -------------------------------------------------------------------------
// the FOC-Stim's electrode colours (PlaStim): 1 red, 2 blue, 3 yellow, 4 green; the number on them dark on yellow
static const uint16_t EL_COL[4] = {0xD145, 0x1B7C, 0xF600, 0x2CE7};
static const uint16_t EL_INK[4] = {TFT_WHITE, TFT_WHITE, TFT_BLACK, TFT_WHITE};
// PlaStim: the store's wordmark (plastim.net: "PlaStim", Montserrat bold, accent #3C64F4)
static const uint16_t C_BRAND = 0x3B3E;             // #3C64F4

// the PlaStim wordmark: a small biphasic pulse in the accent colour, then "Pla" + "Stim" (accent), centred on the
// line `mid`. scale 1 fits a title bar (Font2), 2 a 26 px banner, 3 the start-up splash. Returns the x after it.
static int brand(lgfx::LovyanGFX &g, int x, int mid, int scale) {
    const int s = scale, h = scale == 1 ? 5 : scale == 2 ? 7 : 15;
    const int px[] = {0, 3, 3, 7, 7, 11, 11, 14}, py[] = {0, 0, -1, -1, 1, 1, 0, 0};
    for (int k = 0; k + 1 < 8; k++)
        for (int t = 0; t < (scale > 1 ? 3 : 2); t++)
            g.drawLine(x + px[k] * s, mid + py[k] * h + t - 1, x + px[k + 1] * s, mid + py[k + 1] * h + t - 1, C_BRAND);
    x += 14 * s + 5 * s;
    if (scale >= 3) g.setFont(&fonts::FreeSansBold24pt7b);
    else if (scale == 2) g.setFont(&fonts::FreeSansBold12pt7b);
    else g.setFont(&fonts::Font2);
    g.setTextDatum(middle_left);
    g.setTextColor(TFT_WHITE);
    g.drawString("Pla", x, mid);
    x += g.textWidth("Pla");
    g.setTextColor(C_BRAND);
    g.drawString("Stim", x, mid);
    x += g.textWidth("Stim");
    g.setTextDatum(top_left);
    g.setFont(&fonts::Font2);
    return x;
}

static const uint16_t C_BG = TFT_BLACK, C_FG = TFT_WHITE, C_DIM = 0x8410, C_OFF = 0x31A6, C_A = 0xFD20, C_B = 0x07FF;

static void bar(int x, int y, int w, int h, float frac, uint16_t col, const char *label) {
    canvas.drawRect(x, y, w, h, C_DIM);
    canvas.fillRect(x + 1, y + 1, (int)((w - 2) * (frac < 0 ? 0 : frac > 1 ? 1 : frac)), h - 2, col);
    canvas.setTextColor(C_FG);
    canvas.drawString(label, x + 4, y + 2);
}

static const char *trip_short(const char *line, char *buf, size_t cap);

static void draw_run() {
    char s[96];
    bool armed = sc.saf.armed;
    canvas.fillRect(0, 0, 320, 26, armed ? 0x0400 : 0x8000);
    canvas.setTextColor(C_FG);
    canvas.setFont(&fonts::Font4);
    canvas.drawString(armed ? "RUNNING" : "STOPPED", 6, 2);
    canvas.setFont(&fonts::Font2);
    // the heartbeat: every frame from the box flashes the dot; green while they arrive, amber after 1.5 s of
    // silence, red after 4 s or when not connected (PlaStim: "so I can tell if it's connected or not")
    static uint32_t seen_frames = 0, flash_ms = 0;
    uint32_t now_ms = millis();
    if (sb.rx_frames != seen_frames) { seen_frames = sb.rx_frames; flash_ms = now_ms; }
    bool live = sb.state != BOXLINK_IDLE && sb.state != BOXLINK_FAULT && sb.rx_frames > 0;
    uint32_t age = now_ms - sb.last_rx_ms;
    uint16_t dot = !live ? TFT_RED : age < 1500 ? TFT_GREEN : age < 4000 ? TFT_ORANGE : TFT_RED;
    // no word when all is well (the green dot says it; PlaStim: it overran); a word only for what needs attention
    const char *word = sb.state == BOXLINK_RUNNING ? ""
                     : sb.state == BOXLINK_FAULT ? "fault"
                     : sb.state == BOXLINK_IDLE
                         ? (cfg.direct ? (n_joined ? "connecting" : "waiting for box")
                                       : WiFi.status() == WL_CONNECTED ? "connecting" : "no Wi-Fi")
                     : "handshake";
    const char *box = cfg.nboxes ? cfg.boxes[sc.set.box].name : "-";
    int rssi = cfg.direct ? box_rssi : (int)WiFi.RSSI();
    if (live && !word[0]) snprintf(s, sizeof s, "%s %.1fs %ddBm", box, age / 1000.0, rssi);
    else snprintf(s, sizeof s, "%s %s", box, word);
    canvas.drawRightString(s, 296, 5);
    canvas.fillCircle(307, 12, now_ms - flash_ms < 120 ? 7 : 5, dot);

    pack_entry_t e;
    const char *pname = (sc.set.pattern >= 0 && pack_ok && pack_entry(&pack, sc.set.pattern, &e) == PACK_OK) ? e.name : "(no pattern)";
    canvas.setTextColor(C_FG);
    canvas.drawString(pname, 6, 29);
    // pads: the ones marked off are dimmed (a channel routed onto one is silent)
    canvas.drawString("Pads", 234, 29);
    for (int i = 0; i < 4; i++) {
        char d[2] = {(char)('1' + i), 0};
        canvas.setTextColor(sc.set.pads[i] ? EL_COL[i] : C_OFF);
        canvas.drawString(d, 272 + i * 11, 29);
    }
    // MA (knob 2): upright on the right edge, so it never reads as another volume (PlaStim). Label and number on top.
    {
        const int mx = 298, my = 64, mw = 16, mh = 118;
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(0xB5B6);
        canvas.drawCentreString("MA", mx + mw / 2, 46);
        snprintf(s, sizeof s, "%d", sc.ma_steps);
        canvas.setTextColor(C_FG);
        canvas.drawCentreString(s, mx + mw / 2, 55);
        canvas.setFont(&fonts::Font2);
        canvas.drawRect(mx, my, mw, mh, C_DIM);
        int fh = (int)((mh - 2) * sc.ma_steps / 100.0f);
        canvas.fillRect(mx + 1, my + mh - 1 - fh, mw - 2, fh, 0x7BCF);
    }
    // the box's own knob is the real master; ours is a fraction of it: a dim track up to the box knob (the
    // ceiling), the applied level bright inside it, the yellow marker at the knob (PlaStim: "keep the master on the unit")
    const float box_vol = sb.state == BOXLINK_RUNNING ? sb.tele.device_volume : 0.0f;
    if (sb.state == BOXLINK_RUNNING)
        snprintf(s, sizeof s, "Master %d%% of box %.0f%%", ctrl_master_percent(&sc), box_vol * 100);
    else
        snprintf(s, sizeof s, "Master %d%% of box (not connected)", ctrl_master_percent(&sc));
    {
        const float bv = box_vol < 0 ? 0 : box_vol > 1 ? 1 : box_vol;
        const float applied = ctrl_master_percent(&sc) / 100.0f * bv;
        canvas.drawRect(6, 46, 286, 14, C_DIM);                    // left of the MA column
        canvas.fillRect(7, 47, (int)(284 * bv), 12, 0x2124);
        canvas.fillRect(7, 47, (int)(284 * applied), 12, 0x2D7F);
        if (sb.state == BOXLINK_RUNNING) canvas.fillRect(6 + (int)(284 * bv), 45, 3, 16, TFT_YELLOW);
        canvas.setTextColor(C_FG);
        canvas.drawString(s, 10, 46);
    }
    const boxlink_telemetry_t &t = sb.tele;
    const bool fresh = sb.state == BOXLINK_RUNNING && now_ms - t.currents_ms < 2000;
    for (int p = 0; p < 2; p++) {
        int r = sc.set.reversed[p] ? route_reverse(sc.set.wire_route[p]) : sc.set.wire_route[p];
        bool off = !sc.set.pads[r / 10 - 1] || !sc.set.pads[r % 10 - 1];     // silent: a pad on its wires is off
        uint16_t col = off ? C_OFF : (p ? C_B : C_A);
        if (off)
            snprintf(s, sizeof s, "Wires %d-%d  pad off", r / 10, r % 10);
        else
            snprintf(s, sizeof s, "Wires %d-%d  %d%%%s", r / 10, r % 10, ctrl_level_percent(&sc, p),
                     sc.level_target[p] != ctrl_level_percent(&sc, p) ? " ..." : "");
        bar(6 + p * 146, 63, 140, 14, ctrl_level_percent(&sc, p) / 100.0f, col, s);
        // the last ~10 s, newest at the right, all three from the pattern (not the current reaching the body):
        //   filled, channel colour  intensity 0..100 % (independent of the level; 0 = gated off or pad off)
        //   grey                    pulse rate, log 10 Hz (bottom) .. 63 Hz (middle) .. 400 Hz (top)
        //   magenta                 pulse width as sent, 0 .. 400 us (linear)
        int x0 = 6 + p * 146, y0 = 80, h = 102, w = 140;
        int top = y0 + 1, span = h - 3, base = y0 + h - 2;
        canvas.drawRect(x0, y0, w, h, C_DIM);
        for (int x = x0 + 2; x < x0 + w - 2; x += 3) canvas.drawPixel(x, top + span / 2, 0x39E7);   // the middle
        const uint16_t fill = (col >> 2) & 0x39E7;                                                  // quarter bright
        int prev_i = -1, prev_r = 0, prev_w = 0;
        for (int x = 0; x < w - 2; x++) {
            int i = (shist_pos + x * HIST_N / (w - 2)) % HIST_N;
            int yi = base - (int)(shist[p][i] * span), yr = base - (int)(shist_rate[p][i] * span);
            int yw = base - (int)(shist_width[p][i] * span);
            if (yi < base) canvas.drawFastVLine(x0 + 1 + x, yi, base - yi, fill);
            if (prev_i >= 0) {
                canvas.drawLine(x0 + x, prev_r, x0 + 1 + x, yr, 0x7BEF);
                canvas.drawLine(x0 + x, prev_w, x0 + 1 + x, yw, 0xF81F);
                canvas.drawLine(x0 + x, prev_i, x0 + 1 + x, yi, col);
            }
            prev_i = yi, prev_r = yr, prev_w = yw;
        }
        {   // the current rate and width, coloured like their lines (the legend)
            int ch = sc.set.swapped ? 1 - p : p;
            const et312_channel_t &c = ch ? sc.foc.frame.b : sc.foc.frame.a;
            double wd = c.pulse_width_us < FORK_WIDTH_MIN ? FORK_WIDTH_MIN
                      : (c.pulse_width_us > FORK_WIDTH_MAX ? FORK_WIDTH_MAX : c.pulse_width_us);
            canvas.setFont(&fonts::Font0);
            snprintf(s, sizeof s, "%.0f Hz", c.pulse_rate_hz);
            canvas.setTextColor(0xBDF7);
            canvas.drawString(s, x0 + 3, y0 + 3);
            snprintf(s, sizeof s, "%.0f us", wd);
            canvas.setTextColor(0xFBDF);
            canvas.drawString(s, x0 + 51, y0 + 3);
            canvas.setFont(&fonts::Font2);
        }
        // bottom row: the current each wire pair actually gets (the box's measured peak; on a shared electrode
        // the other one of the pair is this channel's alone, so the smaller of the two)
        float pk = t.peak[r / 10 - 1] < t.peak[r % 10 - 1] ? t.peak[r / 10 - 1] : t.peak[r % 10 - 1];
        if (off || !fresh) snprintf(s, sizeof s, "%d-%d  -- mA", r / 10, r % 10);
        else snprintf(s, sizeof s, "%d-%d %5.1f mA", r / 10, r % 10, pk * 1000);
        canvas.setTextColor(col);
        canvas.drawString(s, 6 + p * 100, 222);
    }
    snprintf(s, sizeof s, "bat %.0f%% | %d%%", t.battery_soc * 100, M5.Power.getBatteryLevel());   // box | remote
    canvas.setTextColor(C_FG);
    canvas.drawRightString(s, 314, 222);

    // diagnostics, small. Guard events and (fork v7) climb holds over the last 10 s: the box's counts are since it
    // booted, so the screen keeps a second-by-second ring. A guard event = a pulse over a ceiling (softened after);
    // a hold = v7 stopping the model's climb just under it (steady, expected where the sensed peak runs high).
    // Per channel A / B (v7): sensed / commanded lead peak and lead charge, and the loop-resistance estimate.
    // Then the control tick and the Wi-Fi round trip.
    static float ring[11][2];
    static uint32_t ring_sec = 0;
    static int ring_n = 0;
    if (sb.state != BOXLINK_RUNNING || (ring_n && t.guard < ring[(ring_n - 1) % 11][0])) ring_n = 0;
    if (sb.state == BOXLINK_RUNNING && (ring_n == 0 || now_ms - ring_sec >= 1000)) {
        ring[ring_n % 11][0] = t.guard;
        ring[ring_n % 11][1] = t.hold;
        ring_n++;
        ring_sec = now_ms;
    }
    const int oldest = (ring_n > 11 ? ring_n - 11 : 0) % 11;
    int guard_10s = ring_n ? (int)(t.guard - ring[oldest][0]) : 0;
    int hold_10s = ring_n ? (int)(t.hold - ring[oldest][1]) : 0;
    const bool v7 = sb.fork_version >= 7;
    canvas.setFont(&fonts::Font0);
    if (v7) snprintf(s, sizeof s, "guard +%d hold +%d /10s", guard_10s, hold_10s < 0 ? 0 : hold_10s);
    else snprintf(s, sizeof s, "guard +%d/10s", guard_10s);
    canvas.setTextColor(guard_10s > 0 ? TFT_ORANGE : C_DIM);
    canvas.drawString(s, 6, 185);
    canvas.setTextColor(C_DIM);
    if (v7) snprintf(s, sizeof s, "A/B pk %.2f/%.2f rho %.2f/%.2f", t.pk[0], t.pk[1], t.rho[0], t.rho[1]);
    else snprintf(s, sizeof s, "sigma %.2f %.2f", t.sigma[0], t.sigma[1]);
    canvas.drawRightString(s, 314, 185);
    if (v7) {
        snprintf(s, sizeof s, "A/B r %.1f/%.1f ohm", t.r_est[0], t.r_est[1]);
        canvas.drawString(s, 6, 195);
    }
    snprintf(s, sizeof s, "tick %u rtt %.0f/%u", (unsigned)stick_gap, sb.rtt_avg_ms, (unsigned)srtt_max);
    canvas.setTextColor(stick_gap > 25 || srtt_max > 60 ? TFT_ORANGE : C_DIM);
    canvas.drawRightString(s, 314, 195);
    canvas.setFont(&fonts::Font2);
    // a message (why a start was refused, a fault...) - not the pattern name again, it is at the top. While stopped
    // after a trip, the trip's measurement and pulse lines instead (until the next start)
    if (!sc.saf.armed && sb.ntrip >= 3) {
        char b[200];
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(TFT_ORANGE);
        canvas.drawString(trip_short(sb.trip[1], b, sizeof b), 6, 204);
        canvas.drawString(trip_short(sb.trip[2], b, sizeof b), 6, 213);
        canvas.setFont(&fonts::Font2);
    } else if (sc.status[0] && strcmp(sc.status, pname) != 0) {
        canvas.setTextColor(TFT_ORANGE);
        canvas.drawString(sc.status, 6, 204);
    }
}

static void draw_list(const char *title, int count, int cursor, bool patterns) {
    // the PlaStim banner, as tall as the run screen's RUNNING / STOPPED band (PlaStim); the output keeps running
    // while a pattern is picked, so the state stays on it, in its own colour
    canvas.fillRect(0, 0, 320, 26, 0x10A2);
    canvas.fillRect(0, 26, 320, 1, C_BRAND);
    brand(canvas, 8, 13, 2);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(sc.saf.armed ? TFT_GREEN : 0xF9E7);
    canvas.drawRightString(sc.saf.armed ? "RUNNING" : "STOPPED", 314, 5);
    // then the heading
    canvas.setTextColor(C_FG);
    canvas.drawString(title, 6, 30);
    canvas.setTextColor(C_DIM);
    canvas.drawRightString("1 scroll/pick  2 +10", 314, 30);
    canvas.drawFastHLine(0, 47, 320, 0x2124);
    int rows = 10, first = cursor - rows / 2;
    if (first > count - rows) first = count - rows;
    if (first < 0) first = 0;
    for (int i = first; i < count && i < first + rows; i++) {
        char s[64];
        int y = 51 + (i - first) * 18;
        if (patterns) {
            pack_entry_t e;
            if (pack_entry(&pack, i, &e) != PACK_OK) continue;
            static const char *const groups[] = {"built-in", "ErosLink", "example", "yours", "ours"};
            snprintf(s, sizeof s, "%s  [%s]", e.name, e.group >= 0 && e.group <= 4 ? groups[e.group] : "?");
        } else {
            ctrl_option_text(&sc, i, s, sizeof s);
        }
        if (i == cursor) canvas.fillRect(0, y - 1, 320, 18, 0x4208);
        canvas.setTextColor(i == sc.set.pattern && patterns ? C_A : C_FG);
        canvas.drawString(s, 8, y);
    }
}

// ---- options: the wiring as a picture ------------------------------------------------------------------------
// Four pads in a row; position 1's wires (orange, knob 3) as a bracket above them, position 2's (cyan, knob 4)
// below, with an arrow for the polarity and the pattern (A / B) each plays. The highlight is knob 1's cursor.
static const int PAD_X[4] = {52, 124, 196, 268}, PAD_Y = 88, PAD_R = 15;

static void wire_bracket(int route, bool above, uint16_t col, bool hi_wire, bool hi_pol) {
    int a = route / 10 - 1, b = route % 10 - 1;
    int xa = PAD_X[a], xb = PAD_X[b], y0 = above ? PAD_Y - PAD_R - 1 : PAD_Y + PAD_R + 1, ybar = above ? 50 : 126;
    uint16_t lc = hi_wire ? C_FG : col;
    for (int d = -1; d <= 1; d++) {
        canvas.drawLine(xa + d, y0, xa + d, ybar, lc);
        canvas.drawLine(xb + d, y0, xb + d, ybar, lc);
        canvas.drawLine(xa, ybar + d, xb, ybar + d, lc);
    }
    int mx = (xa + xb) / 2, dir = xb > xa ? 1 : -1;               // the arrow points from the first digit to the second
    canvas.fillTriangle(mx + 9 * dir, ybar, mx - 7 * dir, ybar - 8, mx - 7 * dir, ybar + 8, lc);
    if (hi_pol) {
        canvas.drawCircle(mx, ybar, 11, C_FG);
        canvas.drawCircle(mx, ybar, 12, C_FG);
    }
}

static void button(int x, int y, int w, const char *text, bool hi) {
    canvas.fillRect(x, y, w, 18, hi ? 0x4208 : 0x18E3);
    canvas.drawRect(x, y, w, 18, hi ? C_FG : C_DIM);
    canvas.setTextColor(C_FG);
    canvas.drawString(text, x + 6, y + 1);
}

static void draw_options() {
    char s[64];
    int cur = sc.cursor;
    canvas.setFont(&fonts::Font2);
    canvas.fillRect(0, 0, 320, 20, 0x2124);
    int tx = brand(canvas, 6, 10, 1);
    canvas.setTextColor(C_FG);
    canvas.drawString("Wiring & options", tx + 8, 3);
    canvas.drawRightString("4 press: back", 314, 3);
    for (int p = 0; p < 2; p++) {
        int base = sc.set.wire_route[p];
        int r = sc.set.reversed[p] ? route_reverse(base) : base;
        bool off = !sc.set.pads[r / 10 - 1] || !sc.set.pads[r % 10 - 1];
        uint16_t col = off ? C_OFF : (p ? C_B : C_A);
        bool plays_a = sc.set.swapped ? p == 1 : p == 0;
        wire_bracket(r, p == 0, col, cur == (p ? OPT_WIRES_2 : OPT_WIRES_1), cur == (p ? OPT_POLARITY_2 : OPT_POLARITY_1));
        snprintf(s, sizeof s, "knob %d: %d->%d%s  plays %c%s", 3 + p, r / 10, r % 10,
                 sc.set.reversed[p] ? " (reversed)" : "", plays_a ? 'A' : 'B', off ? "  pad off" : "");
        canvas.setTextColor(col);
        canvas.drawString(s, 6, p ? 138 : 24);
    }
    for (int i = 0; i < 4; i++) {
        bool on = sc.set.pads[i];
        // in its electrode colour; a pad marked off: dark, colour ring, crossed out
        canvas.fillCircle(PAD_X[i], PAD_Y, PAD_R, on ? EL_COL[i] : 0x1082);
        canvas.drawCircle(PAD_X[i], PAD_Y, PAD_R, on ? C_FG : EL_COL[i]);
        char d[2] = {(char)('1' + i), 0};
        canvas.setTextColor(on ? EL_INK[i] : EL_COL[i]);
        canvas.drawCentreString(d, PAD_X[i], PAD_Y - 7);
        if (!on) {
            canvas.drawLine(PAD_X[i] - 9, PAD_Y - 9, PAD_X[i] + 9, PAD_Y + 9, C_FG);
            canvas.drawLine(PAD_X[i] - 9, PAD_Y + 9, PAD_X[i] + 9, PAD_Y - 9, C_FG);
        }
        if (cur == OPT_PAD_1 + i) {
            canvas.drawCircle(PAD_X[i], PAD_Y, PAD_R + 3, C_FG);
            canvas.drawCircle(PAD_X[i], PAD_Y, PAD_R + 4, C_FG);
        }
    }
    snprintf(s, sizeof s, "Swap A/B: %s", sc.set.swapped ? "on" : "off");
    button(6, 154, 150, s, cur == OPT_SWAP);
    ctrl_option_text(&sc, OPT_SHAPE, s, sizeof s);
    if (strlen(s) > 22) s[22] = 0;                  // the button's width; the long note is for the list view
    button(164, 154, 150, s, cur == OPT_SHAPE);
    snprintf(s, sizeof s, "Skip mode ramp: %s", sc.set.skip_mode_ramp ? "on" : "off");
    button(6, 176, 150, s, cur == OPT_SKIP_RAMP);
    snprintf(s, sizeof s, "Box: %s", cfg.nboxes ? cfg.boxes[sc.set.box].name : "-");
    button(164, 176, 150, s, cur == OPT_BOX);
    button(6, 198, 150, "Back", cur == OPT_BACK);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DIM);
    canvas.drawString("1: move, press toggles   2: pulse shape", 6, 221);
    canvas.drawString("3 / 4: wires, every pair both ways", 6, 231);
    canvas.drawRightString("levels hold here", 314, 231);
    canvas.setFont(&fonts::Font2);
}

// a trip line without its prefixes and units, so more of it fits the small font
static const char *trip_short(const char *line, char *buf, size_t cap) {
    const char *p = line;
    if (!strncmp(p, "biphasic trip: ", 15)) p += 15;
    else if (!strncmp(p, "biphasic: ", 10)) p += 10;
    size_t n = 0;
    for (; *p && n + 1 < cap; p++) {
        if (!strncmp(p, " A primary", 10)) { p += 9; continue; }
        buf[n++] = *p;
    }
    buf[n] = 0;
    return buf;
}

// the fault, and the box's trip report wrapped to the screen (it stays through the reconnect attempts)
static void draw_fault() {
    canvas.fillRect(0, 150, 320, 90, 0x3000);
    canvas.setTextColor(C_FG);
    canvas.setFont(&fonts::Font0);
    canvas.drawString(sb.fault, 4, 152);
    int y = 163;
    if (!sb.ntrip) canvas.drawString("power-cycle the box if it tripped; it reconnects by itself", 4, y);
    canvas.setTextColor(TFT_ORANGE);
    for (int i = 0; i < sb.ntrip && y < 232; i++) {
        char b[200];
        const char *t = trip_short(sb.trip[i], b, sizeof b);
        for (size_t off = 0; off < strlen(t) && y < 232; off += 52, y += 10) {
            char part[53];
            strlcpy(part, t + off, sizeof part);
            canvas.drawString(part, 4, y);
        }
    }
}

static void draw() {
    canvas.fillScreen(C_BG);
    canvas.setFont(&fonts::Font2);
    if (!cfg.ok || !pack_ok) {
        canvas.setTextColor(C_FG);
        canvas.drawString("stim remote", 6, 6);
        canvas.drawString(!cfg.ok ? cfg.error : "no patterns.bin (load from the PC)", 6, 30);
        canvas.drawString("py -3.13 -m stimengine.remote load --port COMx", 6, 54);
    } else if (sc.screen == SCREEN_PATTERNS) {
        draw_list("Patterns", pack.count, sc.cursor, true);
    } else if (sc.screen == SCREEN_OPTIONS) {
        draw_options();
    } else {
        draw_run();
        if (sb.state == BOXLINK_FAULT) draw_fault();
    }
    canvas.pushSprite(0, 0);
}

// ---- main -----------------------------------------------------------------------------------------------------
void setup() {
    auto m5cfg = M5.config();
    M5.begin(m5cfg);
    // screen on and a message first thing: a black screen then only ever means no power (the CoreS3's power chip
    // can leave the ESP32 half-powered after a 6 s power-button hold; a reset press brings it back)
    M5.Display.setBrightness(160);
    M5.Display.fillScreen(TFT_BLACK);
    brand(M5.Display, 38, 100, 3);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(0x8410, TFT_BLACK);
    M5.Display.drawCentreString("remote - starting", 160, 150);
    Serial.setRxBufferSize(4096);             // the loader sends 1 KB chunks; the default 256 B would overflow
    Serial.begin(115200);
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    for (int i = 0; i < 4; i++) {
        enc[i].attachHalfQuad(ENC_PINS[i][0], ENC_PINS[i][1]);
        enc[i].setCount(0);
        enc_base[i] = 0;
    }
    pinMode(PIN_MX, INPUT);
    pinMode(PIN_PUSH1, INPUT);
    pinMode(PIN_PUSH4, INPUT);
    // A button still held at boot (the MX press that woke the remote from sleep) must not count as a press: it only
    // counts after it has been released and pressed again. Otherwise the wake press would be read as START.
    for (Button *b : {&btn_mx, &btn_p1, &btn_p4}) {
        b->state = digitalRead(b->pin) == HIGH;
        b->changed = millis();
    }
    canvas.setColorDepth(8);
    canvas.createSprite(320, 240);
    LittleFS.begin(true);
    mtx = xSemaphoreCreateMutex();
    load_config();
    load_pack();
    boxlink_io_t io = {nullptr, tcp_write};
    boxlink_init(&blink, &io, cfg.cap);
    loader_io_t lio = {nullptr, l_send, l_busy, l_free, l_open, l_write, l_commit, l_abort, l_remove, l_list, l_reload};
    loader_init(&ldr, &lio);
    start_controller();                       // levels 0, stopped
    wifi_start();
    // control above the loop (priority 1) on the same core, so drawing never delays it; network low, on core 0
    xTaskCreatePinnedToCore(control_task, "control", 8192, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(network_task, "network", 6144, nullptr, 1, nullptr, 0);
}

// The power button: a short press switches the remote off, tidily. Output first: STOP (if running), both channels to
// zero and the box's signal stopped, the settings saved. Then DEEP SLEEP, not the power chip's off: on the OSSM
// carrier (battery on the M-bus BAT pin) a CoreS3 SE whose AXP2101 is off cannot be switched on again by its power
// button, only by USB power (tested 2026-09-29, also after the chip's own 6 s hold). In deep sleep the screen, Wi-Fi
// and the audio/camera/SD rails are off (the 5 V boost stays on, see below) and the MX button or a knob press (RTC GPIOs, high = pressed)
// wakes it; waking is a normal boot (levels 0, stopped). The chip's 6 s hold remains the full off for storage.
static void power_off() {
    ctrl_settings_t s;
    {
        Lock lock;
        if (ctrl_armed(&ctl)) ctrl_button(&ctl, now_s());        // STOP always wins
        if (link_live()) {
            boxlink_stop(&blink, millis());                        // zero both channels + signal stop
            tcp_flush();
        }
        s = ctl.set;
    }
    save_settings(s);
    M5.Display.fillScreen(TFT_BLACK);
    brand(M5.Display, 38, 100, 3);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(0x8410, TFT_BLACK);
    M5.Display.drawCentreString("remote - switching off", 160, 150);
    M5.Display.drawCentreString("press the big button to switch on", 160, 172);
    delay(1500);                                                   // let the stop reach the box; time to read
    // a button still held would wake it at once: wait for all three to be released
    while (digitalRead(PIN_MX) == HIGH || digitalRead(PIN_PUSH1) == HIGH || digitalRead(PIN_PUSH4) == HIGH) delay(10);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    // NOT M5.Power.setExtOutput(false): on the carrier the battery (M-bus BAT) seems to reach the AXP2101 through the
    // SY7088 5 V boost, whose enable comes from the AW9523 on the 3.3 V rail. Switching the boost off (or the AXP off)
    // cuts the power chip's own supply, and only USB brings it back (2026-09-29: dark, no button woke it).
    M5.Power.Axp2101.writeRegister8(0x90, 0xB0);                  // ALDO1..4 off (audio, mic, camera, SD); boot restores
    const uint64_t wake = (1ULL << PIN_MX) | (1ULL << PIN_PUSH1) | (1ULL << PIN_PUSH4);
    for (gpio_num_t p : {(gpio_num_t)PIN_MX, (gpio_num_t)PIN_PUSH1, (gpio_num_t)PIN_PUSH4}) {
        rtc_gpio_pullup_dis(p);
        rtc_gpio_pulldown_en(p);                                   // released = low, whatever the carrier's resistors
    }
    esp_sleep_enable_ext1_wakeup(wake, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();
}

void loop() {
    M5.update();
    if (M5.BtnPWR.wasClicked()) power_off();
    uint8_t ub[512];
    for (int avail; (avail = Serial.available()) > 0;) {       // drain everything waiting
        int n = Serial.readBytes(ub, avail < (int)sizeof ub ? avail : (int)sizeof ub);
        if (n <= 0) break;
        Lock lock;
        loader_feed(&ldr, ub, (size_t)n, millis());
    }
    bool receiving;
    ctrl_settings_t to_save;
    bool save = false;
    static uint32_t settings_dirty_ms = 0, last_draw_ms = 0;
    static float shown_box_vol = -1;
    uint32_t ms = millis();
    {
        Lock lock;
        loader_poll(&ldr, ms);
        receiving = ldr.receiving;
        if (memcmp(&ctl.set, &saved_set, sizeof ctl.set) != 0) {
            if (!settings_dirty_ms) settings_dirty_ms = ms;
            if (ms - settings_dirty_ms > 2000) {
                to_save = ctl.set;
                saved_set = ctl.set;
                settings_dirty_ms = 0;
                save = true;
            }
        }
    }
    if (save) save_settings(to_save);           // file write outside the lock
    // ~15 Hz, and at once (>= 20 ms apart) when the box knob moves; never during a USB transfer
    bool knob_moved = blink.tele.device_volume != shown_box_vol && ms - last_draw_ms >= 20;
    if ((ms - last_draw_ms >= 66 || knob_moved) && !receiving) {
        last_draw_ms = ms;
        {
            Lock lock;                          // a snapshot in microseconds; drawing then runs without the lock
            sc = ctl;
            sb = blink;
            memcpy(shist, hist, sizeof shist);
            memcpy(shist_rate, hist_rate, sizeof shist_rate);
            memcpy(shist_width, hist_width, sizeof shist_width);
            shist_pos = hist_pos;
            stick_gap = tick_gap_shown;
            srtt_max = rtt_max_shown;
        }
        shown_box_vol = sb.tele.device_volume;
        draw();
        // a new trip report: out on the USB serial ("[" lines: the PC loader skips them) and into /last_trip.txt,
        // so it can be read later even after the remote restarts
        static uint32_t trip_seen = 0;
        if (sb.trip_seq != trip_seen && sb.ntrip > 0) {
            trip_seen = sb.trip_seq;
            File f = LittleFS.open("/last_trip.txt", "w");
            for (int i = 0; i < sb.ntrip; i++) {
                Serial.printf("[trip] %s\n", sb.trip[i]);
                if (f) f.printf("%s\n", sb.trip[i]);
            }
            if (f) f.close();
        }
    }
    vTaskDelay(1);
}
