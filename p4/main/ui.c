// The main screen (see ui.h). LVGL 9, landscape 1280 x 720 on the 5" board. ASCII only (the built-in fonts).
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#define C_BG     0x0b0d12
#define C_PANEL  0x161a22
#define C_PANEL2 0x1d222c
#define C_LINE   0x2a3140
#define C_TXT    0xe8ecf3
#define C_MUT    0x8b93a3
#define C_BRAND  0x3b6cf0
#define C_A      0xff7a45
#define C_B      0x38bdf8
#define C_STOP   0xd92d2d
#define C_GO     0x1f9d55
#define C_RATE   0x9aa1ae
#define C_WIDTH  0xff3fd8
static const uint32_t EL_COL[4] = {0xd0282a, 0x1a6ee0, 0xf2c200, 0x2a9c3a};   // pads 1..4 as on the box
static const uint32_t EL_INK[4] = {0xffffff, 0xffffff, 0x000000, 0xffffff};

static QueueHandle_t evq;
static const ui_state_t *S;

static lv_obj_t *l_state, *l_box, *l_link, *l_fw, *l_batt, *l_pat, *l_patsub, *l_note;
static lv_obj_t *sl_level[2], *l_level[2], *l_meas[2], *l_levsub[2];
static lv_obj_t *sl_master, *l_master, *l_mastersub, *sl_ma, *l_ma;
static lv_obj_t *wiring, *graph[2], *l_legend[2], *bm_shape, *l_wifi, *l_joined, *btn_stop, *l_stop;
static bool show_pass;
static lv_obj_t *btn_ch[2], *l_ch[2], *l_hint, *settings, *l_set_box;
static int edit_ch = -1, first_pad = -1;
static lv_obj_t *col_left, *col_mid, *col_ma, *top_bar;

static void post(ui_ev_kind_t k, int ch, int v) {
    ui_event_t e = {k, ch, v};
    if (evq) xQueueSend(evq, &e, 0);
}
bool ui_poll_event(ui_event_t *ev) { return evq && xQueueReceive(evq, ev, 0) == pdTRUE; }

