// The remote on the RADR hardware: its board layer. The app (../../app/app.cpp) is the M5 remote's, shared; this file
// brings the display, the knobs and buttons, the serial port, the LEDs, the buzzer, the vibration motor, the fuel gauge,
// and the first-start check (core/radr.h).
//
// The hardware is R+D's wireless remote ("RADR", v0.4; https://github.com/researchanddesire, CERN-OHL-S-2.0). This
// firmware is ours and written from scratch: the pins and parts below are facts read from their published schematic
// and pin list, nothing of their code is used (README "The RADR hardware"; SPEC.md for the licence reasoning).
//   ESP32-S3-WROOM-1-N16R8 (16 MB quad flash, 8 MB octal PSRAM); UART0 (GPIO 43 / 44) to a CP2102, DTR / RTS to EN
//   and IO0 through the usual two-transistor auto-reset (no BOOT button)
//   ST7789 2.0" 240 x 320 (used landscape): SCLK 5, MOSI 6, CS 4, DC 7, RST 16, backlight 15 (PWM, through a transistor)
//   encoders (ALPS EC12E-type, 24 detents / 24 pulses, no push, 10k pull-ups on the board): left 10 / 11, right 42 / 41
//   buttons to GND: left shoulder 48, right shoulder 1, under the screen left 38, centre 39, right 40
//   vibration motor 47 and a 4 kHz buzzer 2 (each through a transistor), 3 x WS2812B on 12
//   I2C SDA 8 / SCL 9: LSM6DS3 0x6A (unused), MAX17048 fuel gauge 0x36, 24C04 EEPROM 0x50 (v0.4: fitted; on boards
//   with it the left encoder's A / B are swapped; the check settles each knob's direction whatever the wiring)
#include <Arduino.h>
#include <ESP32Encoder.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <Wire.h>

#include "app.h"

extern "C" {
#include "radr.h"
}

// ---- pins ---------------------------------------------------------------------------------------------------------
static const int PIN_TFT_SCLK = 5, PIN_TFT_MOSI = 6, PIN_TFT_CS = 4, PIN_TFT_DC = 7, PIN_TFT_RST = 16, PIN_TFT_BL = 15;
static const int PIN_ENC[RADR_N_KNOBS][2] = {{10, 11}, {42, 41}};       // left, right (A, B)
static const int PIN_BTN[RADR_N_BTNS] = {48, 1, 38, 39, 40};            // radr.h RADR_BTN_* order; low = pressed
static const int PIN_VIBRATOR = 47, PIN_BUZZER = 2, PIN_LEDS = 12, N_LEDS = 3;
static const int PIN_SDA = 8, PIN_SCL = 9;
static const uint8_t I2C_FUEL_GAUGE = 0x36, I2C_EEPROM = 0x50;
static const uint32_t DEBOUNCE_MS = 30;
static const int BRIGHTNESS = 160;

// ---- the display --------------------------------------------------------------------------------------------------
class RadrDisplay : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 panel_;
    lgfx::Bus_SPI bus_;
    lgfx::Light_PWM light_;

public:
    RadrDisplay() {
        {
            auto c = bus_.config();
            c.spi_host = SPI2_HOST;
            c.spi_mode = 0;
            c.freq_write = 40000000;               // (the ST7789's limit)
            c.freq_read = 16000000;
            c.spi_3wire = true;                    // no MISO
            c.use_lock = true;
            c.dma_channel = SPI_DMA_CH_AUTO;
            c.pin_sclk = PIN_TFT_SCLK;
            c.pin_mosi = PIN_TFT_MOSI;
            c.pin_miso = -1;
            c.pin_dc = PIN_TFT_DC;
            bus_.config(c);
            panel_.setBus(&bus_);
        }
        {
            auto c = panel_.config();
            c.pin_cs = PIN_TFT_CS;
            c.pin_rst = PIN_TFT_RST;
            c.pin_busy = -1;
            c.panel_width = 240;
            c.panel_height = 320;
            c.offset_x = 0;
            c.offset_y = 0;
            c.offset_rotation = 0;
            c.readable = false;
            c.invert = true;                       // an IPS ST7789: inverted (the check's screen step can flip it)
            c.rgb_order = false;
            c.dlen_16bit = false;
            c.bus_shared = false;
            panel_.config(c);
        }
        {
            auto c = light_.config();
            c.pin_bl = PIN_TFT_BL;
            c.invert = false;
            c.freq = 5000;
            c.pwm_channel = 7;                     // (tone() takes LEDC channel 0)
            light_.config(c);
            panel_.setLight(&light_);
        }
        setPanel(&panel_);
    }
};

