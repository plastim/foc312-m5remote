/* foc312 fork path (see foc312.h), mirroring stimengine/et312/foc312.py. */
#include "foc312.h"

#include <math.h>
#include <string.h>

void foc312_init(foc312_t *f, uint64_t seed) {
    memset(f, 0, sizeof *f);
    et312_engine_init(&f->et, seed);
    f->levels[0] = f->levels[1] = 0.0;           /* knobs start at zero, like the box */
    et312_set_levels(&f->et, 0.0, 0.0);
    f->ma = 0.5;
    et312_set_ma(&f->et, f->ma);
    f->routes[0] = 12;
    f->routes[1] = 34;
    f->shape = SHAPE_ROUNDED;
    for (int i = 0; i < 4; i++) f->pads[i] = true;
    f->pad_gain[0] = f->pad_gain[1] = 1.0;
    f->gain_t = NAN;
    f->monophasic_asymmetry = 3.0;
    f->acc = 0.0;
    f->last = NAN;
    f->output_on = false;
    et312_frame(&f->et, &f->frame);
}

bool foc312_blocked(const foc312_t *f, int ch) {
    int r = f->routes[ch & 1];
    return !f->pads[r / 10 - 1] || !f->pads[r % 10 - 1];
}

static void update_pad_gain(foc312_t *f, double now) {     /* _update_pad_gain */
    double dt = isnan(f->gain_t) ? 0.0 : (now - f->gain_t > 0.0 ? now - f->gain_t : 0.0);
    f->gain_t = now;
    for (int ch = 0; ch < 2; ch++) {
        if (foc312_blocked(f, ch)) {
            f->pad_gain[ch] = 0.0;
        } else {
            double g = f->pad_gain[ch] + dt / FOC312_UNBLOCK_RAMP_S;
            f->pad_gain[ch] = g < 1.0 ? g : 1.0;
        }
    }
}

void foc312_channel_view(const foc312_t *f, int ch, foc312_channel_view_t *v) {   /* channel_view (fork fields) */
    const et312_channel_t *c = ch == 0 ? &f->frame.a : &f->frame.b;
    double eff = c->gate_on ? c->intensity : 0.0;
    double rate = c->pulse_rate_hz < FORK_FREQ_MIN ? FORK_FREQ_MIN : (c->pulse_rate_hz > FORK_FREQ_MAX ? FORK_FREQ_MAX : c->pulse_rate_hz);
    double width = c->pulse_width_us < FORK_WIDTH_MIN ? FORK_WIDTH_MIN : (c->pulse_width_us > FORK_WIDTH_MAX ? FORK_WIDTH_MAX : c->pulse_width_us);
    v->intensity = py_round(eff, 4);
    v->rate_hz = py_round(rate, 1);
    v->width_us = nearbyint(width);
    /* monophasic = the asymmetric pulse, its lead polarity carried by the route; alternating halves play symmetric */
    v->asymmetry = (c->biphasic || c->alternating) ? 1.0 : f->monophasic_asymmetry;
    v->route_sent = c->leading_polarity < 0 ? route_reverse(f->routes[ch]) : f->routes[ch];
    v->blocked = foc312_blocked(f, ch);
}

void foc312_write_outputs(foc312_t *f, safety_t *s, double now) {   /* _write_outputs, fork branch */
    if (!f->output_on) return;
    for (int i = 0; i < 2; i++) {
        foc312_channel_view_t v;
        foc312_channel_view(f, i, &v);
        if (v.blocked) f->pad_gain[i] = 0.0;   /* a route onto a missing pad is silent at once */
        double gain = v.blocked ? 0.0 : f->pad_gain[i];
        safety_set_channel(s, i, v.intensity * gain, v.rate_hz, v.width_us, v.asymmetry, v.route_sent, f->shape,
                           false, now);
    }
}

int foc312_advance(foc312_t *f, double now, safety_t *s) {   /* advance + _after_frame */
    double dt = isnan(f->last) ? 0.0 : now - f->last;
    f->last = now;
    double add = dt < FOC312_MAX_CATCHUP_S ? dt : FOC312_MAX_CATCHUP_S;
    f->acc += add > 0.0 ? add : 0.0;
    int n = (int)(f->acc * ET312_TICK_HZ);
    f->acc -= n / ET312_TICK_HZ;
    for (int i = 0; i < n; i++) et312_step(&f->et, &f->frame);
    if (n) {
        update_pad_gain(f, now);
        foc312_write_outputs(f, s, now);
    }
    return n;
}

void foc312_set_levels(foc312_t *f, double a, double b) {
    f->levels[0] = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
    f->levels[1] = b < 0.0 ? 0.0 : (b > 1.0 ? 1.0 : b);
    et312_set_levels(&f->et, f->levels[0], f->levels[1]);
}

void foc312_set_ma(foc312_t *f, double v) {
    f->ma = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    et312_set_ma(&f->et, f->ma);
}

void foc312_set_route(foc312_t *f, int ch, int code, safety_t *s, double now) {
    if (!route_valid(code)) return;
    f->routes[ch & 1] = code;
    foc312_write_outputs(f, s, now);
}

void foc312_set_pads(foc312_t *f, const bool pads[4], safety_t *s, double now) {
    for (int i = 0; i < 4; i++) f->pads[i] = pads[i];
    for (int ch = 0; ch < 2; ch++)
        if (foc312_blocked(f, ch)) f->pad_gain[ch] = 0.0;
    foc312_write_outputs(f, s, now);
}

void foc312_set_shape(foc312_t *f, int shape, safety_t *s, double now) {
    if (!shape_valid(shape)) return;
    f->shape = shape;
    foc312_write_outputs(f, s, now);
}