// ---- small builders ----------------------------------------------------------------------------------------------
static lv_obj_t *box(lv_obj_t *parent, uint32_t bg, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
    return o;
}
static lv_obj_t *card(lv_obj_t *parent) { return box(parent, C_PANEL, 14); }

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}
static lv_obj_t *button(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t bg, uint32_t fg) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = label(b, text, font, fg);
    lv_obj_center(l);
    return b;
}
static lv_obj_t *chip(lv_obj_t *parent) {
    lv_obj_t *c = box(parent, C_PANEL2, 999);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, 12, 0);
    lv_obj_set_style_pad_ver(c, 5, 0);
    lv_obj_t *l = label(c, "", &lv_font_montserrat_16, C_MUT);
    lv_label_set_recolor(l, true);
    return l;
}
static void style_slider(lv_obj_t *s, uint32_t from, uint32_t to, bool vertical) {
    lv_obj_set_style_bg_color(s, lv_color_hex(C_PANEL2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_border_width(s, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(from), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s, lv_color_hex(to), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s, vertical ? LV_GRAD_DIR_VER : LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);
}

// ---- events -------------------------------------------------------------------------------------------------------
static void on_slider(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (s == sl_level[0]) post(UI_EV_LEVEL, 0, v);
    else if (s == sl_level[1]) post(UI_EV_LEVEL, 1, v);
    else if (s == sl_master) post(UI_EV_MASTER, 0, v);
    else if (s == sl_ma) post(UI_EV_MA, 0, v);
}
static void on_step(lv_event_t *e) {            // user data: slider, sign in the low bit of the event param
    lv_obj_t *s = lv_event_get_user_data(e);
    int step = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    lv_slider_set_value(s, lv_slider_get_value(s) + step, LV_ANIM_OFF);
    lv_obj_send_event(s, LV_EVENT_VALUE_CHANGED, NULL);
}
static void on_click(lv_event_t *e) {
    intptr_t what = (intptr_t)lv_event_get_user_data(e);
    switch (what) {
    case 1: post(S && S->running ? UI_EV_STOP : UI_EV_START, 0, 0); break;
    case 2: post(UI_EV_FLIP, 0, 0); break;
    case 3: post(UI_EV_FLIP, 1, 0); break;
    case 4: post(UI_EV_SAME_PADS, 0, 0); break;
    case 5: post(UI_EV_PATTERN_PREV, 0, 0); break;
    case 6: post(UI_EV_PATTERN_NEXT, 0, 0); break;
    case 7: post(UI_EV_PATTERN_LIST, 0, 0); break;
    case 8: lv_obj_remove_flag(settings, LV_OBJ_FLAG_HIDDEN); post(UI_EV_SETTINGS, 0, 0); break;
    case 10: lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); break;
    case 9: show_pass = !show_pass; break;
    }
}
static void on_shape(lv_event_t *e) {
    post(UI_EV_SHAPE, 0, (int)lv_buttonmatrix_get_selected_button(lv_event_get_target(e)));
}

// a slider: name + sub on a line above; then -, slider, +, value
static lv_obj_t *slider_row(lv_obj_t *parent, const char *name, uint32_t from, uint32_t to, lv_obj_t **sl,
                            lv_obj_t **val, lv_obj_t **sub) {
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wrap, 2, 0);
    lv_obj_t *head = lv_obj_create(wrap);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(head, 10, 0);
    label(head, name, &lv_font_montserrat_20, C_TXT);
    *sub = label(head, "", &lv_font_montserrat_14, C_MUT);
    lv_label_set_recolor(*sub, true);
    lv_obj_t *row = lv_obj_create(wrap);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 54);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *minus = button(row, LV_SYMBOL_MINUS, &lv_font_montserrat_24, C_PANEL2, C_TXT);
    lv_obj_set_size(minus, 52, 50);
    *sl = lv_slider_create(row);
    lv_slider_set_range(*sl, 0, 100);
    lv_obj_set_height(*sl, 28);
    lv_obj_set_flex_grow(*sl, 1);
    style_slider(*sl, from, to, false);
    lv_obj_add_event_cb(*sl, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *plus = button(row, LV_SYMBOL_PLUS, &lv_font_montserrat_24, C_PANEL2, C_TXT);
    lv_obj_set_size(plus, 52, 50);
    lv_obj_set_user_data(minus, (void *)(intptr_t)-1);
    lv_obj_set_user_data(plus, (void *)(intptr_t)1);
    lv_obj_add_event_cb(minus, on_step, LV_EVENT_CLICKED, *sl);
    lv_obj_add_event_cb(plus, on_step, LV_EVENT_CLICKED, *sl);
    *val = label(row, "0", &lv_font_montserrat_28, C_TXT);
    lv_obj_set_width(*val, 76);
    lv_obj_set_style_text_align(*val, LV_TEXT_ALIGN_RIGHT, 0);
    return wrap;
}

// wiring, as on the PC player: pick a channel, tap the leading pad, then the other end
static const char *HINT_IDLE = "Pick A or B, then tap the leading pad, then the other end.";
static void wiring_pads(const lv_area_t *a, int px[4], int *py) {
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    *py = a->y1 + h - 34;
    for (int i = 0; i < 4; i++) px[i] = a->x1 + (int)((0.14f + 0.24f * i) * w);
}
static void on_channel(lv_event_t *e) {
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    edit_ch = edit_ch == ch ? -1 : ch;
    first_pad = -1;
    if (edit_ch < 0) lv_label_set_text(l_hint, HINT_IDLE);
    else lv_label_set_text_fmt(l_hint, "%c: tap the leading pad", 'A' + edit_ch);
    lv_obj_invalidate(wiring);
}
static void on_wiring_tap(lv_event_t *e) {
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(wiring, &a);
    int px[4], py, pad = -1;
    wiring_pads(&a, px, &py);
    for (int i = 0; i < 4; i++)
        if (abs(pt.x - px[i]) < 48 && abs(pt.y - py) < 52) pad = i;
    if (pad < 0) return;
    if (edit_ch < 0) { lv_label_set_text(l_hint, "Pick A or B first."); return; }
    if (first_pad < 0) {
        first_pad = pad;
        lv_label_set_text_fmt(l_hint, "%c from pad %d: now tap the other end", 'A' + edit_ch, pad + 1);
    } else if (pad == first_pad) {
        lv_label_set_text(l_hint, "The other end is a different pad.");
    } else {
        post(UI_EV_ROUTE, edit_ch, (first_pad + 1) * 10 + pad + 1);
        edit_ch = first_pad = -1;
        lv_label_set_text(l_hint, HINT_IDLE);
    }
    lv_obj_invalidate(wiring);
}

