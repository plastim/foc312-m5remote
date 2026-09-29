/* The remote's controller (see ctrl.h). */
#include "ctrl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const int ROUTES[CTRL_N_ROUTES] = {12, 34, 23, 41, 13, 24};
/* the shapes knob 2 steps through, peakiest to squarest. Taper stops at 80 %: above it the edges are sharper than
 * soft square's 20 us, and square itself stays bench-only (it tripped on skin, 2026-09-26). Fork v7 for all but
 * rounded and soft square. */
static const int SHAPE_STEPS[] = {SHAPE_TRIANGLE, SHAPE_ROUNDED, 41, 42, 43, 44, 45, 46, 47, 48, SHAPE_SOFT};
#define N_SHAPE_STEPS ((int)(sizeof SHAPE_STEPS / sizeof SHAPE_STEPS[0]))

static bool shape_offered(const ctrl_t *c, int shape) {
    for (int i = 0; i < N_SHAPE_STEPS; i++)
        if (SHAPE_STEPS[i] == shape) return shape_min_fork(shape) <= c->saf.fork_version || shape_min_fork(shape) <= 2;
    return false;
}

/* the next offered shape from `shape` in direction dir (+1 / -1); wrap: past the end back to the start */
static int step_shape(const ctrl_t *c, int shape, int dir, bool wrap) {
    int k = 0;
    for (int i = 0; i < N_SHAPE_STEPS; i++)
        if (SHAPE_STEPS[i] == shape) k = i;
    for (int n = 0; n < N_SHAPE_STEPS; n++) {
        k += dir;
        if (k < 0 || k >= N_SHAPE_STEPS) {
            if (!wrap) return shape;
            k = (k + N_SHAPE_STEPS) % N_SHAPE_STEPS;
        }
        if (shape_offered(c, SHAPE_STEPS[k])) return SHAPE_STEPS[k];
    }
    return shape;
}
/* every pair both ways, for knobs 3 and 4 on the options screen: even = as listed, odd = reversed */
static const int DIRECTED[2 * CTRL_N_ROUTES] = {12, 21, 34, 43, 23, 32, 41, 14, 13, 31, 24, 42};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void ctrl_default_settings(ctrl_settings_t *s) {
    memset(s, 0, sizeof *s);
    s->pattern = -1;
    s->wire_route[0] = 12;
    s->wire_route[1] = 34;
    for (int i = 0; i < 4; i++) s->pads[i] = true;
    s->shape = SHAPE_ROUNDED;
}

static void status(ctrl_t *c, const char *text) {
    snprintf(c->status, sizeof c->status, "%s", text);
}

static int position_route(const ctrl_t *c, int pos) {
    int r = c->set.wire_route[pos];
    return c->set.reversed[pos] ? route_reverse(r) : r;
}

static void apply_routing(ctrl_t *c, double now) {
    int r0 = position_route(c, 0), r1 = position_route(c, 1);
    foc312_set_route(&c->foc, 0, c->set.swapped ? r1 : r0, &c->saf, now);
    foc312_set_route(&c->foc, 1, c->set.swapped ? r0 : r1, &c->saf, now);
}

static void play_pattern(ctrl_t *c, int index) {
    pack_entry_t e;
    if (!c->pack || pack_entry(c->pack, index, &e) != PACK_OK) {
        status(c, "pattern not found");
        return;
    }
    if (pack_play(c->pack, &e, &c->foc.et) != ET_OK) {
        status(c, "pattern would not start");
        return;
    }
    et312_frame(&c->foc.et, &c->foc.frame);
    c->set.pattern = index;
    snprintf(c->status, sizeof c->status, "%s", e.name);
}

static void zero_levels(ctrl_t *c) {
    c->level_target[0] = c->level_target[1] = 0;
    c->level_applied[0] = c->level_applied[1] = 0.0;
}