static RadrDisplay &lcd() {
    static RadrDisplay d;                          // (constructed on first use: the app's canvas points at it early)
    return d;
}

// ---- state --------------------------------------------------------------------------------------------------------
static Preferences prefs;
static radr_controls_t rc;
static radr_check_t chk;                           // the check: control task under the app's lock; the loop copies it
static portMUX_TYPE chk_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool checking = false;             // the check runs (first start, or Options > Remote check)
static volatile bool check_finished = false;       // control -> loop: save the result
static bool checked = false;                       // NVS: the owner has checked this unit
static int knob_dir[RADR_N_KNOBS] = {1, 1};        // what clockwise reads as (NVS; the check settles it)
static bool scr_flip = false, scr_invert = false;  // NVS: 180 degrees, colours inverted (relative to the panel's)
static bool eeprom_found = false;
static uint32_t device_seen = 0;                   // the controller's device_seq last seen (Options > Remote check)

static ESP32Encoder enc[RADR_N_KNOBS];
static long enc_base[RADR_N_KNOBS];
struct Button { int pin; bool state; uint32_t changed; bool live; };   // state: pressed; live: a press seen since boot
static Button btn[RADR_N_BTNS];

static volatile uint32_t vib_off_ms = 0;
static uint32_t focus_seen = 0;
static int battery_pct = 0;

const char *board_name() { return "radr"; }

void board_link_ids(uint32_t *first, uint32_t *last) {
    *first = BOXLINK_IDS_RADR_FIRST;
    *last = BOXLINK_IDS_RADR_LAST;
}

static void apply_screen(bool flip, bool invert) {
    lcd().setRotation(flip ? 1 : 3);               // landscape, the face buttons below the screen (the panel sits turned 180 on the RADR; flip = the other way up)
    lcd().invertDisplay(invert);
}

void board_begin() {
    prefs.begin("plastim", false);
    checked = prefs.getUChar("checked", 0) == 1;
    knob_dir[RADR_KNOB_L] = prefs.getChar("dir_l", 0);
    knob_dir[RADR_KNOB_R] = prefs.getChar("dir_r", 0);
    scr_flip = prefs.getBool("flip2", false);
    scr_invert = prefs.getBool("invert", false);
    lcd().init();
    apply_screen(scr_flip, scr_invert);
    lcd().setBrightness(BRIGHTNESS);
    // the controller's messages name this board's controls
    ctrl_set_control_names("the right button", "the left knob", "the right knob: A, B", "the centre button");
}

lgfx::LovyanGFX &board_display() { return lcd(); }

void board_serial_begin(size_t rx_queue) {
    Serial.setRxBufferSize(rx_queue);              // UART0 through the CP2102: the windowed loader keeps it drained
    Serial.begin(921600);                          // (stimengine.remote.loader SERIAL_BAUD)
}

// ---- the LEDs (3 x WS2812B: GRB, 800 kHz, through the RMT at 100 ns a tick) ---------------------------------------
static rmt_obj_t *leds_rmt = nullptr;
static uint32_t leds_shown = 0xFFFFFFFF;

