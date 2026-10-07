/* The remote's controls on the RADR hardware, and its first-start check (radr.h). */
#include "radr.h"

#include <string.h>

/* ---- the controls ------------------------------------------------------------------------------------------------ */
void radr_controls_init(radr_controls_t *r) {
    memset(r, 0, sizeof *r);
    r->focus = RADR_FOCUS_A;
}

static const ctrl_knob_t FOCUS_KNOB[RADR_N_FOCUS] = {KNOB_2, KNOB_3, KNOB_4};

int radr_right_knob(const radr_controls_t *r, const ctrl_t *c) {
    int f = r->focus >= 0 && r->focus < RADR_N_FOCUS ? r->focus : RADR_FOCUS_A;
    switch (c->screen) {
    case SCREEN_RUN:
    case SCREEN_OPTIONS: return FOCUS_KNOB[f];          /* A's level / pair, B's level / pair, MA / the shape */
    case SCREEN_PATTERNS: return KNOB_2;               /* ten at a time */
    default: return -1;
    }
}

void radr_knob(radr_controls_t *r, ctrl_t *c, int knob, int detents, double now) {
    if (!detents) return;
    if (knob == RADR_KNOB_L) {
        ctrl_knob(c, KNOB_1, detents, now);
        return;
    }
    int k = radr_right_knob(r, c);
    if (k >= 0) ctrl_knob(c, (ctrl_knob_t)k, detents, now);
}

static void move_focus(radr_controls_t *r, int by) {   /* wraps: MA -> A, A -> MA (the owner, 2026-10-06) */
    int f = ((r->focus + by) % RADR_N_FOCUS + RADR_N_FOCUS) % RADR_N_FOCUS;
    if (f != r->focus) {
        r->focus = f;
        r->focus_seq++;
    }
}

/* The controller's two presses (ctrl.h): knob 1's opens the list on the run screen, picks in the list and activates in
 * Options; knob 4's opens Options on the run screen and goes back from the list and Options. */
void radr_button(radr_controls_t *r, ctrl_t *c, int button, bool down, double now) {
    if (!down) return;                                 /* every button acts on the press */
    switch (button) {
    case RADR_BTN_CENTRE:                              /* START / STOP (STOP always wins), on every screen */
        ctrl_button(c, now);
        break;
    /* the shoulders (the owner, 2026-10-06): on the run screen they move the focus; in the patterns list top-left
       selects and top-right goes back; in Options top-left selects the highlighted row and top-right steps the
       focused channel (A, or B when B has the focus) to its next pair, live through the usual rewire. */
    case RADR_BTN_L_SHOULDER:
        if (c->screen == SCREEN_PATTERNS || c->screen == SCREEN_OPTIONS) ctrl_push_knob1(c, now);   /* select */
        else move_focus(r, -1);
        break;
    case RADR_BTN_R_SHOULDER:
        if (c->screen == SCREEN_PATTERNS) ctrl_push_knob4(c, now);                                  /* back */
        else if (c->screen == SCREEN_OPTIONS)
            ctrl_option_activate(c, r->focus == RADR_FOCUS_B ? OPT_WIRES_2 : OPT_WIRES_1, now);     /* next pair */
        else move_focus(r, +1);
        break;
    /* under the screen: on the run screen the left one = Options, the right one = the patterns list. In the list and
       in Options the RIGHT button selects (picks / activates) and the left one goes back (the owner, 2026-10-06). */
    case RADR_BTN_UNDER_L:
        ctrl_push_knob4(c, now);                       /* run: Options; list / Options: back */
        break;
    case RADR_BTN_UNDER_R:
        ctrl_push_knob1(c, now);                       /* run: the list; list: pick; Options: activate */
        break;
    default: break;
    }
}

/* ---- the check --------------------------------------------------------------------------------------------------- */
static const char *const BUTTON_NAMES[RADR_N_BTNS] = {"left shoulder", "right shoulder", "under-left", "centre",
                                                      "under-right"};

