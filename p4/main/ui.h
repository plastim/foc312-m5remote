// The main screen of the P4 touch remote (layout agreed with PlaStim 2026-09-29, mockups in notes/p4-remote.md).
// Step 1: the screen alone, on demo data; the core (patterns, safety, box link) is wired in next.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define UI_HIST 240                  // points in each output graph (about the last 10 s)

typedef struct {
    bool running;
    int level[2];                    // 0..100, channel A / B, as it is now (rises at most 10 a second)
    int level_target[2];             // where the user asked for
    int master;                      // 0..100, share of the box's own knob, as it is now
    int master_target;
    int ma;                          // 0..100, Multi Adjust
    int box_knob;                    // 0..100, the box's volume knob as it reports it
    int ma_measured[2];              // mA, per channel
    int route[2];                    // e.g. 12 = pads 1 -> 2 (first digit leads)
    int shape;                       // 0 triangle, 1 rounded, 2 taper, 3 soft square
    int taper_pct;
    const char *pattern, *pattern_group;
    int pattern_index, pattern_count;
    const char *box_name, *fw;
    int link_ms, box_batt;
    const char *ap_ssid, *ap_pass;   // the remote's own network
    int ap_channel;
    bool box_joined;
    // graph history per channel: intensity 0..1, rate Hz, width us (newest at [head-1])
    float hist_int[2][UI_HIST], hist_rate[2][UI_HIST], hist_width[2][UI_HIST];
    int hist_head;
    float rate_now[2], width_now[2], int_now[2];
} ui_state_t;

// what the user asked for, drained by the controller
typedef enum { UI_EV_NONE, UI_EV_STOP, UI_EV_START, UI_EV_LEVEL, UI_EV_MASTER, UI_EV_MA, UI_EV_FLIP, UI_EV_SAME_PADS, UI_EV_ROUTE,
               UI_EV_SHAPE, UI_EV_PATTERN_PREV, UI_EV_PATTERN_NEXT, UI_EV_PATTERN_LIST, UI_EV_SETTINGS } ui_ev_kind_t;
typedef struct { ui_ev_kind_t kind; int ch; int value; } ui_event_t;

void ui_create(ui_state_t *st);                  // under the display lock
void ui_refresh(const ui_state_t *st);           // under the display lock; call ~20 times a second
bool ui_poll_event(ui_event_t *ev);              // thread-safe
void ui_note(const char *text);                  // a short line in the status area (under the display lock)
void ui_debug_layout(void);                     // log the display size and the columns' positions