static void leds_set(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t key = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
    if (!leds_rmt || key == leds_shown) return;
    leds_shown = key;
    static rmt_data_t bits[24 * N_LEDS];
    const uint8_t grb[3] = {g, r, b};
    int k = 0;
    for (int led = 0; led < N_LEDS; led++)
        for (int c = 0; c < 3; c++)
            for (int i = 7; i >= 0; i--, k++) {
                bool one = (grb[c] >> i) & 1;
                bits[k].level0 = 1;
                bits[k].duration0 = one ? 8 : 4;   // 0.8 / 0.4 us high
                bits[k].level1 = 0;
                bits[k].duration1 = one ? 4 : 8;
            }
    rmtWriteBlocking(leds_rmt, bits, 24 * N_LEDS);
}

static void buzz_tick(uint32_t ms) {                // the vibration motor, briefly (a focus change)
    digitalWrite(PIN_VIBRATOR, HIGH);
    vib_off_ms = millis() + ms;
}

// ---- inputs ---------------------------------------------------------------------------------------------------------
static bool i2c_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void board_inputs_begin() {
    pinMode(PIN_VIBRATOR, OUTPUT);
    digitalWrite(PIN_VIBRATOR, LOW);
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    eeprom_found = i2c_present(I2C_EEPROM);        // (an address probe: nothing is written to it)
    if (knob_dir[RADR_KNOB_L] == 0) knob_dir[RADR_KNOB_L] = eeprom_found ? -1 : 1;   // the v0.4 quirk: a first guess
    if (knob_dir[RADR_KNOB_R] == 0) knob_dir[RADR_KNOB_R] = 1;
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    for (int i = 0; i < RADR_N_KNOBS; i++) {
        enc[i].attachHalfQuad(PIN_ENC[i][0], PIN_ENC[i][1]);
        enc[i].setCount(0);
        enc_base[i] = 0;
    }
    // a button held at boot is not a press: it counts once released and pressed again
    for (int i = 0; i < RADR_N_BTNS; i++) {
        pinMode(PIN_BTN[i], INPUT_PULLUP);
        btn[i] = {PIN_BTN[i], digitalRead(PIN_BTN[i]) == LOW, millis(), false};
    }
    leds_rmt = rmtInit(PIN_LEDS, RMT_TX_MODE, RMT_MEM_64);
    if (leds_rmt) rmtSetTick(leds_rmt, 100);
    leds_set(0, 0, 0);
    radr_controls_init(&rc);
    if (!checked) {                                 // first start: the check before any box
        radr_check_begin(&chk, knob_dir, scr_flip, scr_invert);
        checking = true;
    }
}

// a debounced edge: +1 pressed, -1 released, 0 nothing
static int edge(Button &b) {
    bool v = digitalRead(b.pin) == LOW;
    uint32_t ms = millis();
    if (v != b.state && ms - b.changed > DEBOUNCE_MS) {
        b.state = v;
        b.changed = ms;
        return v ? 1 : -1;
    }
    return 0;
}