const char *radr_button_name(int button) {
    return button >= 0 && button < RADR_N_BTNS ? BUTTON_NAMES[button] : "?";
}

static void knobs_over(radr_check_t *k) {
    for (int i = 0; i < RADR_N_KNOBS; i++) {
        k->settled[i] = false;
        k->pending[i] = k->cw[i] = k->ccw[i] = k->last[i] = 0;
    }
}

void radr_check_begin(radr_check_t *k, const int dir[RADR_N_KNOBS], bool flip, bool invert) {
    memset(k, 0, sizeof *k);
    for (int i = 0; i < RADR_N_KNOBS; i++) k->dir[i] = dir && dir[i] < 0 ? -1 : 1;
    k->flip = flip;
    k->invert = invert;
    k->step = RADR_CHECK_KNOBS;
    knobs_over(k);
}

bool radr_check_knob_ok(const radr_check_t *k, int knob) {
    return knob >= 0 && knob < RADR_N_KNOBS && k->settled[knob] && k->cw[knob] >= RADR_CHECK_TURNS &&
           k->ccw[knob] >= RADR_CHECK_TURNS;
}

void radr_check_knob(radr_check_t *k, int knob, int raw) {
    if (knob < 0 || knob >= RADR_N_KNOBS || !raw || k->step != RADR_CHECK_KNOBS) return;
    k->seq++;
    if (!k->settled[knob]) {
        /* asked to turn it clockwise first: RADR_CHECK_SETTLE detents one way settle what clockwise reads as; a turn
         * back before that starts the count again */
        if ((k->pending[knob] > 0 && raw < 0) || (k->pending[knob] < 0 && raw > 0)) k->pending[knob] = 0;
        k->pending[knob] += raw;
        int n = k->pending[knob] < 0 ? -k->pending[knob] : k->pending[knob];
        if (n >= RADR_CHECK_SETTLE) {
            k->dir[knob] = k->pending[knob] > 0 ? 1 : -1;
            k->settled[knob] = true;
            k->cw[knob] = n;
            k->last[knob] = 1;
        }
        return;
    }
    int turned = raw * k->dir[knob];                  /* clockwise > 0 */
    if (turned > 0) k->cw[knob] += turned;
    else k->ccw[knob] -= turned;
    k->last[knob] = turned > 0 ? 1 : -1;
    if (radr_check_knob_ok(k, RADR_KNOB_L) && radr_check_knob_ok(k, RADR_KNOB_R)) k->step = RADR_CHECK_BUTTONS;
}

void radr_check_button(radr_check_t *k, int button, bool down) {
    if (button < 0 || button >= RADR_N_BTNS) return;
    k->down[button] = down;
    k->seq++;
    if (!down) return;
    switch (k->step) {
    case RADR_CHECK_KNOBS:
        if (button == RADR_BTN_UNDER_L) knobs_over(k);          /* start the knobs over (a turn the wrong way) */
        break;
    case RADR_CHECK_BUTTONS: {
        k->pressed[button] = true;
        bool all = true;
        for (int i = 0; i < RADR_N_BTNS; i++) all = all && k->pressed[i];
        if (all) {
            k->step = RADR_CHECK_SCREEN;
            for (int i = 0; i < RADR_N_BTNS; i++) k->down[i] = false;   /* (the last press is not the screen's) */
        }
        break;
    }
    case RADR_CHECK_SCREEN:
        if (button == RADR_BTN_UNDER_L) k->flip = !k->flip;
        else if (button == RADR_BTN_UNDER_R) k->invert = !k->invert;
        else if (button == RADR_BTN_CENTRE) k->step = RADR_CHECK_DONE;
        break;
    default: break;
    }
}

bool radr_check_done(const radr_check_t *k) {
    return k->step == RADR_CHECK_DONE;
}
