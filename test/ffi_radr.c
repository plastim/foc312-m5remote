/* ctypes shim over the RADR build's controls and check (core/radr.c) for tests/test_remote_radr.py, built with
 * ffi_ctrl.c (its controller: rc_init, rc_tick, ...). */
#include "radr.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

ctrl_t *rc_ctrl(void);

static radr_controls_t R;
static radr_check_t K;

EXPORT void rr_init(void) { radr_controls_init(&R); }
EXPORT void rr_knob(int knob, int detents, double now) { radr_knob(&R, rc_ctrl(), knob, detents, now); }
EXPORT void rr_button(int button, int down, double now) { radr_button(&R, rc_ctrl(), button, down != 0, now); }
EXPORT int rr_focus(void) { return R.focus; }
EXPORT unsigned rr_focus_seq(void) { return R.focus_seq; }
EXPORT int rr_right_knob(void) { return radr_right_knob(&R, rc_ctrl()); }

EXPORT void rk_begin(int dir_l, int dir_r, int flip, int invert) {
    int dir[RADR_N_KNOBS] = {dir_l, dir_r};
    radr_check_begin(&K, dir, flip != 0, invert != 0);
}
EXPORT void rk_knob(int knob, int raw) { radr_check_knob(&K, knob, raw); }
EXPORT void rk_button(int button, int down) { radr_check_button(&K, button, down != 0); }
EXPORT int rk_step(void) { return K.step; }
EXPORT int rk_dir(int knob) { return K.dir[knob & 1]; }
EXPORT int rk_settled(int knob) { return K.settled[knob & 1]; }
EXPORT int rk_cw(int knob) { return K.cw[knob & 1]; }
EXPORT int rk_ccw(int knob) { return K.ccw[knob & 1]; }
EXPORT int rk_knob_ok(int knob) { return radr_check_knob_ok(&K, knob); }
EXPORT int rk_pressed(int button) { return button >= 0 && button < RADR_N_BTNS && K.pressed[button]; }
EXPORT int rk_flip(void) { return K.flip; }
EXPORT int rk_invert(void) { return K.invert; }
EXPORT int rk_done(void) { return radr_check_done(&K); }
EXPORT const char *rk_button_name(int button) { return radr_button_name(button); }