// ---- custom drawing: the wiring picture and the output graphs ------------------------------------------------------
static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t col, int w, lv_opa_t opa) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col);
    d.width = w;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(layer, &d);
}
static void draw_text(lv_layer_t *layer, int cx, int cy, const char *txt, const lv_font_t *f, uint32_t col) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_hex(col);
    d.font = f;
    d.text = txt;
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t a = {cx - 40, cy - f->line_height / 2, cx + 40, cy + f->line_height / 2};
    lv_draw_label(layer, &d, &a);
}

static void wiring_draw(lv_event_t *e) {
    if (!S) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    int py, r = 28;
    int px[4];
    wiring_pads(&a, px, &py);
    for (int ch = 0; ch < 2; ch++) {                      // arcs over the pads, arrow at the trailing pad
        int from = S->route[ch] / 10 - 1, to = S->route[ch] % 10 - 1;
        if (from < 0 || to < 0 || from > 3 || to > 3) continue;
        int x1 = px[from], x2 = px[to], cx = (x1 + x2) / 2, rad = abs(x2 - x1) / 2;
        uint32_t col = ch ? C_B : C_A;
        lv_draw_arc_dsc_t ad;
        lv_draw_arc_dsc_init(&ad);
        ad.color = lv_color_hex(col);
        ad.width = 7;
        ad.center.x = cx;
        ad.center.y = py - r - 2;
        ad.radius = rad;
        ad.start_angle = 180;
        ad.end_angle = 360;
        ad.rounded = 1;
        lv_draw_arc(layer, &ad);
        int tx = x2, ty = py - r - 2;                     // arrowhead pointing down onto the trailing pad
        draw_line(layer, tx - 11, ty - 14, tx, ty, col, 7, LV_OPA_COVER);
        draw_line(layer, tx + 11, ty - 14, tx, ty, col, 7, LV_OPA_COVER);
        draw_text(layer, cx, py - r - 2 - rad - 16, ch ? "B" : "A", &lv_font_montserrat_20, col);
    }
    for (int i = 0; i < 4; i++) {
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = lv_color_hex(EL_COL[i]);
        rd.radius = LV_RADIUS_CIRCLE;
        if (i == first_pad) {                               // the leading pad just tapped
            lv_draw_rect_dsc_t ring;
            lv_draw_rect_dsc_init(&ring);
            ring.bg_opa = LV_OPA_TRANSP;
            ring.border_color = lv_color_white();
            ring.border_width = 4;
            ring.radius = LV_RADIUS_CIRCLE;
            lv_area_t rr = {px[i] - r - 7, py - r - 7, px[i] + r + 7, py + r + 7};
            lv_draw_rect(layer, &ring, &rr);
        }
        lv_area_t c = {px[i] - r, py - r, px[i] + r, py + r};
        lv_draw_rect(layer, &rd, &c);
        char t[2] = {(char)('1' + i), 0};
        draw_text(layer, px[i], py, t, &lv_font_montserrat_28, EL_INK[i]);
    }
}