void ctrl_init(ctrl_t *c, const pack_t *pack, const ctrl_settings_t *saved, int n_boxes, double now,
               double slow_start_s, double deadman_silence_s, double deadman_ramp_down_s, double amps_cap,
               double monophasic_asymmetry, uint64_t seed) {
    memset(c, 0, sizeof *c);
    foc312_init(&c->foc, seed);
    c->foc.output_on = true;
    c->foc.monophasic_asymmetry = monophasic_asymmetry < 1.0 ? 1.0 : (monophasic_asymmetry > 4.0 ? 4.0 : monophasic_asymmetry);
    safety_init(&c->saf, now, slow_start_s, deadman_silence_s, deadman_ramp_down_s, amps_cap);
    c->pack = pack;
    c->n_boxes = n_boxes < 1 ? 1 : n_boxes;
    if (pack) pack_install_builtins(pack, &c->foc.et);
    if (saved) c->set = *saved; else ctrl_default_settings(&c->set);
    /* a damaged or stale settings file must not produce anything strange */
    for (int i = 0; i < 2; i++)
        if (!route_valid(c->set.wire_route[i])) c->set.wire_route[i] = i ? 34 : 12;
    if (!shape_valid(c->set.shape) || c->set.shape == SHAPE_SQUARE || c->set.shape > 48) c->set.shape = SHAPE_ROUNDED;
    if (c->set.box < 0 || c->set.box >= c->n_boxes) c->set.box = 0;
    if (!pack || c->set.pattern >= pack->count) c->set.pattern = -1;
    c->foc.et.skip_mode_ramp = c->set.skip_mode_ramp;
    foc312_set_pads(&c->foc, c->set.pads, &c->saf, now);
    foc312_set_shape(&c->foc, c->set.shape, &c->saf, now);
    apply_routing(c, now);
    c->ma_steps = 50;
    foc312_set_ma(&c->foc, 0.5);
    c->master_target = 100;                  /* nothing plays until a level is raised: levels start at 0 */
    c->master_applied = 100.0;
    zero_levels(c);
    c->last_t = NAN;
    c->stop_t = -1e9;
    c->screen = SCREEN_RUN;
    int want = c->set.pattern;
    c->set.pattern = -1;
    if (want >= 0) play_pattern(c, want);
    else status(c, pack && pack->count ? "press knob 1 to pick a pattern" : "no patterns loaded");
}

static void switch_box(ctrl_t *c, int box, double now);

static void move_cursor(ctrl_t *c, int by, double now) {
    int n = c->screen == SCREEN_PATTERNS ? (c->pack ? c->pack->count : 0) : OPT_COUNT;
    c->cursor = n ? clampi(c->cursor + by, 0, n - 1) : 0;
    c->nav_t = now;
}

void ctrl_knob(ctrl_t *c, ctrl_knob_t knob, int detents, double now) {
    if (!detents) return;
    switch (knob) {
    case KNOB_1:
        if (c->screen == SCREEN_RUN) {
            c->master_target = clampi(c->master_target + detents, 0, 100);
            if (c->master_target < c->master_applied) c->master_applied = c->master_target;      /* down: instant */
        } else {
            move_cursor(c, detents, now);
        }
        break;
    case KNOB_2:
        if (c->screen == SCREEN_RUN) {
            c->ma_steps = clampi(c->ma_steps + detents, 0, 100);
            foc312_set_ma(&c->foc, c->ma_steps / 100.0);
        } else if (c->screen == SCREEN_PATTERNS) {
            move_cursor(c, 10 * detents, now);
        } else {
            /* options: knob 2 is the pulse shape, whatever is highlighted (PlaStim: "make it change the wave / cycle
             * through them"), one shape per detent, round the list; the highlight moves to Shape to show it */
            int shape = c->set.shape;
            for (int n = detents < 0 ? -detents : detents; n > 0; n--) shape = step_shape(c, shape, detents > 0 ? 1 : -1, true);
            c->cursor = OPT_SHAPE;
            c->nav_t = now;
            if (shape != c->set.shape) { c->set.shape = shape; foc312_set_shape(&c->foc, shape, &c->saf, now); }
        }
        break;
    case KNOB_3:
    case KNOB_4: {
        int p = knob == KNOB_3 ? 0 : 1;
        if (c->screen == SCREEN_OPTIONS) {         /* this position's wires, every pair both ways */
            int r = position_route(c, p), k = 0;
            for (int i = 0; i < 2 * CTRL_N_ROUTES; i++)
                if (DIRECTED[i] == r) k = i;
            int steps = (detents < 0 ? -detents : detents) % (2 * CTRL_N_ROUTES);
            k = (k + (detents > 0 ? steps : 2 * CTRL_N_ROUTES - steps)) % (2 * CTRL_N_ROUTES);
            c->set.wire_route[p] = DIRECTED[k & ~1];
            c->set.reversed[p] = (k & 1) != 0;
            c->cursor = p ? OPT_WIRES_2 : OPT_WIRES_1;
            c->nav_t = now;
            apply_routing(c, now);
            break;
        }
        if (c->screen != SCREEN_RUN) break;        /* the pattern list: no job */
        c->level_target[p] = clampi(c->level_target[p] + detents, 0, 100);
        if (c->level_target[p] < c->level_applied[p]) c->level_applied[p] = c->level_target[p];
        break;
    }
    }
}

