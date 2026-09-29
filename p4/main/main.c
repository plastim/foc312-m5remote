// The foc312 remote on the Waveshare ESP32-P4 5" touch board. STEP 1: the main screen on demo data (no box, no Wi-Fi,
// no output of any kind). The controls change only what is shown; the BOOT button (GPIO35) acts as STOP.
#include <math.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_lv_adapter.h"
#include "ui.h"

#define PIN_BOOT GPIO_NUM_35          // the P4's BOOT strapping button: low = pressed (to be confirmed on the board)

static const char *TAG = "p4remote";

// NOT bsp_display_lock(): the Waveshare BSP returns esp_lv_adapter_lock()'s esp_err_t as a bool, so it is true when
// the lock FAILED and false when it was taken (2026-09-29: a held-forever lock, then an LVGL assert while rendering).
// Touch comes in the panel's native portrait space (720 x 1280); the screen is turned to landscape by the adapter
// below LVGL, so LVGL doesn't rotate touches. Measured 2026-09-29: logo top-left = raw (669, 82), bottom-right =
// raw (33, 1221) -> screen x = raw y, screen y = 719 - raw x.
static lv_indev_read_cb_t touch_read_orig;
static void touch_read_rotated(lv_indev_t *in, lv_indev_data_t *d) {
    touch_read_orig(in, d);
    int32_t rx = d->point.x, ry = d->point.y;
    d->point.x = ry < 0 ? 0 : ry > 1279 ? 1279 : ry;
    d->point.y = 719 - (rx < 0 ? 0 : rx > 719 ? 719 : rx);
}

static bool ui_lock(int32_t ms) { return esp_lv_adapter_lock(ms) == ESP_OK; }
static void ui_unlock(void) { esp_lv_adapter_unlock(); }
static ui_state_t st;

static const char *DEMO_PATTERNS[] = {"Waves", "Stroke", "Climb", "Climb (slow finish)", "Climb (peak hold)", "Combo"};
#define N_DEMO (int)(sizeof DEMO_PATTERNS / sizeof DEMO_PATTERNS[0])
static int pat = 4;

static void demo_state(void) {
    st = (ui_state_t){0};
    st.level[0] = st.level_target[0] = 38; st.level[1] = st.level_target[1] = 30;
    st.master = st.master_target = 62; st.ma = 45; st.box_knob = 70;
    st.route[0] = 12; st.route[1] = 34; st.shape = 1; st.taper_pct = 40;
    st.pattern = DEMO_PATTERNS[pat]; st.pattern_group = "Built-in modes";
    st.pattern_index = pat + 1; st.pattern_count = N_DEMO;
    st.box_name = "box 2"; st.fw = "v8"; st.link_ms = 33; st.box_batt = 82;
    st.ap_ssid = "stim-remote"; st.ap_pass = "(demo)"; st.ap_channel = 6; st.box_joined = true;
}

// levels and master rise at most 10 steps a second (the core's CTRL_RISE_STEPS_PER_S); down is instant
#define RISE_PER_S 10.0f
static float rise_acc[3];
static void rise(int *now, int target, float *acc, float dt) {
    if (target <= *now) { *now = target; *acc = 0; return; }
    *acc += RISE_PER_S * dt;
    int step = (int)*acc;
    if (step > 0) { *acc -= step; *now = *now + step > target ? target : *now + step; }
}

static void demo_tick(float t) {
    rise(&st.level[0], st.level_target[0], &rise_acc[0], 0.042f);
    rise(&st.level[1], st.level_target[1], &rise_acc[1], 0.042f);
    rise(&st.master, st.master_target, &rise_acc[2], 0.042f);                   // a climb of ~6 s, held 1.5 s at the top, then again
    float ph = fmodf(t, 7.5f), c = (ph < 6 ? ph : 6) / 6.0f;
    float hz = ph < 6 ? 15.0f * powf(355.0f / 15.0f, c * c) : 355.0f;
    for (int ch = 0; ch < 2; ch++) {
        float inten = st.running ? 0.62f : 0.0f;
        st.hist_int[ch][st.hist_head] = inten;
        st.hist_rate[ch][st.hist_head] = hz;
        st.hist_width[ch][st.hist_head] = 130;
        st.int_now[ch] = inten; st.rate_now[ch] = hz; st.width_now[ch] = 130;
        st.ma_measured[ch] = st.running ? st.level[ch] * st.master * st.box_knob / 10000 * 120 / 100 : 0;
    }
    st.hist_head = (st.hist_head + 1) % UI_HIST;
}

static void handle(const ui_event_t *e) {
    switch (e->kind) {
    case UI_EV_STOP: st.running = false; break;
    case UI_EV_START: st.running = true; break;
    case UI_EV_LEVEL: st.level_target[e->ch] = e->value; if (e->value < st.level[e->ch]) st.level[e->ch] = e->value; break;
    case UI_EV_MASTER: st.master_target = e->value; if (e->value < st.master) st.master = e->value; break;
    case UI_EV_ROUTE: st.route[e->ch] = e->value; break;
    case UI_EV_MA: st.ma = e->value; break;
    case UI_EV_FLIP: st.route[e->ch] = (st.route[e->ch] % 10) * 10 + st.route[e->ch] / 10; break;
    case UI_EV_SAME_PADS: st.route[1] = st.route[0]; break;
    case UI_EV_SHAPE: st.shape = e->value; break;
    case UI_EV_PATTERN_PREV: pat = (pat + N_DEMO - 1) % N_DEMO; break;
    case UI_EV_PATTERN_NEXT: pat = (pat + 1) % N_DEMO; break;
    default: break;
    }
    st.pattern = DEMO_PATTERNS[pat];
    st.pattern_index = pat + 1;
}

void app_main(void) {
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    demo_state();

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,            // the 720 x 1280 panel, turned to landscape
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    ui_lock(-1);
    lv_indev_t *touch = bsp_display_get_input_dev();
    if (touch) {
        touch_read_orig = lv_indev_get_read_cb(touch);
        lv_indev_set_read_cb(touch, touch_read_rotated);
    }
    ui_create(&st);
    ui_note("demo screen - no box, no output");
    ui_debug_layout();
    ui_unlock();
    ESP_LOGI(TAG, "reset reason %d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "screen up");

    int n = 0, boot_prev = 1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(42));                  // 240 graph points = about 10 s
        demo_tick(n * 0.042f);
        int boot = gpio_get_level(PIN_BOOT);
        if (boot != boot_prev) ESP_LOGI(TAG, "GPIO35 = %d", boot);
        bool note_boot = false;
        if (!boot && boot_prev) {                       // BOOT pressed: STOP, always
            st.running = false;
            note_boot = true;
            ESP_LOGI(TAG, "BOOT pressed -> STOP");
        }
        boot_prev = boot;
        ui_event_t e;
        while (ui_poll_event(&e)) handle(&e);
        if (ui_lock(100)) {
            lv_indev_t *in = bsp_display_get_input_dev();
            if (in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) {
                lv_point_t pt;
                lv_indev_get_point(in, &pt);
                ESP_LOGI(TAG, "touch %d,%d", (int)pt.x, (int)pt.y);
            }
            if (note_boot) ui_note("BOOT button pressed -> STOP (GPIO35 confirmed)");
            ui_refresh(&st);
            ui_unlock();
        }
        n++;
    }
}