void board_inputs(ctrl_t *c, double t) {
    int raw[RADR_N_KNOBS];
    for (int i = 0; i < RADR_N_KNOBS; i++) {
        long n = enc[i].getCount();
        long det = (n - enc_base[i]) / 2;           // 2 counts per detent; a bounce mid-detent adds or loses nothing
        enc_base[i] += det * 2;
        raw[i] = (int)det;
    }
    int ev[RADR_N_BTNS];
    for (int i = 0; i < RADR_N_BTNS; i++) ev[i] = edge(btn[i]);

    if (!checking && c->device_seq != device_seen) {          // Options > Remote check (refused while output flows)
        device_seen = c->device_seq;
        if (!ctrl_armed(c)) {
            portENTER_CRITICAL(&chk_mux);
            radr_check_begin(&chk, knob_dir, scr_flip, scr_invert);
            portEXIT_CRITICAL(&chk_mux);
            checking = true;
        }
    }
    if (checking) {                                 // the check takes every input; nothing reaches the controller
        if (ctrl_armed(c)) ctrl_button(c, t);       // (never armed here: it starts stopped; STOP if it were)
        int step = chk.step;
        portENTER_CRITICAL(&chk_mux);
        for (int i = 0; i < RADR_N_KNOBS; i++) if (raw[i]) radr_check_knob(&chk, i, raw[i]);
        for (int i = 0; i < RADR_N_BTNS; i++) if (ev[i]) radr_check_button(&chk, i, ev[i] > 0);
        bool done = radr_check_done(&chk);
        int now_step = chk.step;
        portEXIT_CRITICAL(&chk_mux);
        if (now_step != step) buzz_tick(60);        // a step passed
        if (done && !check_finished) {
            check_finished = true;                  // the loop saves it, then lets the remote drive a box
            for (int i = 0; i < RADR_N_BTNS; i++) btn[i].live = false;   // the press that ended it is not the app's
            snprintf(c->status, sizeof c->status, "remote check passed: knobs, buttons, screen");
        }
        return;
    }
    if (check_finished) return;                     // (saved by the loop in a moment)
    for (int i = 0; i < RADR_N_KNOBS; i++)
        if (raw[i]) radr_knob(&rc, c, i, raw[i] * knob_dir[i], t);
    for (int i = 0; i < RADR_N_BTNS; i++) {
        if (ev[i] > 0) {
            btn[i].live = true;
            radr_button(&rc, c, i, true, t);        // every button acts on the press (the centre: START / STOP)
        } else if (ev[i] < 0 && btn[i].live) {      // a release counts only after a press seen (not one held at boot)
            radr_button(&rc, c, i, false, t);
        }
    }
    if (rc.focus_seq != focus_seen) {
        focus_seen = rc.focus_seq;
        buzz_tick(25);                              // the focus moved: a short tick
    }
}

void board_controller_ready(ctrl_t *c) {
    ctrl_hide_options(c, CTRL_HIDE_DEFAULT & ~CTRL_OPT_BIT(OPT_DEVICE));   // every option, and the remote check
    device_seen = c->device_seq;
}

bool board_may_drive() { return checked && !checking; }

int board_focus() { return checking ? -1 : rc.focus; }

int board_battery_percent() { return battery_pct; }

// the controls' names on the screens (btn = the buttons under the screen; top-L / top-R = the shoulders)
const char *board_text(int which) {
    switch (which) {
    case BT_LIST: return "pick: top-L / R btn  back: top-R / L btn";
    case BT_OPTIONS_TITLE: return "L btn: back";
    case BT_OPTIONS_1: return "L knob: move  top-L/R btn: select  top-R: next pair";
    case BT_OPTIONS_2: return "R knob: pair or shape (the focus)";
    case BT_PAIR_A: return "R knob (A)";
    case BT_PAIR_B: return "R knob (B)";
    default: return "";
    }
}

// ---- the loop's side -----------------------------------------------------------------------------------------------
static int read_battery() {                         // the MAX17048's SOC register: % in the high byte, 1/256 in the low
    Wire.beginTransmission(I2C_FUEL_GAUGE);
    Wire.write(0x04);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)I2C_FUEL_GAUGE, 2) != 2) return -1;
    int hi = Wire.read(), lo = Wire.read();
    int pct = hi + (lo >= 128 ? 1 : 0);
    return pct > 100 ? 100 : pct;
}

void board_poll() {
    if (vib_off_ms && (int32_t)(millis() - vib_off_ms) >= 0) {
        digitalWrite(PIN_VIBRATOR, LOW);
        vib_off_ms = 0;
    }
    if (check_finished) {                           // the check passed: keep it (NVS), then the remote may drive a box
        radr_check_t k;
        portENTER_CRITICAL(&chk_mux);
        k = chk;
        portEXIT_CRITICAL(&chk_mux);
        knob_dir[RADR_KNOB_L] = k.dir[RADR_KNOB_L];
        knob_dir[RADR_KNOB_R] = k.dir[RADR_KNOB_R];
        scr_flip = k.flip;
        scr_invert = k.invert;
        prefs.putChar("dir_l", (int8_t)knob_dir[RADR_KNOB_L]);
        prefs.putChar("dir_r", (int8_t)knob_dir[RADR_KNOB_R]);
        prefs.putBool("flip2", scr_flip);
        prefs.putBool("invert", scr_invert);
        prefs.putUChar("checked", 1);
        checked = true;
        apply_screen(scr_flip, scr_invert);
        checking = false;
        check_finished = false;
    }
    static uint32_t bat_ms = 0;
    if (!bat_ms || millis() - bat_ms > 5000) {
        bat_ms = millis();
        int p = read_battery();
        if (p >= 0) battery_pct = p;
    }
}