static void activate_option(ctrl_t *c, int opt, double now) {
    switch (opt) {
    case OPT_POLARITY_1:
    case OPT_POLARITY_2:
        c->set.reversed[opt == OPT_POLARITY_1 ? 0 : 1] = !c->set.reversed[opt == OPT_POLARITY_1 ? 0 : 1];
        apply_routing(c, now);
        break;
    case OPT_WIRES_1:
    case OPT_WIRES_2: {
        int p = opt == OPT_WIRES_1 ? 0 : 1, k = 0;
        for (int i = 0; i < CTRL_N_ROUTES; i++)
            if (ROUTES[i] == c->set.wire_route[p]) k = i;
        c->set.wire_route[p] = ROUTES[(k + 1) % CTRL_N_ROUTES];
        apply_routing(c, now);
        break;
    }
    case OPT_SWAP:
        c->set.swapped = !c->set.swapped;       /* the patterns move; the levels stay with the positions */
        apply_routing(c, now);
        break;
    case OPT_PAD_1: case OPT_PAD_2: case OPT_PAD_3: case OPT_PAD_4:
        c->set.pads[opt - OPT_PAD_1] = !c->set.pads[opt - OPT_PAD_1];
        foc312_set_pads(&c->foc, c->set.pads, &c->saf, now);
        break;
    case OPT_SHAPE:
        c->set.shape = step_shape(c, c->set.shape, 1, true);
        foc312_set_shape(&c->foc, c->set.shape, &c->saf, now);
        break;
    case OPT_SKIP_RAMP:
        c->set.skip_mode_ramp = !c->set.skip_mode_ramp;
        c->foc.et.skip_mode_ramp = c->set.skip_mode_ramp;
        break;
    case OPT_BOX:
        switch_box(c, (c->set.box + 1) % c->n_boxes, now);
        break;
    case OPT_BACK:
    default:
        c->screen = SCREEN_RUN;
        break;
    }
}

static void switch_box(ctrl_t *c, int box, double now) {
    if (box == c->set.box) return;
    if (ctrl_armed(c)) {                        /* never switch boxes under a running output */
        safety_disarm(&c->saf);
        c->stop_t = now;
    }
    c->set.box = box;
    status(c, "switching box");
}

void ctrl_push_knob1(ctrl_t *c, double now) {
    c->nav_t = now;
    if (c->screen == SCREEN_RUN) {
        if (!c->pack || !c->pack->count) { status(c, "no patterns loaded"); return; }
        c->screen = SCREEN_PATTERNS;
        c->cursor = c->set.pattern >= 0 ? c->set.pattern : 0;
    } else if (c->screen == SCREEN_PATTERNS) {
        play_pattern(c, c->cursor);
        c->screen = SCREEN_RUN;
    } else {
        activate_option(c, c->cursor, now);
    }
}

void ctrl_push_knob4(ctrl_t *c, double now) {
    c->nav_t = now;
    if (c->screen == SCREEN_RUN) {
        c->screen = SCREEN_OPTIONS;
        c->cursor = 0;
    } else {
        c->screen = SCREEN_RUN;
    }
}

bool ctrl_armed(const ctrl_t *c) {
    return c->saf.armed;
}

void ctrl_button(ctrl_t *c, double now) {
    if (ctrl_armed(c)) {                        /* STOP always wins */
        safety_disarm(&c->saf);
        c->stop_t = now;
        status(c, "stopped");
        return;
    }
    if (now - c->stop_t < CTRL_RESTART_BLOCK_S) return;   /* a quick double press never turns it back on */
    if (!c->link_running) { status(c, "no box connected"); return; }
    if (c->set.pattern < 0) { status(c, "pick a pattern first (knob 1)"); return; }
    safety_arm(&c->saf);                        /* master slow-starts from 0 */
    status(c, "running");
}

void ctrl_set_box_fork(ctrl_t *c, int fork_version, double now) {
    (void)now;
    c->saf.fork_version = fork_version;
}

