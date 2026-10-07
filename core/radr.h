/* The remote on the RADR hardware (R+D's wireless remote, v0.4: two knobs, two shoulder buttons, three buttons under
 * the screen): its controls on the remote's controller (ctrl.h), and its first-start check. Pure logic, no hardware:
 * the app feeds knob detents and button edges. Tested on the host (foc312-engine tests/test_remote_radr.py).
 *
 * The controls (owner-approved, 2026-10-06):
 *  - The left knob is Master on the run screen, always (ctrl KNOB_1: in the list it scrolls, in Options it moves the
 *    highlight, as knob 1 on the M5).
 *  - The right knob is the focused one of A / B / MA (ctrl KNOB_2 / KNOB_3 / KNOB_4). On the run screen the shoulder
 *    buttons move the focus, wrapping round (A <- MA, MA -> A): shown as tabs on the screen, each change a short
 *    vibration (focus_seq). In Options the right knob does the same knob's job there (A's pair, B's pair, the pulse
 *    shape); in the list it jumps 10.
 *  - The centre button is START / STOP, acting on the press on every screen (ctrl_button, as the M5's MX button: STOP
 *    always wins).
 *  - Under the screen: on the run screen the left button opens Options and the right one the Patterns list. In the
 *    list and in Options the RIGHT button selects (picks the pattern / activates the highlighted item) and the LEFT one
 *    goes back.
 *  - The shoulders in the list: top-left selects, top-right goes back. In Options: top-left selects the highlighted
 *    item, top-right steps the focused channel (A, or B when B has the focus) to its next pair, live.
 * Everything else (the rate limits, the restart block, the safety stack) is the controller's, as on the M5.
 *
 * The check (radr_check_*): on its first start the remote will not pair with or drive a box until its owner has turned
 * each knob both ways, pressed each button and confirmed the screen. The knob step also settles each knob's direction
 * on this unit: the owner is asked to turn it clockwise first, so the first RADR_CHECK_SETTLE detents one way are
 * clockwise (the v0.4 board swaps the left knob's A / B when its 24C04 EEPROM is fitted; whatever the wiring, the
 * check finds it). The screen step offers a 180-degree turn and colour inversion. The app keeps the result (NVS) and
 * Options > Remote check runs it again. */
#ifndef RADR_H
#define RADR_H

#include <stdbool.h>
#include <stdint.h>

#include "ctrl.h"

enum { RADR_KNOB_L = 0, RADR_KNOB_R = 1, RADR_N_KNOBS };
enum { RADR_BTN_L_SHOULDER = 0, RADR_BTN_R_SHOULDER, RADR_BTN_UNDER_L, RADR_BTN_CENTRE, RADR_BTN_UNDER_R, RADR_N_BTNS };
enum { RADR_FOCUS_A = 0, RADR_FOCUS_B, RADR_FOCUS_MA, RADR_N_FOCUS };

typedef struct {
    int focus;                    /* RADR_FOCUS_*: what the right knob is on the run screen and in Options */
    uint32_t focus_seq;           /* +1 for every focus change (the app: a short vibration) */
} radr_controls_t;

void radr_controls_init(radr_controls_t *r);
/* a knob turned by `detents` (clockwise > 0, the direction already applied) */
void radr_knob(radr_controls_t *r, ctrl_t *c, int knob, int detents, double now);
/* a button pressed (down) or released */
void radr_button(radr_controls_t *r, ctrl_t *c, int button, bool down, double now);
/* the controller knob the right knob is on this screen (-1: none) */
int radr_right_knob(const radr_controls_t *r, const ctrl_t *c);

/* ---- the check ------------------------------------------------------------------------------------------------ */
#define RADR_CHECK_SETTLE 2       /* detents one way that settle a knob's direction (asked: clockwise first) */
#define RADR_CHECK_TURNS 3        /* detents each way to pass */

enum { RADR_CHECK_KNOBS = 0, RADR_CHECK_BUTTONS, RADR_CHECK_SCREEN, RADR_CHECK_DONE };

typedef struct {
    int step;                     /* RADR_CHECK_* */
    int dir[RADR_N_KNOBS];        /* what a clockwise detent reads as: +1 / -1 (settled in the knob step) */
    bool settled[RADR_N_KNOBS];
    int pending[RADR_N_KNOBS];    /* raw detents before the direction is settled */
    int cw[RADR_N_KNOBS], ccw[RADR_N_KNOBS];   /* detents seen each way once settled */
    int last[RADR_N_KNOBS];       /* the last turn's way: +1 clockwise, -1 counter-clockwise, 0 none (the screen) */
    bool pressed[RADR_N_BTNS];
    bool down[RADR_N_BTNS];       /* held now (the screen) */
    bool flip, invert;            /* the screen: turned 180 degrees, colours inverted */
    uint32_t seq;                 /* +1 for every change (the app redraws, ticks) */
} radr_check_t;

/* start the check (dir: the knobs' directions before it, e.g. the board's guess; flip / invert: the screen's now) */
void radr_check_begin(radr_check_t *k, const int dir[RADR_N_KNOBS], bool flip, bool invert);
/* a knob turned: raw detents as the encoder reads them (no direction applied) */
void radr_check_knob(radr_check_t *k, int knob, int raw_detents);
/* a button pressed (down) or released. Knob step: under-left starts the knobs over. Screen step: under-left turns the
 * screen 180 degrees, under-right inverts the colours, the centre button confirms (done) */
void radr_check_button(radr_check_t *k, int button, bool down);
bool radr_check_knob_ok(const radr_check_t *k, int knob);
bool radr_check_done(const radr_check_t *k);
const char *radr_button_name(int button);

#endif
