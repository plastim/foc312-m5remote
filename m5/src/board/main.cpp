// The M5 remote's board: the OSSM M5 Remote by ortlof (CoreS3 SE, 4 encoders, 2 with push, 1 MX button), measured
// with the hwtest firmware. The app (../../app/app.cpp, shared with the RADR build) does the rest.
#include <Arduino.h>
#include <ESP32Encoder.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "app.h"

// ---- the board ----------------------------------------------------------------------------------------------------
static const int ENC_PINS[4][2] = {{5, 9}, {18, 17}, {1, 2}, {7, 6}};   // knobs 1..4 as printed on the remote
// what each knob does on each screen is core/ctrl.h's business (knob 1 master / scroll, 2-3 levels A / B, 4 MA)
static const int ENC_DIR[4] = {+1, +1, +1, +1};                          // clockwise = up (checked by PlaStim)
static const int PIN_MX = 10, PIN_PUSH1 = 8, PIN_PUSH4 = 14;             // high = pressed
static const uint32_t DEBOUNCE_MS = 30;

static ESP32Encoder enc[4];
static long enc_base[4];

const char *board_name() { return "m5"; }

void board_link_ids(uint32_t *first, uint32_t *last) {
    *first = BOXLINK_IDS_M5_FIRST;
    *last = BOXLINK_IDS_M5_LAST;
}

void board_begin() {
    auto m5cfg = M5.config();
    M5.begin(m5cfg);
    M5.Display.setBrightness(160);
}

lgfx::LovyanGFX &board_display() { return M5.Display; }

void board_serial_begin(size_t rx_queue) {
    Serial.setRxBufferSize(rx_queue);
    Serial.begin(115200);                      // (the ESP32-S3's own USB serial: the baud is not used)
}

// ---- inputs (control task) ------------------------------------------------------------------------------------
struct Button { int pin; bool state; uint32_t changed; };
static Button btn_mx = {PIN_MX, false, 0}, btn_p1 = {PIN_PUSH1, false, 0}, btn_p4 = {PIN_PUSH4, false, 0};

void board_inputs_begin() {
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
}

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

void board_inputs(ctrl_t *ctl, double t) {
    for (int i = 0; i < 4; i++) {
        long c = enc[i].getCount();
        long det = (c - enc_base[i]) / 2;     // 2 counts per detent; a bounce mid-detent adds or loses nothing
        if (det) {
            enc_base[i] += det * 2;
            ctrl_knob(ctl, (ctrl_knob_t)i, (int)det * ENC_DIR[i], t);
        }
    }
    if (pressed(btn_mx)) ctrl_button(ctl, t);
    if (pressed(btn_p1)) ctrl_push_knob1(ctl, t);
    if (pressed(btn_p4)) ctrl_push_knob4(ctl, t);
}

void board_controller_ready(ctrl_t *) {}       // (every option as always; the Remote check stays hidden)
bool board_may_drive() { return true; }
bool board_draw(AppCanvas &) { return false; }
void board_show(const ctrl_t &, const boxlink_t &) {}
int board_battery_percent() { return M5.Power.getBatteryLevel(); }
int board_focus() { return -1; }

const char *board_text(int which) {
    switch (which) {
    case BT_LIST: return "1 scroll/pick  2 +10";
    case BT_OPTIONS_TITLE: return "4 press: back";
    case BT_OPTIONS_1: return "1: move, press toggles   2: pulse shape";
    case BT_OPTIONS_2: return "3 / 4: wires, every pair both ways";
    case BT_PAIR_A: return "knob 2";
    case BT_PAIR_B: return "knob 3";
    default: return "";
    }
}

// ---- the power button ------------------------------------------------------------------------------------------
// A short press switches the remote off, tidily. Output first (app_shutdown: STOP if running, both channels to zero
// and the box's signal stopped, the settings saved). Then DEEP SLEEP, not the power chip's off: on the OSSM carrier
// (battery on the M-bus BAT pin) a CoreS3 SE whose AXP2101 is off cannot be switched on again by its power button,
// only by USB power (tested 2026-09-29, also after the chip's own 6 s hold). In deep sleep the screen, Wi-Fi and the
// audio/camera/SD rails are off (the 5 V boost stays on, see below) and the MX button or a knob press (RTC GPIOs, high =
// pressed) wakes it; waking is a normal boot (levels 0, stopped). The chip's 6 s hold remains the full off for storage.
static void power_off() {
    app_shutdown();
    M5.Display.fillScreen(TFT_BLACK);
    app_brand(M5.Display, 38, 100, 3);
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

void board_poll() {
    M5.update();
    if (M5.BtnPWR.wasClicked()) power_off();
}

void setup() { app_setup(); }
void loop() { app_loop(); }