// lights and sounds after each snapshot: the LEDs say ARMED (red: output may flow), connected and stopped (dim green),
// the check (dim blue), a fault (amber), nothing (off); a beep when output starts, a lower one when it stops
void board_show(const ctrl_t &c, const boxlink_t &l) {
    static bool was_armed = false;
    bool armed = c.saf.armed;
    if (checking) leds_set(0, 0, 10);
    else if (armed) leds_set(60, 0, 0);
    else if (l.state == BOXLINK_FAULT) leds_set(30, 12, 0);
    else if (l.state == BOXLINK_RUNNING) leds_set(0, 8, 0);
    else leds_set(0, 0, 0);
    if (armed != was_armed) tone(PIN_BUZZER, armed ? 4000 : 2000, armed ? 40 : 80);
    was_armed = armed;
}

// ---- the check's screens ---------------------------------------------------------------------------------------------
static const uint16_t C_DIM = 0x8410, C_OK = TFT_GREEN, C_TODO = 0xB5B6;

static void check_header(AppCanvas &cv, const char *title, int step) {
    cv.fillRect(0, 0, 320, 24, 0x2124);
    app_brand(cv, 6, 12, 1);
    cv.setFont(&fonts::Font2);
    cv.setTextColor(TFT_WHITE);
    cv.drawString(title, 100, 4);
    char s[24];
    snprintf(s, sizeof s, "%d / 3", step);
    cv.setTextColor(C_DIM);
    cv.drawRightString(s, 314, 4);
}

static void draw_knob_panel(AppCanvas &cv, const radr_check_t &k, int knob, int x) {
    char s[48];
    bool ok = radr_check_knob_ok(&k, knob);
    cv.drawRect(x, 70, 150, 110, ok ? C_OK : C_DIM);
    cv.setFont(&fonts::Font2);
    cv.setTextColor(ok ? C_OK : TFT_WHITE);
    cv.drawCentreString(knob == RADR_KNOB_L ? "LEFT knob" : "RIGHT knob", x + 75, 74);
    if (!k.settled[knob]) {
        cv.setTextColor(TFT_ORANGE);
        cv.drawCentreString("turn it clockwise", x + 75, 104);
        cv.setTextColor(C_DIM);
        snprintf(s, sizeof s, "%d of %d", k.pending[knob] < 0 ? -k.pending[knob] : k.pending[knob], RADR_CHECK_SETTLE);
        cv.drawCentreString(s, x + 75, 124);
        return;
    }
    snprintf(s, sizeof s, "clockwise %d / %d", k.cw[knob] > 99 ? 99 : k.cw[knob], RADR_CHECK_TURNS);
    cv.setTextColor(k.cw[knob] >= RADR_CHECK_TURNS ? C_OK : C_TODO);
    cv.drawCentreString(s, x + 75, 98);
    snprintf(s, sizeof s, "back %d / %d", k.ccw[knob] > 99 ? 99 : k.ccw[knob], RADR_CHECK_TURNS);
    cv.setTextColor(k.ccw[knob] >= RADR_CHECK_TURNS ? C_OK : C_TODO);
    cv.drawCentreString(s, x + 75, 116);
    cv.setTextColor(TFT_WHITE);
    cv.drawCentreString(k.last[knob] > 0 ? "last: clockwise >" : k.last[knob] < 0 ? "< last: back" : "", x + 75, 138);
    cv.setFont(&fonts::Font0);
    cv.setTextColor(C_DIM);
    cv.drawCentreString(k.dir[knob] > 0 ? "(reads as wired)" : "(reads reversed: settled)", x + 75, 160);
    cv.setFont(&fonts::Font2);
}

