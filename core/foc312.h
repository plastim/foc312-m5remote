/* foc312's fork-firmware path, C99 port of stimengine/et312/foc312.py (Foc312Runner: advance, pads guard,
 * routes, pulse shape, channel_view, _write_outputs) feeding the safety stack (safety.h). The Python runner is the
 * reference (tests/test_remote_core.py). */
#ifndef FOC312_H
#define FOC312_H

#include <stdbool.h>

#include "et312.h"
#include "safety.h"

#define FOC312_MAX_CATCHUP_S 0.25
#define FOC312_UNBLOCK_RAMP_S 1.0

typedef struct {
    et312_engine_t et;
    et312_frame_t frame;
    double levels[2];            /* the level knobs, per channel */
    double ma;
    int routes[2];               /* the user's routes (the ET-312 mode's polarity bit may reverse what is sent) */
    int shape;                   /* both channels */
    bool pads[4];                /* electrodes with a pad on them */
    double pad_gain[2];          /* 0 while blocked, ramps back to 1 over FOC312_UNBLOCK_RAMP_S */
    double gain_t;               /* NAN until the first frame */
    double monophasic_asymmetry; /* return/lead width for the ET-312's monophasic pulses (config; 1..4) */
    double acc, last;            /* tick accumulator; last = NAN until the first advance */
    bool output_on;              /* writing to the safety stack (the page's "Fork fw" output) */
} foc312_t;

typedef struct {
    double intensity;            /* effective x pad gain */
    double rate_hz, width_us, asymmetry;
    int route_sent;
    bool blocked;
} foc312_channel_view_t;

void foc312_init(foc312_t *f, uint64_t seed);
/* run the ET-312 ticks due at `now`; after a frame, pad gain + write both channels into `s` */
int foc312_advance(foc312_t *f, double now, safety_t *s);
void foc312_set_levels(foc312_t *f, double a, double b);
void foc312_set_ma(foc312_t *f, double v);
/* these write the outputs at once, as the Python runner does */
void foc312_set_route(foc312_t *f, int ch, int code, safety_t *s, double now);
void foc312_set_pads(foc312_t *f, const bool pads[4], safety_t *s, double now);
void foc312_set_shape(foc312_t *f, int shape, safety_t *s, double now);
bool foc312_blocked(const foc312_t *f, int ch);
void foc312_channel_view(const foc312_t *f, int ch, foc312_channel_view_t *v);
void foc312_write_outputs(foc312_t *f, safety_t *s, double now);

#endif
