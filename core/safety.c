/* Safety stack + fork output values (see safety.h). Mirrors stimengine/engine.py; function comments name the
 * Python method each part follows. */
#include "safety.h"

#include <math.h>

static double clip01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static double clipr(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

double py_round(double x, int ndigits) {
    /* Python rounds the exact binary value to the nearest decimal (ties to even); scaling first can differ only
     * when x * 10^n lands exactly on a .5 it did not have, far below every tolerance these values feed */
    double p = pow(10.0, ndigits);
    return nearbyint(x * p) / p;
}

bool route_valid(int code) {
    int x = code / 10, y = code % 10;
    return code >= 11 && code <= 44 && x >= 1 && x <= 4 && y >= 1 && y <= 4 && x != y;
}

int route_reverse(int code) {
    return (code % 10) * 10 + code / 10;
}

/* device/fork.phase_charge: charge of one unit-peak phase in 20 us samples */
static double phase_charge(int shape, double width_us) {
    double w = width_us / 20.0;
    if (shape == SHAPE_SQUARE) return w;
    if (shape == SHAPE_SOFT) return w - (1.0 < w / 2 ? 1.0 : w / 2);
    if (shape == SHAPE_TRIANGLE) return w / 2;
    if (shape >= SHAPE_TAPER_0) {
        double r = (1.0 - (shape - SHAPE_TAPER_0) / 10.0) * w / 2;
        return 4.0 * r / 3.141592653589793 + (w - 2 * r);
    }
    return 2.0 * w / 3.141592653589793;
}

bool shape_valid(int shape) {
    return (shape >= SHAPE_ROUNDED && shape <= SHAPE_TRIANGLE) || (shape >= SHAPE_TAPER_0 && shape <= SHAPE_TAPER_1);
}

int shape_min_fork(int shape) {
    return (shape == SHAPE_TRIANGLE || shape >= SHAPE_TAPER_0) ? 7 : 2;
}

double shape_axis_value(int shape) {
    return shape < SHAPE_TAPER_0 ? (double)shape : shape / 10.0;
}

double shape_charge_factor(int shape, double width_us) {
    return phase_charge(SHAPE_ROUNDED, width_us) / phase_charge(shape, width_us);
}

void safety_init(safety_t *s, double now, double slow_start_s, double deadman_silence_s, double deadman_ramp_down_s,
                 double amps_cap) {
    static const int routes[2] = {12, 34};
    s->slow_start_s = slow_start_s;
    s->deadman_silence_s = deadman_silence_s;
    s->deadman_ramp_down_s = deadman_ramp_down_s;
    s->amps_cap = amps_cap > SAFETY_HARD_AMPS_CAP ? SAFETY_HARD_AMPS_CAP : amps_cap;
    s->armed = false;
    s->master_target = s->master_now = 0.0;
    s->api_target = 1.0;
    s->deadman_scale = 1.0;
    s->deadman_active = false;
    s->last_control = now;
    s->last_tick = 0.0;
    for (int i = 0; i < 2; i++) {
        safety_channel_t *c = &s->ch[i];
        c->intensity = 0.0;
        c->rate_hz = 50.0;
        c->width_us = 150.0;
        c->asymmetry = 1.0;
        c->route = routes[i];
        c->shape = c->shape_sent = SHAPE_ROUNDED;
        c->shape_t0 = -1e9;
        c->shape_pending = false;
        c->shape_in_t0 = -1e9;
    }
}

void safety_arm(safety_t *s) {          /* Engine.arm */
    s->armed = true;
    s->master_now = 0.0;
}

void safety_disarm(safety_t *s) {       /* Engine.disarm: immediate zero */
    s->armed = false;
    s->master_now = 0.0;
}

void safety_set_master(safety_t *s, double level, bool external, double now) {
    s->master_target = clip01(level);
    if (external) s->last_control = now;
}

void safety_set_api_volume(safety_t *s, double level, bool external, double now) {
    s->api_target = clip01(level);
    if (external) s->last_control = now;
}

void safety_renew_lease(safety_t *s, double now) {
    s->last_control = now;
}

void safety_set_channel(safety_t *s, int channel, double intensity, double rate_hz, double width_us,
                        double asymmetry, int route, int shape, bool external, double now) {   /* Engine.set_biphasic */
    safety_channel_t *c = &s->ch[channel & 1];
    if (route >= 0 && route_valid(route)) c->route = route;
    if (shape >= 0 && shape_valid(shape) && shape != c->shape) {
        c->shape = shape;
        c->shape_t0 = now;
        c->shape_pending = true;
    }
    if (!isnan(intensity)) c->intensity = clip01(intensity);
    if (!isnan(rate_hz)) c->rate_hz = clipr(rate_hz, FORK_FREQ_MIN, FORK_FREQ_MAX);
    if (!isnan(width_us)) c->width_us = clipr(width_us, FORK_WIDTH_MIN, FORK_WIDTH_MAX);
    if (!isnan(asymmetry)) c->asymmetry = clipr(asymmetry, FORK_ASYM_MIN, FORK_ASYM_MAX);
    if (external) s->last_control = now;
}

void safety_tick(safety_t *s, double now) {
    /* Engine._tick: dt, then _update_master and _update_deadman */
    double dt = s->last_tick == 0.0 ? SAFETY_TICK_S : clipr(now - s->last_tick, 0.0, 0.25);
    s->last_tick = now;
    if (!s->armed) {
        s->master_now = 0.0;
    } else if (s->master_now < s->master_target) {
        double step = s->slow_start_s > 0 ? dt / s->slow_start_s : 1.0;
        double v = s->master_now + step;
        s->master_now = v < s->master_target ? v : s->master_target;
    } else {
        s->master_now = s->master_target;     /* reductions are immediate */
    }
    double silent = now - s->last_control;
    if (silent > s->deadman_silence_s) {
        s->deadman_active = true;
        double step = s->deadman_ramp_down_s > 0 ? dt / s->deadman_ramp_down_s : 1.0;
        s->deadman_scale = s->deadman_scale - step > 0.0 ? s->deadman_scale - step : 0.0;
    } else if (s->deadman_active) {
        s->deadman_active = false;
        s->deadman_scale = 1.0;
    }
}

double safety_volume(const safety_t *s) {    /* Engine._volume_law */
    return clip01(s->master_now * s->api_target * s->deadman_scale);
}

static double shape_fade(safety_channel_t *c, double now) {   /* Engine._shape_fade */
    if (c->shape_pending) {
        double dt = now - c->shape_t0;
        if (dt < SAFETY_SHAPE_FADE_DOWN_S) {
            double v = 1.0 - dt / SAFETY_SHAPE_FADE_DOWN_S;
            return v > 0.0 ? v : 0.0;
        }
        c->shape_sent = c->shape;                /* switched on a tick that sends 0; the fade-in counts from here */
        c->shape_pending = false;
        c->shape_in_t0 = now;
        return 0.0;
    }
    double v = (now - c->shape_in_t0) / SAFETY_SHAPE_FADE_UP_S;
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

void safety_values(safety_t *s, double now, safety_values_t *out) {   /* Engine._biphasic_values (fork v2+) */
    double vol = safety_volume(s);
    double cap = s->amps_cap < SAFETY_HARD_AMPS_CAP ? s->amps_cap : SAFETY_HARD_AMPS_CAP;
    out->gap_us = 0.0;
    for (int i = 0; i < 2; i++) {
        safety_channel_t *c = &s->ch[i];
        double width_sent = py_round(c->width_us, 1);
        double fade = shape_fade(c, now);
        /* the remote only drives fork >= 2 (boxlink refuses older), so only the v7 shapes are gated */
        int need = shape_min_fork(c->shape_sent);
        int shape = (need > 2 && need > s->fork_version) ? SHAPE_ROUNDED : c->shape_sent;
        double factor = shape_charge_factor(shape, width_sent);
        double amps = c->intensity * vol * cap * factor;
        amps = amps < 0.0 ? 0.0 : amps;
        amps = (amps < cap ? amps : cap) * fade;          /* never above the cap: a clipped pulse is weaker */
        out->amps[i] = py_round(amps, 5);
        out->rate_hz[i] = py_round(c->rate_hz, 1);
        out->width_us[i] = width_sent;
        out->asymmetry[i] = py_round(c->asymmetry, 3);
        out->polarity[i] = 0.0;
        out->route[i] = route_valid(c->route) ? c->route : (i == 0 ? 12 : 34);
        out->shape[i] = shape;
    }
}