static void draw_buttons(AppCanvas &cv, const radr_check_t &k) {
    // where they are: the shoulders on top, the three under the screen at the bottom
    static const int X[RADR_N_BTNS] = {8, 202, 8, 110, 212}, Y[RADR_N_BTNS] = {60, 60, 170, 170, 170};
    static const int W[RADR_N_BTNS] = {110, 110, 98, 98, 100};
    for (int i = 0; i < RADR_N_BTNS; i++) {
        bool done = k.pressed[i], held = k.down[i];
        cv.fillRect(X[i], Y[i], W[i], 34, held ? 0x4208 : done ? 0x0320 : 0x18E3);
        cv.drawRect(X[i], Y[i], W[i], 34, done ? C_OK : C_DIM);
        cv.setTextColor(done ? C_OK : TFT_WHITE);
        cv.drawCentreString(radr_button_name(i), X[i] + W[i] / 2, Y[i] + 9);
    }
}

static void draw_screen_test(AppCanvas &cv, const radr_check_t &k) {
    (void)k;
    static const uint16_t COL[4] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
    static const char *const NAME[4] = {"RED", "GREEN", "BLUE", "WHITE"};
    for (int i = 0; i < 4; i++) {
        cv.fillRect(8 + i * 77, 64, 73, 56, COL[i]);
        cv.setTextColor(TFT_WHITE);
        cv.drawCentreString(NAME[i], 8 + i * 77 + 36, 124);
    }
    cv.fillTriangle(160, 28, 150, 40, 170, 40, TFT_YELLOW);       // this edge is the top
    cv.setTextColor(TFT_YELLOW);
    cv.drawString("top", 176, 28);
}

bool board_draw(AppCanvas &cv) {
    if (!checking) return false;
    radr_check_t k;
    portENTER_CRITICAL(&chk_mux);
    k = chk;
    portEXIT_CRITICAL(&chk_mux);
    static bool shown_flip = false, shown_invert = false, shown_init = false;
    if (!shown_init || k.flip != shown_flip || k.invert != shown_invert) {   // the screen step's choices, live
        apply_screen(k.flip, k.invert);
        shown_flip = k.flip;
        shown_invert = k.invert;
        shown_init = true;
    }
    cv.fillScreen(TFT_BLACK);
    cv.setFont(&fonts::Font2);
    if (k.step == RADR_CHECK_KNOBS) {
        check_header(cv, "Remote check: knobs", 1);
        cv.setTextColor(TFT_WHITE);
        cv.drawString("Turn each knob CLOCKWISE first (3 clicks),", 8, 30);
        cv.drawString("then back the other way (3 clicks).", 8, 48);
        draw_knob_panel(cv, k, RADR_KNOB_L, 6);
        draw_knob_panel(cv, k, RADR_KNOB_R, 164);
        cv.setTextColor(C_DIM);
        cv.drawString("Turned one the wrong way first?", 8, 192);
        cv.drawString("Under-left button: start the knobs again.", 8, 210);
    } else if (k.step == RADR_CHECK_BUTTONS) {
        check_header(cv, "Remote check: buttons", 2);
        cv.setTextColor(TFT_WHITE);
        cv.drawString("Press each button once.", 8, 30);
        draw_buttons(cv, k);
        cv.setTextColor(C_DIM);
        cv.drawCentreString("(shoulders on top, three under the screen)", 160, 120);
    } else {
        check_header(cv, "Remote check: screen", 3);
        draw_screen_test(cv, k);
        cv.setTextColor(TFT_WHITE);
        cv.drawString("Each colour as its name, this text upright?", 8, 146);
        cv.setTextColor(C_TODO);
        cv.drawString("centre: yes, done", 8, 168);
        cv.drawString("under-left: turn the screen 180 degrees", 8, 186);
        cv.drawString("under-right: invert the colours", 8, 204);
    }
    cv.setFont(&fonts::Font0);
    cv.setTextColor(C_DIM);
    cv.drawString("No box is driven until the check is done.", 8, 230);
    cv.setFont(&fonts::Font2);
    return true;
}

void setup() { app_setup(); }
void loop() { app_loop(); }