static float rate_y(float hz) {                           // log 10..400 Hz -> 0..1, like the M5 remote
    if (hz < 10) hz = 10;
    float v = logf(hz / 10.0f) / logf(40.0f);
    return v > 1 ? 1 : v;
}
// The graphs are drawn into an RGB565 canvas with direct pixel writes (one image per frame for LVGL), not as
// thousands of lv_draw_line jobs (that took ~4 s a frame on the P4, 2026-09-29).
static lv_obj_t *gcanvas[2];
static uint16_t *gbuf[2];
static int gw, gh;
static inline uint16_t rgb565(uint32_t c) { return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F); }
static inline uint16_t blend(uint16_t bg, uint16_t fg, int a) {   // a 0..32
    int r = ((bg >> 11) * (32 - a) + (fg >> 11) * a) >> 5, g = (((bg >> 5) & 63) * (32 - a) + ((fg >> 5) & 63) * a) >> 5,
        b = ((bg & 31) * (32 - a) + (fg & 31) * a) >> 5;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static void gline(uint16_t *buf, int x0, int y0, int x1, int y1, uint16_t c, int thick) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        for (int t = 0; t < thick; t++) {
            int y = y0 + t - thick / 2;
            if (x0 >= 0 && x0 < gw && y >= 0 && y < gh) buf[y * gw + x0] = c;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void graph_render(int ch) {
    uint16_t *buf = gbuf[ch];
    if (!buf || !S) return;
    const uint16_t bg = rgb565(0x0f1218), col = rgb565(ch ? C_B : C_A), fill = blend(bg, col, 9);
    const uint16_t rate = rgb565(C_RATE), wid = rgb565(C_WIDTH), mid = rgb565(0x4a5162);
    for (int k = 0; k < gw * gh; k++) buf[k] = bg;
    int top = 2, base = gh - 3, span = base - top;
    for (int x = 0; x < gw; x += 6) buf[(top + span / 2) * gw + x] = mid;
    int pi = -1, pr = 0, pw = 0;
    for (int x = 0; x < gw; x++) {
        int i = (S->hist_head + x * UI_HIST / gw) % UI_HIST;
        int yi = base - (int)(S->hist_int[ch][i] * span);
        int yr = base - (int)(rate_y(S->hist_rate[ch][i]) * span);
        int yw = base - (int)(S->hist_width[ch][i] / 400.0f * span);
        for (int y = yi; y < base; y++) buf[y * gw + x] = fill;
        if (pi >= 0) {
            gline(buf, x - 1, pw, x, yw, wid, 2);
            gline(buf, x - 1, pi, x, yi, col, 2);
            gline(buf, x - 1, pr, x, yr, rate, 3);
        }
        pi = yi, pr = yr, pw = yw;
    }
    lv_obj_invalidate(gcanvas[ch]);
}
static void graph_make_canvases(void) {           // after layout: the canvases fill the graph boxes under the legend
    lv_obj_update_layout(lv_screen_active());
    gw = lv_obj_get_content_width(graph[0]);
    gh = lv_obj_get_content_height(graph[0]) - 30;
    for (int ch = 0; ch < 2; ch++) {
        gbuf[ch] = heap_caps_malloc((size_t)gw * gh * 2, MALLOC_CAP_SPIRAM);
        gcanvas[ch] = lv_canvas_create(graph[ch]);
        lv_canvas_set_buffer(gcanvas[ch], gbuf[ch], gw, gh, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(gcanvas[ch], LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

// ---- the screen ---------------------------------------------------------------------------------------------------
void ui_create(ui_state_t *st) {
    S = st;
    if (!evq) evq = xQueueCreate(32, sizeof(ui_event_t));
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 14, 0);
    static int32_t cols[] = {LV_GRID_FR(21), LV_GRID_FR(20), 104, LV_GRID_TEMPLATE_LAST};   // FR() must be < 100
    static int32_t rows[] = {52, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(scr, cols, rows);
    lv_obj_set_style_pad_row(scr, 12, 0);
    lv_obj_set_style_pad_column(scr, 12, 0);

    // top bar
    lv_obj_t *top = top_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_grid_cell(top, LV_GRID_ALIGN_STRETCH, 0, 3, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 10, 0);
    lv_obj_t *logo = label(top, "Pla#3b6cf0 Stim#", &lv_font_montserrat_32, C_TXT);
    lv_label_set_recolor(logo, true);
    l_box = chip(top); l_link = chip(top); l_fw = chip(top); l_batt = chip(top);
    l_note = label(top, "", &lv_font_montserrat_16, C_MUT);
    lv_obj_set_flex_grow(l_note, 1);
    lv_obj_t *pill = box(top, 0x12351f, 10);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    l_state = label(pill, "STOPPED", &lv_font_montserrat_20, 0x5ee08e);
    lv_obj_center(l_state);
    lv_obj_t *gear = button(top, LV_SYMBOL_SETTINGS, &lv_font_montserrat_24, C_PANEL2, C_MUT);
    lv_obj_set_size(gear, 50, 48);
    lv_obj_add_event_cb(gear, on_click, LV_EVENT_CLICKED, (void *)8);

    // left: pattern, levels, output
    lv_obj_t *left = col_left = lv_obj_create(scr);
    lv_obj_remove_style_all(left);
    lv_obj_set_grid_cell(left, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 12, 0);
    lv_obj_t *pc = card(left);
    lv_obj_set_size(pc, LV_PCT(100), 86);
    lv_obj_set_flex_flow(pc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pc, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *prev = button(pc, LV_SYMBOL_LEFT, &lv_font_montserrat_24, C_PANEL2, C_TXT);
    lv_obj_set_size(prev, 58, 58);
    lv_obj_add_event_cb(prev, on_click, LV_EVENT_CLICKED, (void *)5);
    lv_obj_t *pn = lv_obj_create(pc);
    lv_obj_remove_style_all(pn);
    lv_obj_set_flex_grow(pn, 1);
    lv_obj_set_height(pn, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(pn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pn, on_click, LV_EVENT_CLICKED, (void *)7);
    l_pat = label(pn, "", &lv_font_montserrat_28, C_TXT);
    l_patsub = label(pn, "", &lv_font_montserrat_16, C_MUT);
    lv_obj_t *next = button(pc, LV_SYMBOL_RIGHT, &lv_font_montserrat_24, C_PANEL2, C_TXT);
    lv_obj_set_size(next, 58, 58);
    lv_obj_add_event_cb(next, on_click, LV_EVENT_CLICKED, (void *)6);

    lv_obj_t *lc = card(left);
    lv_obj_set_size(lc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lc, 0, 0);
    for (int ch = 0; ch < 2; ch++) {
        slider_row(lc, ch ? "Level B" : "Level A", ch ? 0x123f57 : 0x6a2e19, ch ? C_B : C_A, &sl_level[ch],
                   &l_level[ch], &l_levsub[ch]);
        l_meas[ch] = label(lc, "", &lv_font_montserrat_14, C_MUT);
        lv_obj_set_width(l_meas[ch], LV_PCT(100));
        lv_obj_set_style_text_align(l_meas[ch], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *oc = card(left);
    lv_obj_set_width(oc, LV_PCT(100));
    lv_obj_set_flex_grow(oc, 1);
    lv_obj_set_flex_flow(oc, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *oh = label(oc, "OUTPUT  last 10 s   fill = strength   #b9c0cc grey = rate#   #ff3fd8 magenta = width#",
                         &lv_font_montserrat_14, C_MUT);
    lv_label_set_recolor(oh, true);
    lv_obj_t *gr = lv_obj_create(oc);
    lv_obj_remove_style_all(gr);
    lv_obj_set_width(gr, LV_PCT(100));
    lv_obj_set_flex_grow(gr, 1);
    lv_obj_set_flex_flow(gr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(gr, 12, 0);
    for (int ch = 0; ch < 2; ch++) {
        graph[ch] = box(gr, 0x0f1218, 10);
        lv_obj_set_height(graph[ch], LV_PCT(100));
        lv_obj_set_flex_grow(graph[ch], 1);
        lv_obj_set_style_pad_all(graph[ch], 8, 0);
        l_legend[ch] = label(graph[ch], "", &lv_font_montserrat_18, C_MUT);
        lv_label_set_recolor(l_legend[ch], true);
    }

    // middle: wiring, shape, Wi-Fi, master, STOP
    lv_obj_t *mid = col_mid = lv_obj_create(scr);
    lv_obj_remove_style_all(mid);
    lv_obj_set_grid_cell(mid, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, 10, 0);
    lv_obj_t *wc = card(mid);
    lv_obj_set_size(wc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wc, 6, 0);
    lv_obj_t *wh = lv_obj_create(wc);
    lv_obj_remove_style_all(wh);
    lv_obj_set_size(wh, LV_PCT(100), 48);
    lv_obj_set_flex_flow(wh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wh, 10, 0);
    for (int ch = 0; ch < 2; ch++) {
        btn_ch[ch] = button(wh, "", &lv_font_montserrat_20, C_PANEL2, ch ? C_B : C_A);
        lv_obj_set_size(btn_ch[ch], 104, 46);
        l_ch[ch] = lv_obj_get_child(btn_ch[ch], 0);
        lv_obj_add_event_cb(btn_ch[ch], on_channel, LV_EVENT_CLICKED, (void *)(intptr_t)ch);
    }
    l_hint = label(wh, HINT_IDLE, &lv_font_montserrat_14, C_MUT);
    lv_label_set_long_mode(l_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(l_hint, 1);
    wiring = lv_obj_create(wc);
    lv_obj_remove_style_all(wiring);
    lv_obj_set_size(wiring, LV_PCT(100), 124);
    lv_obj_add_flag(wiring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wiring, wiring_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(wiring, on_wiring_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fr = lv_obj_create(wc);
    lv_obj_remove_style_all(fr);
    lv_obj_set_size(fr, LV_PCT(100), 48);
    lv_obj_set_flex_flow(fr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fr, 10, 0);
    const char *fl[] = {LV_SYMBOL_REFRESH " flip A", LV_SYMBOL_REFRESH " flip B", "A+B same pads"};
    const uint32_t fc[] = {C_A, C_B, C_TXT};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = button(fr, fl[i], &lv_font_montserrat_18, C_PANEL2, fc[i]);
        lv_obj_set_height(b, 48);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, (void *)(intptr_t)(2 + i));
    }

    lv_obj_t *sc = card(mid);
    lv_obj_set_size(sc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    label(sc, "PULSE SHAPE", &lv_font_montserrat_14, C_MUT);
    static const char *shapes[] = {"Triangle", "Rounded", "Taper", "Soft square", ""};
    bm_shape = lv_buttonmatrix_create(sc);
    lv_buttonmatrix_set_map(bm_shape, shapes);
    lv_buttonmatrix_set_button_ctrl_all(bm_shape, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm_shape, true);
    lv_obj_set_size(bm_shape, LV_PCT(100), 62);
    lv_obj_set_style_bg_opa(bm_shape, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm_shape, 0, 0);
    lv_obj_set_style_pad_all(bm_shape, 0, 0);
    lv_obj_set_style_pad_column(bm_shape, 8, 0);
    lv_obj_set_style_bg_color(bm_shape, lv_color_hex(C_PANEL2), LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm_shape, lv_color_hex(C_TXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm_shape, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm_shape, lv_color_hex(0x1c2a52), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(bm_shape, lv_color_hex(C_BRAND), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(bm_shape, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm_shape, 12, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm_shape, on_shape, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sp = lv_obj_create(mid);
    lv_obj_remove_style_all(sp);
    lv_obj_set_width(sp, 1);
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t *mc = card(mid);
    lv_obj_set_size(mc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(mc, 6, 0);
    slider_row(mc, "Master", 0x2b3f7a, C_BRAND, &sl_master, &l_master, &l_mastersub);

    btn_stop = lv_button_create(mid);
    lv_obj_set_size(btn_stop, LV_PCT(100), 82);
    lv_obj_set_style_radius(btn_stop, 16, 0);
    lv_obj_set_style_shadow_width(btn_stop, 0, 0);
    l_stop = label(btn_stop, "", &lv_font_montserrat_40, C_TXT);
    lv_label_set_recolor(l_stop, true);
    lv_obj_center(l_stop);
    lv_obj_add_event_cb(btn_stop, on_click, LV_EVENT_CLICKED, (void *)1);

    // right: MA, vertical
    lv_obj_t *mac = col_ma = card(scr);
    lv_obj_set_grid_cell(mac, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_flex_flow(mac, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mac, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(mac, 10, 0);
    label(mac, "MA", &lv_font_montserrat_24, C_TXT);
    lv_obj_t *mas = label(mac, "Multi\nAdjust", &lv_font_montserrat_12, C_MUT);
    lv_obj_set_style_text_align(mas, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *mplus = button(mac, LV_SYMBOL_PLUS, &lv_font_montserrat_24, C_PANEL2, C_TXT);
    lv_obj_set_size(mplus, 60, 52);
    sl_ma = lv_slider_create(mac);
    lv_slider_set_range(sl_ma, 0, 100);
    lv_obj_set_width(sl_ma, 34);
    lv_obj_set_flex_grow(sl_ma, 1);
    style_slider(sl_ma, 0xaab3c5, 0x555e70, true);
    lv_obj_add_event_cb(sl_ma, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *mminus = button(mac, LV_SYMBOL_MINUS, &lv_font_montserrat_24, C_PANEL2, C_TXT);
    lv_obj_set_size(mminus, 60, 52);
    lv_obj_set_user_data(mplus, (void *)(intptr_t)1);
    lv_obj_set_user_data(mminus, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(mplus, on_step, LV_EVENT_CLICKED, sl_ma);
    lv_obj_add_event_cb(mminus, on_step, LV_EVENT_CLICKED, sl_ma);
    l_ma = label(mac, "0", &lv_font_montserrat_28, C_TXT);

    settings = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(settings, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(settings, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(settings, 0, 0);
    lv_obj_set_style_radius(settings, 0, 0);
    lv_obj_set_style_pad_all(settings, 24, 0);
    lv_obj_set_flex_flow(settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(settings, 16, 0);
    lv_obj_t *sh = lv_obj_create(settings);
    lv_obj_remove_style_all(sh);
    lv_obj_set_size(sh, LV_PCT(100), 60);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sh, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(sh, "Settings", &lv_font_montserrat_32, C_TXT);
    lv_obj_t *back = button(sh, LV_SYMBOL_LEFT " Back", &lv_font_montserrat_24, C_PANEL2, C_TXT);
    lv_obj_set_size(back, 150, 56);
    lv_obj_add_event_cb(back, on_click, LV_EVENT_CLICKED, (void *)10);
    lv_obj_t *wf = card(settings);
    lv_obj_set_size(wf, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wf, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(wf, 20, 0);
    lv_obj_set_style_pad_column(wf, 18, 0);
    label(wf, LV_SYMBOL_WIFI, &lv_font_montserrat_40, C_BRAND);
    l_wifi = label(wf, "", &lv_font_montserrat_24, C_TXT);
    lv_label_set_recolor(l_wifi, true);
    lv_obj_set_flex_grow(l_wifi, 1);
    lv_obj_add_flag(l_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(l_wifi, on_click, LV_EVENT_CLICKED, (void *)9);
    l_joined = chip(wf);
    lv_obj_t *bx = card(settings);
    lv_obj_set_size(bx, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bx, 20, 0);
    l_set_box = label(bx, "", &lv_font_montserrat_24, C_TXT);
    lv_label_set_recolor(l_set_box, true);
    label(settings, "Patterns, the Wi-Fi setup and the safety limits come from the PC app (M5 remote tab).",
          &lv_font_montserrat_18, C_MUT);
    lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN);

    graph_make_canvases();
    ui_refresh(st);
}

void ui_note(const char *text) { lv_label_set_text(l_note, text); }

static void set_text(lv_obj_t *l, const char *fmt, ...) {        // re-set a label only when its text changed
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (strcmp(lv_label_get_text(l), b) != 0) lv_label_set_text(l, b);
}

static void set_slider(lv_obj_t *s, int v) {
    if (lv_slider_get_value(s) != v && !lv_obj_has_state(s, LV_STATE_PRESSED)) lv_slider_set_value(s, v, LV_ANIM_OFF);
}

void ui_refresh(const ui_state_t *st) {
    char b[160];
    S = st;
    set_text(l_box, "box #e8ecf3 %s#", st->box_name);
    set_text(l_link, "link #e8ecf3 %d ms#", st->link_ms);
    set_text(l_fw, "fw #e8ecf3 %s#", st->fw);
    set_text(l_batt, "stim-batt #e8ecf3 %d%%#", st->box_batt);
    set_text(l_state, "%s", st->running ? "RUNNING" : "STOPPED");
    lv_obj_set_style_text_color(l_state, lv_color_hex(st->running ? 0x5ee08e : 0xf0b429), 0);
    lv_obj_set_style_bg_color(lv_obj_get_parent(l_state), lv_color_hex(st->running ? 0x12351f : 0x3a2c0c), 0);
    set_text(l_pat, "%s", st->pattern);
    set_text(l_patsub, "%s - %d / %d - tap for the list", st->pattern_group, st->pattern_index,
                          st->pattern_count);
    for (int ch = 0; ch < 2; ch++) {
        set_slider(sl_level[ch], st->level_target[ch]);
        set_text(l_level[ch], st->level[ch] < st->level_target[ch] ? "%d..." : "%d", st->level[ch]);
        set_text(l_ch[ch], "%c  %d", 'A' + ch, st->route[ch]);
        lv_obj_set_style_border_color(btn_ch[ch], lv_color_hex(edit_ch == ch ? (ch ? C_B : C_A) : C_LINE), 0);
        lv_obj_set_style_border_width(btn_ch[ch], edit_ch == ch ? 3 : 1, 0);
        set_text(l_levsub[ch], "pads %d -> %d", st->route[ch] / 10, st->route[ch] % 10);
        set_text(l_meas[ch], "%c measured %d mA", 'A' + ch, st->ma_measured[ch]);
        set_text(l_legend[ch], "#%06x %c %d%%#  #b9c0cc %.0f Hz#  #ff3fd8 %.0f us#",
                              (unsigned)(ch ? C_B : C_A), 'A' + ch, (int)(st->int_now[ch] * 100), st->rate_now[ch],
                              st->width_now[ch]);
        graph_render(ch);
    }
    set_slider(sl_master, st->master_target);
    set_text(l_master, st->master < st->master_target ? "%d..." : "%d", st->master);
    set_text(l_mastersub, "box knob #e8ecf3 %d%%#\n-> #e8ecf3 %d%% out#", st->box_knob,
                          st->box_knob * st->master / 100);
    set_slider(sl_ma, st->ma);
    set_text(l_ma, "%d", st->ma);
    uint32_t sel = lv_buttonmatrix_get_selected_button(bm_shape);
    if ((int)sel != st->shape) lv_buttonmatrix_set_button_ctrl(bm_shape, st->shape, LV_BUTTONMATRIX_CTRL_CHECKED);
    snprintf(b, sizeof b, "#8b93a3 The remote's own Wi-Fi (the box joins this)#\nName  #ffffff %s#\nPassword  #ffffff %s#  %s\nChannel  %d",
             st->ap_ssid, show_pass ? st->ap_pass : "********", show_pass ? "" : "#3b6cf0 (tap to show)#", st->ap_channel);
    set_text(l_wifi, "%s", b);
    set_text(l_joined, "%s #e8ecf3 %s#", st->box_name, st->box_joined ? "joined" : "not joined");
    set_text(l_set_box, "#8b93a3 Box#\n%s   firmware %s   link %d ms   stim-batt %d%%", st->box_name, st->fw, st->link_ms,
             st->box_batt);
    lv_obj_set_style_bg_color(btn_stop, lv_color_hex(st->running ? C_STOP : C_GO), 0);
    set_text(l_stop, "%s", st->running ? "STOP" : "START");
    static int last_routes = -1;
    if (st->route[0] * 100 + st->route[1] != last_routes) {
        last_routes = st->route[0] * 100 + st->route[1];
        lv_obj_invalidate(wiring);
    }
}

#include "esp_log.h"
void ui_debug_layout(void) {                      // under the lock: where the layout put things
    lv_display_t *d = lv_display_get_default();
    lv_obj_update_layout(lv_screen_active());
    ESP_LOGI("ui", "display %dx%d (rotation %d)", (int)lv_display_get_horizontal_resolution(d),
             (int)lv_display_get_vertical_resolution(d), (int)lv_display_get_rotation(d));
    lv_obj_t *o[] = {lv_screen_active(), top_bar, col_left, col_mid, col_ma};
    const char *n[] = {"screen", "top", "left", "mid", "ma"};
    for (int i = 0; i < 5; i++) {
        lv_area_t a;
        lv_obj_get_coords(o[i], &a);
        ESP_LOGI("ui", "%-6s x %d..%d  y %d..%d", n[i], (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2);
    }
}
