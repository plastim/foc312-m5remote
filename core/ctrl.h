/* The M5 remote's controller: knobs, button and screens -> foc312 + safety, with the remote's own rules
 * (notes/m5-remote.md). Pure logic, no hardware: the app feeds input events and time, ctrl_tick() returns the values
 * for the box link. Tested on the host (tests/test_remote_ctrl.py).
 *
 * Rules:
 *  - STOP always wins: the MX button while armed = instant zero; it never toggles back on. Start (arm, slow start)
 *    needs the link RUNNING, no fault, and CTRL_RESTART_BLOCK_S since the last stop.
 *  - Levels and master rise at most CTRL_RISE_STEPS_PER_S steps a second however fast a knob spins; down is instant.
 *  - Levels stay with the wires: knob 2 sets the wires in position 1, knob 3 position 2; a channel swap moves the
 *    patterns between the positions, not the levels.
 *  - A fault (trip, lost link) disarms and zeroes both levels; levels are 0 at boot.
 *  - The knobs (PlaStim, 2026-09-29). Run screen: 1 master (press: patterns), 2 level A, 3 level B, 4 MA (press:
 *    options). In the menus no knob touches the output (master, MA and levels hold; STOP always works): patterns -
 *    1 scrolls (press picks), 2 jumps ten; options - 1 moves the highlight (press toggles / steps it), 2 and 3 step
 *    position 1's / 2's wires through every pair in both directions, 4 cycles the pulse shape. 4's press: back.
 */
#ifndef CTRL_H
#define CTRL_H

#include <stdbool.h>
#include <stdint.h>

#include "foc312.h"
#include "pack.h"
#include "safety.h"

#define CTRL_RISE_STEPS_PER_S 10.0
#define CTRL_RESTART_BLOCK_S 1.0
#define CTRL_NAV_TIMEOUT_S 20.0       /* pattern / options screens fall back to the run screen */
#define CTRL_N_ROUTES 6

typedef enum { SCREEN_RUN = 0, SCREEN_PATTERNS, SCREEN_OPTIONS } ctrl_screen_t;

/* in the order the options screen steps through them: the wiring picture first, then the list */
typedef enum {
    OPT_WIRES_1 = 0, OPT_POLARITY_1, OPT_WIRES_2, OPT_POLARITY_2, OPT_PAD_1, OPT_PAD_2, OPT_PAD_3, OPT_PAD_4,
    OPT_SWAP, OPT_SHAPE, OPT_SKIP_RAMP, OPT_BOX, OPT_BACK, OPT_COUNT
} ctrl_option_t;

typedef enum { KNOB_1 = 0, KNOB_2, KNOB_3, KNOB_4 } ctrl_knob_t;   /* as printed on the remote */

/* what the app saves and restores (never the levels) */
typedef struct {
    int pattern;                  /* pack entry index, -1 = none */
    int wire_route[2];            /* the wire pairs in positions 1 and 2, e.g. 12 / 34 */
    bool reversed[2];             /* polarity per position (route digits swapped) */
    bool swapped;                 /* channel A's pattern on position 2 */
    bool pads[4];
    int shape;
    bool skip_mode_ramp;
    int box;
} ctrl_settings_t;

typedef struct {
    foc312_t foc;
    safety_t saf;
    const pack_t *pack;
    ctrl_settings_t set;
    ctrl_screen_t screen;
    int cursor;                   /* pattern index or option index on those screens */
    double nav_t;                 /* last navigation input */
    int n_boxes;
    /* knob targets (steps 0..100) and what is applied after the rise limit */
    int ma_steps, master_target, level_target[2];
    double master_applied, level_applied[2];
    double last_t;                /* NAN before the first tick */
    double stop_t;                /* last stop, for the restart block */
    bool link_running;
    char status[96];              /* the latest thing the user should read (why a start was refused, faults) */
} ctrl_t;

void ctrl_init(ctrl_t *c, const pack_t *pack, const ctrl_settings_t *saved, int n_boxes, double now,
               double slow_start_s, double deadman_silence_s, double deadman_ramp_down_s, double amps_cap,
               double monophasic_asymmetry, uint64_t seed);
void ctrl_default_settings(ctrl_settings_t *s);
void ctrl_knob(ctrl_t *c, ctrl_knob_t knob, int detents, double now);
void ctrl_push_knob1(ctrl_t *c, double now);
void ctrl_push_knob4(ctrl_t *c, double now);
void ctrl_button(ctrl_t *c, double now);             /* MX press */
/* the box link's state: running (ready for output) or not; a fault message stops everything */
void ctrl_link(ctrl_t *c, bool running, const char *fault, double now);
/* the connected box's fork firmware version (0 = unknown): the shape choices depend on it (v7: triangle, taper) */
void ctrl_set_box_fork(ctrl_t *c, int fork_version, double now);
/* advance to `now`; fills the values to send (only meaningful while the link runs) */
void ctrl_tick(ctrl_t *c, double now, safety_values_t *out);
bool ctrl_armed(const ctrl_t *c);
int ctrl_level_percent(const ctrl_t *c, int position);   /* applied level of wire position 0/1 */
int ctrl_master_percent(const ctrl_t *c);
const char *ctrl_option_text(const ctrl_t *c, int option, char *buf, int cap);

#endif
