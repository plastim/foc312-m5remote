/* ctypes shim over the remote's controller for tests/test_remote_ctrl.py (built as a shared library with the core). */
#include <stdlib.h>
#include <string.h>

#include "ctrl.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

static ctrl_t C;
static pack_t P;
static uint8_t packbuf[1 << 20];
static safety_values_t V;

EXPORT int rc_init(const uint8_t *pack, int len, int n_boxes, double now, int use_saved, int pattern, int route0,
                   int route1, int box) {
    int err = PACK_OK;
    const pack_t *pk = NULL;
    if (len > 0) {
        memcpy(packbuf, pack, (size_t)len);
        err = pack_open(&P, packbuf, (size_t)len);
        if (err != PACK_OK) return -err;
        pk = &P;
    }
    ctrl_settings_t s;
    ctrl_default_settings(&s);
    s.pattern = pattern;
    s.wire_route[0] = route0;
    s.wire_route[1] = route1;
    s.box = box;
    ctrl_init(&C, pk, use_saved ? &s : NULL, n_boxes, now, 4.0, 2.0, 3.0, 0.15, 3.0, 7);
    return 0;
}
EXPORT void rc_knob(int knob, int detents, double now) { ctrl_knob(&C, (ctrl_knob_t)knob, detents, now); }
EXPORT void rc_push1(double now) { ctrl_push_knob1(&C, now); }
EXPORT void rc_push4(double now) { ctrl_push_knob4(&C, now); }
EXPORT void rc_button(double now) { ctrl_button(&C, now); }
EXPORT void rc_link(int running, const char *fault, double now) { ctrl_link(&C, running != 0, fault, now); }
EXPORT void rc_tick(double now) { ctrl_tick(&C, now, &V); }
EXPORT void rc_set_fork(int v, double now) { ctrl_set_box_fork(&C, v, now); }
EXPORT int rc_shape(void) { return C.set.shape; }
EXPORT int rc_shape_sent(int ch) { return V.shape[ch & 1]; }
EXPORT double rc_charge_factor(int shape, double width) { return shape_charge_factor(shape, width); }
EXPORT double rc_amps(int ch) { return V.amps[ch & 1]; }
EXPORT int rc_route(int ch) { return V.route[ch & 1]; }
EXPORT int rc_armed(void) { return ctrl_armed(&C); }
EXPORT int rc_screen(void) { return (int)C.screen; }
EXPORT int rc_cursor(void) { return C.cursor; }
EXPORT int rc_level(int pos) { return ctrl_level_percent(&C, pos); }
EXPORT int rc_level_target(int pos) { return C.level_target[pos & 1]; }
EXPORT int rc_master(void) { return ctrl_master_percent(&C); }
EXPORT int rc_ma(void) { return C.ma_steps; }
EXPORT double rc_foc_level(int ch) { return C.foc.levels[ch & 1]; }
EXPORT int rc_foc_route(int ch) { return C.foc.routes[ch & 1]; }
EXPORT int rc_pattern(void) { return C.set.pattern; }
EXPORT int rc_box(void) { return C.set.box; }
EXPORT int rc_wire(int pos) { return C.set.wire_route[pos & 1]; }
EXPORT double rc_volume(void) { return safety_volume(&C.saf); }
EXPORT const char *rc_status(void) { return C.status; }
EXPORT const char *rc_option(int opt) {
    static char buf[64];
    return ctrl_option_text(&C, opt, buf, sizeof buf);
}