void ctrl_link(ctrl_t *c, bool running, const char *fault, double now) {
    c->link_running = running && !(fault && fault[0]);
    if (fault && fault[0]) {
        if (ctrl_armed(c)) c->stop_t = now;
        safety_disarm(&c->saf);
        zero_levels(c);                         /* back stopped with the levels at 0 */
        status(c, fault);
    }
}

void ctrl_tick(ctrl_t *c, double now, safety_values_t *out) {
    double dt = isnan(c->last_t) ? 0.0 : now - c->last_t;
    dt = dt < 0.0 ? 0.0 : (dt > 0.25 ? 0.25 : dt);
    c->last_t = now;
    double budget = CTRL_RISE_STEPS_PER_S * dt;
    for (int p = 0; p < 2; p++) {
        if (c->level_applied[p] < c->level_target[p]) {
            double v = c->level_applied[p] + budget;
            c->level_applied[p] = v < c->level_target[p] ? v : c->level_target[p];
        } else {
            c->level_applied[p] = c->level_target[p];
        }
    }
    if (c->master_applied < c->master_target) {
        double v = c->master_applied + budget;
        c->master_applied = v < c->master_target ? v : c->master_target;
    } else {
        c->master_applied = c->master_target;
    }
    double l0 = c->level_applied[0] / 100.0, l1 = c->level_applied[1] / 100.0;
    foc312_set_levels(&c->foc, c->set.swapped ? l1 : l0, c->set.swapped ? l0 : l1);
    safety_set_master(&c->saf, c->master_applied / 100.0, false, now);
    safety_renew_lease(&c->saf, now);          /* the UI loop is alive; a stalled loop stops sending and the box stops */
    if (c->screen != SCREEN_RUN && now - c->nav_t > CTRL_NAV_TIMEOUT_S) c->screen = SCREEN_RUN;
    foc312_advance(&c->foc, now, &c->saf);
    safety_tick(&c->saf, now);
    safety_values(&c->saf, now, out);
}

int ctrl_level_percent(const ctrl_t *c, int position) {
    return (int)(c->level_applied[position & 1] + 0.5);
}

int ctrl_master_percent(const ctrl_t *c) {
    return (int)(c->master_applied + 0.5);
}

const char *ctrl_option_text(const ctrl_t *c, int option, char *buf, int cap) {
    const ctrl_settings_t *s = &c->set;
    switch (option) {
    case OPT_POLARITY_1: case OPT_POLARITY_2:
        snprintf(buf, (size_t)cap, "Polarity %d: %s", option == OPT_POLARITY_1 ? 1 : 2,
                 s->reversed[option == OPT_POLARITY_1 ? 0 : 1] ? "reversed" : "normal");
        break;
    case OPT_WIRES_1: case OPT_WIRES_2: {
        int r = position_route(c, option == OPT_WIRES_1 ? 0 : 1);
        snprintf(buf, (size_t)cap, "Wires %d: %d-%d", option == OPT_WIRES_1 ? 1 : 2, r / 10, r % 10);
        break;
    }
    case OPT_SWAP: snprintf(buf, (size_t)cap, "Swap channels: %s", s->swapped ? "on" : "off"); break;
    case OPT_PAD_1: case OPT_PAD_2: case OPT_PAD_3: case OPT_PAD_4:
        snprintf(buf, (size_t)cap, "Pad %d: %s", option - OPT_PAD_1 + 1, s->pads[option - OPT_PAD_1] ? "connected" : "off");
        break;
    case OPT_SHAPE:
        if (s->shape >= SHAPE_TAPER_0) snprintf(buf, (size_t)cap, "Shape: taper %d%%", (s->shape - SHAPE_TAPER_0) * 10);
        else snprintf(buf, (size_t)cap, "Shape: %s", s->shape == SHAPE_SOFT ? "soft square"
                                                     : s->shape == SHAPE_TRIANGLE ? "triangle" : "rounded");
        if (shape_min_fork(s->shape) > c->saf.fork_version && shape_min_fork(s->shape) > 2)
            snprintf(buf + strlen(buf), (size_t)cap - strlen(buf), " (plays rounded: box < v7)");
        break;
    case OPT_SKIP_RAMP: snprintf(buf, (size_t)cap, "Skip mode ramp: %s", s->skip_mode_ramp ? "on" : "off"); break;
    case OPT_BOX: snprintf(buf, (size_t)cap, "Box: %d of %d", s->box + 1, c->n_boxes); break;
    default: snprintf(buf, (size_t)cap, "Back"); break;
    }
    return buf;
}
