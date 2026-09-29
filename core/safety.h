/* The engine's safety stack and fork-firmware output values, C99 port of the relevant parts of
 * stimengine/engine.py (Engine: arm/disarm, master slow start, deadman, volume law, pulse-shape fade,
 * _biphasic_values). The Python engine is the reference (tests/test_remote_core.py).
 *
 * Law: volume = master x api x deadman, clipped 0..1; slow start on arm (master rises over slow_start_s,
 * reductions immediate); deadman: no control input for deadman_silence_s -> ramp to 0 over deadman_ramp_down_s;
 * amps = intensity x volume x cap x shape charge factor, never above the cap (<= 0.2 A hard); a shape change fades
 * out, switches, fades in (only ever reduces). Times are seconds from any monotonic clock. Fork firmware v2+ only. */
#ifndef SAFETY_H
#define SAFETY_H

#include <stdbool.h>

#define SAFETY_HARD_AMPS_CAP 0.2
#define SAFETY_TICK_S (1.0 / 60.0)        /* the engine's own tick; values are computed per tick */
#define SAFETY_SHAPE_FADE_DOWN_S 0.02   /* stimengine SHAPE_FADE_DOWN_S / _UP_S */
#define SAFETY_SHAPE_FADE_UP_S 0.04

#define FORK_FREQ_MIN 1.0
#define FORK_FREQ_MAX 400.0
#define FORK_WIDTH_MIN 40.0
#define FORK_WIDTH_MAX 400.0
#define FORK_ASYM_MIN 1.0
#define FORK_ASYM_MAX 4.0

enum { SHAPE_ROUNDED = 0, SHAPE_SQUARE = 1, SHAPE_SOFT = 2, SHAPE_TRIANGLE = 3,
       SHAPE_TAPER_0 = 40, SHAPE_TAPER_1 = 50 };      /* device/fork.py ids: 40..50 = taper flat top 0..100 % */

typedef struct {
    double intensity;     /* 0..1 of the amps cap, before the volume law */
    double rate_hz, width_us, asymmetry;
    int route;            /* two-digit electrode code, e.g. 12 */
    int shape, shape_sent;
    double shape_t0;      /* when the shape last changed */
    bool shape_pending;   /* fading out on the old shape; the switch goes out on a tick that sends 0 */
    double shape_in_t0;   /* when the new shape was sent (its fade-in counts from there) */
} safety_channel_t;

typedef struct {
    /* config */
    double slow_start_s, deadman_silence_s, deadman_ramp_down_s, amps_cap;
    /* state */
    bool armed;
    double master_target, master_now, api_target;
    double deadman_scale;
    bool deadman_active;
    double last_control, last_tick;   /* last_tick 0 = never ticked */
    safety_channel_t ch[2];
    int fork_version;                 /* the box's; a v7 shape on an older box plays rounded (Engine._biphasic_values) */
} safety_t;

/* what goes to the box per channel (axes 60..74), plus the shared inter-phase gap */
typedef struct {
    double amps[2], rate_hz[2], width_us[2], asymmetry[2];
    int route[2], shape[2];
    double polarity[2];   /* always 0: a lead swap is a route digit swap */
    double gap_us;
} safety_values_t;

/* amps_cap is clipped to SAFETY_HARD_AMPS_CAP */
void safety_init(safety_t *s, double now, double slow_start_s, double deadman_silence_s, double deadman_ramp_down_s,
                 double amps_cap);
void safety_arm(safety_t *s);
void safety_disarm(safety_t *s);
void safety_set_master(safety_t *s, double level, bool external, double now);
void safety_set_api_volume(safety_t *s, double level, bool external, double now);
void safety_renew_lease(safety_t *s, double now);    /* the UI is alive: keeps the deadman off */
/* per-channel targets; NAN leaves a field unchanged, route/shape < 0 leave them unchanged */
void safety_set_channel(safety_t *s, int channel, double intensity, double rate_hz, double width_us,
                        double asymmetry, int route, int shape, bool external, double now);
void safety_tick(safety_t *s, double now);            /* master slow start + deadman */
double safety_volume(const safety_t *s);
void safety_values(safety_t *s, double now, safety_values_t *out);

/* helpers shared with foc312.c */
bool route_valid(int code);
int route_reverse(int code);
double shape_charge_factor(int shape, double width_us);
bool shape_valid(int shape);              /* device/fork.SHAPES */
int shape_min_fork(int shape);            /* device/fork.shape_min_fork */
double shape_axis_value(int shape);       /* device/fork.shape_axis_value: 0..3, a taper id as 4.0..5.0 */
double py_round(double x, int ndigits);   /* Python round(x, n) except at exact-decimal-tie edge cases */

#endif
